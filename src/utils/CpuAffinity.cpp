#include "utils/CpuAffinity.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sched.h>
#include <sstream>
#include <string>
#include <sys/syscall.h>
#include <unistd.h>
#include <unordered_map>

#include "utils/Log.h"

// MPOL_BIND constant — defined in <linux/mempolicy.h> but that header is
// often missing in user-space toolchains. We invoke mbind via syscall and
// pass the policy number directly to avoid pulling kernel headers.
#ifndef MPOL_BIND
#define MPOL_BIND 2
#endif

namespace numa {

namespace {

constexpr const char* kOnlineFile = "/sys/devices/system/node/online";

std::atomic<int> g_numa_state{-1};   // -1 = uninitialized, 0 = no, 1 = yes
std::once_flag   g_numa_warn_once;

bool detectNumaAvailable() {
    std::ifstream f(kOnlineFile);
    if (!f.good()) return false;
    std::string s;
    std::getline(f, s);
    // "0" alone means single-node (effectively no NUMA); "0-N" or "0,1,..." means real NUMA.
    if (s.empty()) return false;
    if (s == "0") return false;
    return true;
}

// Parse Linux cpulist format ("0-3,8,12-15") into cpu_set_t.
bool parseCpuList(const std::string& s, cpu_set_t& out) {
    CPU_ZERO(&out);
    if (s.empty()) return false;
    bool any = false;
    std::stringstream ss(s);
    std::string segment;
    while (std::getline(ss, segment, ',')) {
        if (segment.empty()) continue;
        const auto dash = segment.find('-');
        try {
            if (dash == std::string::npos) {
                int cpu = std::stoi(segment);
                if (cpu >= 0 && cpu < CPU_SETSIZE) {
                    CPU_SET(cpu, &out);
                    any = true;
                }
            } else {
                int lo = std::stoi(segment.substr(0, dash));
                int hi = std::stoi(segment.substr(dash + 1));
                if (lo > hi) std::swap(lo, hi);
                for (int c = lo; c <= hi; ++c) {
                    if (c >= 0 && c < CPU_SETSIZE) {
                        CPU_SET(c, &out);
                        any = true;
                    }
                }
            }
        } catch (...) {
            return false;
        }
    }
    return any;
}

bool readNodeCpuList(int node, cpu_set_t& out) {
    char path[128];
    std::snprintf(path, sizeof(path),
                  "/sys/devices/system/node/node%d/cpulist", node);
    std::ifstream f(path);
    if (!f.good()) return false;
    std::string s;
    std::getline(f, s);
    return parseCpuList(s, out);
}

bool intersectWithAllowed(cpu_set_t& set) {
    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    if (sched_getaffinity(0, sizeof(allowed), &allowed) != 0) {
        // No allowed-set info — keep the parsed set as-is.
        return CPU_COUNT(&set) > 0;
    }
    cpu_set_t result;
    CPU_ZERO(&result);
    for (int c = 0; c < CPU_SETSIZE; ++c) {
        if (CPU_ISSET(c, &set) && CPU_ISSET(c, &allowed)) {
            CPU_SET(c, &result);
        }
    }
    set = result;
    return CPU_COUNT(&set) > 0;
}

// Cache parsed-and-intersected masks per node. Entries are immutable
// after insert; mutex is only held during insert.
struct MaskCache {
    std::mutex mu;
    std::unordered_map<int, cpu_set_t> map;
};
MaskCache& cache() {
    static MaskCache c;
    return c;
}

// Returns true and fills out_mask if a usable mask exists for node.
bool resolveMask(int node, cpu_set_t& out_mask) {
    auto& c = cache();
    {
        std::lock_guard lk(c.mu);
        auto it = c.map.find(node);
        if (it != c.map.end()) {
            if (CPU_COUNT(&it->second) == 0) return false;
            out_mask = it->second;
            return true;
        }
    }

    cpu_set_t mask;
    if (!readNodeCpuList(node, mask)) {
        LOG_WARN("numa: cannot read cpulist for node {}", node);
        std::lock_guard lk(c.mu);
        cpu_set_t empty;
        CPU_ZERO(&empty);
        c.map.emplace(node, empty);
        return false;
    }
    if (!intersectWithAllowed(mask)) {
        LOG_WARN("numa: node {} cpu set has no overlap with process allowed-set",
                 node);
        std::lock_guard lk(c.mu);
        c.map.emplace(node, mask);
        return false;
    }

    {
        std::lock_guard lk(c.mu);
        c.map.emplace(node, mask);
    }
    out_mask = mask;
    return true;
}

void warnNumaUnavailableOnce() {
    std::call_once(g_numa_warn_once, [] {
        LOG_INFO("numa: NUMA not available on this host (single-node or /sys "
                 "restricted) — affinity calls will be no-ops");
    });
}

}  // namespace

bool numaAvailable() {
    int s = g_numa_state.load(std::memory_order_acquire);
    if (s < 0) {
        s = detectNumaAvailable() ? 1 : 0;
        g_numa_state.store(s, std::memory_order_release);
    }
    return s == 1;
}

bool bindThreadToNode(pthread_t handle, int node) {
    if (node < 0) return true;
    if (!numaAvailable()) {
        warnNumaUnavailableOnce();
        return true;
    }
    cpu_set_t mask;
    if (!resolveMask(node, mask)) return false;
    const int rc = pthread_setaffinity_np(handle, sizeof(mask), &mask);
    if (rc != 0) {
        LOG_WARN("numa: pthread_setaffinity_np(node={}) failed: {}",
                 node, std::strerror(rc));
        return false;
    }
    return true;
}

bool bindCurrentThreadToNode(int node) {
    return bindThreadToNode(pthread_self(), node);
}

bool bindMemoryToNode(void* addr, std::size_t length, int node) {
    if (node < 0 || !addr || length == 0) return true;
    if (!numaAvailable()) {
        warnNumaUnavailableOnce();
        return true;
    }
    // Build a node-mask bitmap covering [0, max_node]. Glibc's mbind takes
    // a ulong* nodemask plus maxnode (highest valid node index + 1).
    constexpr int kMaxNode = 1024;
    if (node >= kMaxNode) {
        LOG_WARN("numa: bindMemoryToNode node={} exceeds kMaxNode={}",
                 node, kMaxNode);
        return false;
    }
    constexpr std::size_t kWords = kMaxNode / (sizeof(unsigned long) * 8);
    unsigned long nodemask[kWords] = {};
    nodemask[node / (sizeof(unsigned long) * 8)] =
        1UL << (node % (sizeof(unsigned long) * 8));

    // Round addr down and length up to page boundary — mbind works at page
    // granularity, partial pages would be ignored or rejected.
    const long page_sz = sysconf(_SC_PAGESIZE);
    auto      base    = reinterpret_cast<std::uintptr_t>(addr);
    const auto aligned = base & ~static_cast<std::uintptr_t>(page_sz - 1);
    const auto end     = (base + length + page_sz - 1) &
                         ~static_cast<std::uintptr_t>(page_sz - 1);
    void* a_addr = reinterpret_cast<void*>(aligned);
    const std::size_t a_len = end - aligned;

    long rc = ::syscall(SYS_mbind, a_addr, a_len, MPOL_BIND,
                        nodemask, kMaxNode, 0u /* flags */);
    if (rc != 0) {
        LOG_WARN("numa: mbind(node={}, len={}) failed: {}",
                 node, a_len, std::strerror(errno));
        return false;
    }
    return true;
}

namespace detail {
void runOnNodeImpl(int /*node*/, void (* /*fn*/)(void*), void* /*ctx*/) {
    // Reserved for non-template C-ABI users; current callers all use the
    // template form.
}
}  // namespace detail

}  // namespace numa
