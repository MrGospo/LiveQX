#include "metrics/HostMetrics.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_set>

#if defined(__linux__)
#include <sys/statvfs.h>
#include <sys/sysinfo.h>
#endif

namespace liveqx::metrics {

namespace {

std::int64_t nowUnixMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

#if defined(__linux__)

// Skip the sysfs/procfs zoo and network shares. statvfs() on cifs/nfs can
// block for seconds when the peer is down, and that would freeze the SSE
// sampler for every viewer.
bool isRealLocalFs(std::string_view fstype) {
    static const std::unordered_set<std::string_view> kVirtual = {
        "proc", "sysfs", "cgroup", "cgroup2", "devpts", "devtmpfs",
        "debugfs", "tracefs", "securityfs", "pstore", "autofs", "mqueue",
        "hugetlbfs", "fusectl", "binfmt_misc", "configfs", "bpf",
        "rpc_pipefs", "nfsd", "selinuxfs", "efivarfs", "ramfs",
        "tmpfs", "overlay", "squashfs",
    };
    static const std::unordered_set<std::string_view> kNetwork = {
        "cifs", "smb3", "nfs", "nfs4", "fuse.sshfs", "fuse.rclone",
    };
    if (kVirtual.count(fstype)) return false;
    if (kNetwork.count(fstype)) return false;
    // Any other fuse.* — unknown blocking behaviour, skip.
    if (fstype.rfind("fuse.", 0) == 0) return false;
    return true;
}

// The mountinfo format is stable since Linux 2.6.26. Fields are
// space-separated; the "optional" block ends with " - " (dash surrounded
// by spaces) after which fstype and source follow.
//   36 35 98:0 /mnt1 /mnt/parent rw,noatime - ext4 /dev/sda1 rw,errors=remount-ro
//                                            ^^^ separator
void readMountinfo(std::vector<HostFsSnapshot>& out) {
    std::FILE* f = std::fopen("/proc/self/mountinfo", "r");
    if (!f) return;
    char line[1024];
    // Some mounts appear multiple times (e.g. bind-mounts of the same fs).
    // Dedupe by mount point path so the UI doesn't show duplicates.
    std::unordered_set<std::string> seen;
    while (std::fgets(line, sizeof(line), f)) {
        // Locate " - " separator.
        char* sep = std::strstr(line, " - ");
        if (!sep) continue;
        // Before separator: skip 4 fields to reach the mount point (field 5).
        // Fields (1-indexed): mount_id parent_id major:minor root mount_point mount_opts opt_fields...
        std::string_view head(line, static_cast<std::size_t>(sep - line));
        // Split head by spaces up to the 5th field.
        std::size_t start = 0;
        std::string_view mount_point;
        int field = 0;
        for (std::size_t i = 0; i <= head.size(); ++i) {
            if (i == head.size() || head[i] == ' ') {
                if (i > start) {
                    ++field;
                    if (field == 5) {
                        mount_point = head.substr(start, i - start);
                        break;
                    }
                }
                start = i + 1;
            }
        }
        if (mount_point.empty()) continue;

        // After " - ": fstype  source  super_opts
        char* rest = sep + 3;
        char* space = std::strchr(rest, ' ');
        if (!space) continue;
        std::string_view fstype(rest, static_cast<std::size_t>(space - rest));

        if (!isRealLocalFs(fstype)) continue;
        std::string mp(mount_point);
        if (!seen.insert(mp).second) continue;

        struct statvfs vfs{};
        if (::statvfs(mp.c_str(), &vfs) != 0) continue;
        HostFsSnapshot s;
        s.mount       = std::move(mp);
        s.fstype.assign(fstype);
        s.total_bytes = static_cast<std::uint64_t>(vfs.f_blocks) *
                        static_cast<std::uint64_t>(vfs.f_frsize);
        // Use f_bavail (available to unprivileged users) rather than
        // f_bfree (which includes root-reserved blocks) — matches what
        // `df` reports, so admins see numbers that match their shell.
        s.free_bytes  = static_cast<std::uint64_t>(vfs.f_bavail) *
                        static_cast<std::uint64_t>(vfs.f_frsize);
        out.push_back(std::move(s));
    }
    std::fclose(f);
    // Deterministic order for the UI (and for testability).
    std::sort(out.begin(), out.end(),
              [](const HostFsSnapshot& a, const HostFsSnapshot& b) {
                  return a.mount < b.mount;
              });
}

void readProcStat(std::vector<HostCpuLine>& out) {
    std::FILE* f = std::fopen("/proc/stat", "r");
    if (!f) return;
    char line[512];
    while (std::fgets(line, sizeof(line), f)) {
        if (std::strncmp(line, "cpu", 3) != 0) break;  // "intr" etc. — done
        HostCpuLine c;
        char name[32] = {0};
        // guest / guest_nice already accounted for in user / nice on Linux ≥ 2.6.24
        int n = std::sscanf(line,
            "%31s %lu %lu %lu %lu %lu %lu %lu %lu",
            name, &c.user, &c.nice, &c.system, &c.idle,
            &c.iowait, &c.irq, &c.softirq, &c.steal);
        if (n >= 5) {
            c.name = name;
            out.push_back(std::move(c));
        }
    }
    std::fclose(f);
}

void readMeminfo(HostMemSnapshot& m) {
    std::FILE* f = std::fopen("/proc/meminfo", "r");
    if (!f) return;
    char line[256];
    auto grabKb = [](const char* line, const char* key, std::size_t klen,
                     std::uint64_t& dst) {
        if (std::strncmp(line, key, klen) == 0) {
            unsigned long kb = 0;
            if (std::sscanf(line + klen, "%lu", &kb) == 1) {
                dst = static_cast<std::uint64_t>(kb) * 1024ULL;
            }
        }
    };
    while (std::fgets(line, sizeof(line), f)) {
        grabKb(line, "MemTotal:",     9,  m.total_bytes);
        grabKb(line, "MemFree:",      8,  m.free_bytes);
        grabKb(line, "MemAvailable:", 13, m.available_bytes);
        grabKb(line, "Buffers:",      8,  m.buffers_bytes);
        grabKb(line, "Cached:",       7,  m.cached_bytes);
        grabKb(line, "SwapTotal:",    10, m.swap_total_bytes);
        grabKb(line, "SwapFree:",     9,  m.swap_free_bytes);
    }
    std::fclose(f);
}

void readLoadavg(HostSnapshot& s) {
    std::FILE* f = std::fopen("/proc/loadavg", "r");
    if (!f) return;
    if (std::fscanf(f, "%lf %lf %lf", &s.load1, &s.load5, &s.load15) != 3) {
        s.load1 = s.load5 = s.load15 = 0.0;
    }
    std::fclose(f);
}

void readUptime(HostSnapshot& s) {
    std::FILE* f = std::fopen("/proc/uptime", "r");
    if (!f) return;
    double up = 0.0;
    if (std::fscanf(f, "%lf", &up) == 1) {
        s.uptime_seconds = static_cast<std::int64_t>(up);
    }
    std::fclose(f);
}

// operstate is one of "up", "down", "unknown", "dormant", "lowerlayerdown".
// Treat everything except "up" as down for the "green dot / grey dot" UI.
bool readIfaceUp(const std::string& name) {
    std::string path = "/sys/class/net/" + name + "/operstate";
    std::FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return false;
    char buf[16] = {0};
    if (!std::fgets(buf, sizeof(buf), f)) { std::fclose(f); return false; }
    std::fclose(f);
    std::size_t n = std::strlen(buf);
    while (n && (buf[n-1] == '\n' || buf[n-1] == '\r' || buf[n-1] == ' ')) buf[--n] = '\0';
    return std::string_view(buf) == "up";
}

void readNetDev(std::vector<HostNicSnapshot>& out) {
    std::FILE* f = std::fopen("/proc/net/dev", "r");
    if (!f) return;
    char line[512];
    // Skip the two header lines.
    if (!std::fgets(line, sizeof(line), f)) { std::fclose(f); return; }
    if (!std::fgets(line, sizeof(line), f)) { std::fclose(f); return; }
    while (std::fgets(line, sizeof(line), f)) {
        char* colon = std::strchr(line, ':');
        if (!colon) continue;
        *colon = '\0';
        // Interface name: trim leading spaces.
        char* name = line;
        while (*name == ' ' || *name == '\t') ++name;

        HostNicSnapshot n;
        // Skip the loopback: not useful for a dashboard, adds noise.
        if (std::strcmp(name, "lo") == 0) continue;

        // rx: bytes packets errs drop fifo frame compressed multicast
        // tx: bytes packets errs drop fifo colls carrier compressed
        std::uint64_t rx_fifo, rx_frame, rx_compressed, rx_multicast;
        std::uint64_t tx_fifo, tx_colls, tx_carrier, tx_compressed;
        int m = std::sscanf(colon + 1,
            "%lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu",
            &n.rx_bytes, &n.rx_packets, &n.rx_errs, &n.rx_drop,
            &rx_fifo, &rx_frame, &rx_compressed, &rx_multicast,
            &n.tx_bytes, &n.tx_packets, &n.tx_errs, &n.tx_drop,
            &tx_fifo, &tx_colls, &tx_carrier, &tx_compressed);
        if (m < 12) continue;
        n.name  = name;
        n.is_up = readIfaceUp(n.name);
        out.push_back(std::move(n));
    }
    std::fclose(f);
    std::sort(out.begin(), out.end(),
              [](const HostNicSnapshot& a, const HostNicSnapshot& b) {
                  return a.name < b.name;
              });
}

// Filter loop / ram / dm-* aggregate devices so the UI shows just the
// physical drives the admin cares about. Kernel exposes partitions and
// whole-disks together; we keep only whole-disks by checking for
// /sys/block/<name> (partitions live under /sys/block/<parent>/<name>).
bool isPrimaryBlockDev(const std::string& name) {
    if (name.rfind("loop", 0) == 0)  return false;
    if (name.rfind("ram", 0)  == 0)  return false;
    std::string path = "/sys/block/" + name;
    // If /sys/block/<name> exists it's a whole-disk device.
    std::FILE* f = std::fopen((path + "/stat").c_str(), "r");
    if (!f) return false;
    std::fclose(f);
    return true;
}

void readDiskstats(std::vector<HostDiskIoSnapshot>& out) {
    std::FILE* f = std::fopen("/proc/diskstats", "r");
    if (!f) return;
    char line[512];
    while (std::fgets(line, sizeof(line), f)) {
        unsigned int major = 0, minor = 0;
        char name[64] = {0};
        std::uint64_t rd_ios, rd_merged, rd_sect, rd_ticks;
        std::uint64_t wr_ios, wr_merged, wr_sect, wr_ticks;
        int m = std::sscanf(line,
            " %u %u %63s %lu %lu %lu %lu %lu %lu %lu %lu",
            &major, &minor, name,
            &rd_ios, &rd_merged, &rd_sect, &rd_ticks,
            &wr_ios, &wr_merged, &wr_sect, &wr_ticks);
        if (m < 11) continue;
        std::string sname(name);
        if (!isPrimaryBlockDev(sname)) continue;
        HostDiskIoSnapshot d;
        d.name             = std::move(sname);
        d.reads_completed  = rd_ios;
        d.sectors_read     = rd_sect;
        d.writes_completed = wr_ios;
        d.sectors_written  = wr_sect;
        out.push_back(std::move(d));
    }
    std::fclose(f);
    std::sort(out.begin(), out.end(),
              [](const HostDiskIoSnapshot& a, const HostDiskIoSnapshot& b) {
                  return a.name < b.name;
              });
}

#endif  // __linux__

// ── Rate helpers ────────────────────────────────────────────────────────────
// Delta seconds between two snapshots. Guard against zero and clock skew
// (system clock jumps backward) by clamping the minimum to something
// finite so rate divisions don't blow up.
double dtSeconds(const HostSnapshot& prev, const HostSnapshot& curr) {
    const double dt = (curr.sampled_at_unix_ms - prev.sampled_at_unix_ms) / 1000.0;
    return dt > 0.001 ? dt : 0.001;
}

// CPU busy fraction over the interval, computed from jiffies.
// busy = user+nice+system+irq+softirq+steal; total = busy + idle+iowait.
// Returns 0..100. Returns 0 if there's no previous line with a matching name.
double cpuBusyPct(const HostCpuLine& prev, const HostCpuLine& curr) {
    const std::uint64_t p_busy = prev.user + prev.nice + prev.system +
                                 prev.irq + prev.softirq + prev.steal;
    const std::uint64_t c_busy = curr.user + curr.nice + curr.system +
                                 curr.irq + curr.softirq + curr.steal;
    const std::uint64_t p_idle = prev.idle + prev.iowait;
    const std::uint64_t c_idle = curr.idle + curr.iowait;
    const std::uint64_t p_total = p_busy + p_idle;
    const std::uint64_t c_total = c_busy + c_idle;
    if (c_total <= p_total) return 0.0;
    const double dt_busy  = static_cast<double>(c_busy  - p_busy);
    const double dt_total = static_cast<double>(c_total - p_total);
    return 100.0 * dt_busy / dt_total;
}

const HostCpuLine* findCpu(const std::vector<HostCpuLine>& v, const std::string& name) {
    for (const auto& c : v) if (c.name == name) return &c;
    return nullptr;
}
const HostNicSnapshot* findNic(const std::vector<HostNicSnapshot>& v, const std::string& name) {
    for (const auto& n : v) if (n.name == name) return &n;
    return nullptr;
}
const HostDiskIoSnapshot* findDisk(const std::vector<HostDiskIoSnapshot>& v,
                                    const std::string& name) {
    for (const auto& d : v) if (d.name == name) return &d;
    return nullptr;
}

nlohmann::json cpuLineJson(const HostCpuLine& c) {
    return {
        {"name",    c.name},
        {"user",    c.user},
        {"nice",    c.nice},
        {"system",  c.system},
        {"idle",    c.idle},
        {"iowait",  c.iowait},
        {"irq",     c.irq},
        {"softirq", c.softirq},
        {"steal",   c.steal},
    };
}

nlohmann::json nicJson(const HostNicSnapshot& n) {
    return {
        {"name",       n.name},
        {"is_up",      n.is_up},
        {"rx_bytes",   n.rx_bytes},
        {"rx_packets", n.rx_packets},
        {"rx_errs",    n.rx_errs},
        {"rx_drop",    n.rx_drop},
        {"tx_bytes",   n.tx_bytes},
        {"tx_packets", n.tx_packets},
        {"tx_errs",    n.tx_errs},
        {"tx_drop",    n.tx_drop},
    };
}

nlohmann::json fsJson(const HostFsSnapshot& f) {
    return {
        {"mount",       f.mount},
        {"fstype",      f.fstype},
        {"total_bytes", f.total_bytes},
        {"free_bytes",  f.free_bytes},
        {"used_bytes",  f.total_bytes > f.free_bytes ? f.total_bytes - f.free_bytes : 0},
    };
}

nlohmann::json diskJson(const HostDiskIoSnapshot& d) {
    return {
        {"name",             d.name},
        {"reads_completed",  d.reads_completed},
        {"sectors_read",     d.sectors_read},
        {"writes_completed", d.writes_completed},
        {"sectors_written",  d.sectors_written},
    };
}

}  // namespace

// ── Public API ──────────────────────────────────────────────────────────────

HostSnapshot HostMetricsReader::sample() {
    HostSnapshot s;
    s.sampled_at_unix_ms = nowUnixMs();
#if defined(__linux__)
    readProcStat(s.cpu);
    readMeminfo(s.mem);
    readLoadavg(s);
    readUptime(s);
    readNetDev(s.nics);
    readMountinfo(s.fs);
    readDiskstats(s.disks);
#endif
    return s;
}

nlohmann::json HostMetricsReader::toJson(const HostSnapshot& s) {
    nlohmann::json j;
    j["sampled_at_unix_ms"] = s.sampled_at_unix_ms;
    j["uptime_seconds"]     = s.uptime_seconds;
    j["load1"]              = s.load1;
    j["load5"]              = s.load5;
    j["load15"]             = s.load15;

    nlohmann::json cpuj = nlohmann::json::object();
    if (!s.cpu.empty()) {
        cpuj["aggregate"] = cpuLineJson(s.cpu.front());
        auto per = nlohmann::json::array();
        for (std::size_t i = 1; i < s.cpu.size(); ++i) per.push_back(cpuLineJson(s.cpu[i]));
        cpuj["per_core"] = std::move(per);
    } else {
        cpuj["aggregate"] = nullptr;
        cpuj["per_core"]  = nlohmann::json::array();
    }
    // Rates are unavailable in a single-snapshot view; the UI shows "—"
    // until the SSE stream delivers the second frame.
    cpuj["total_pct"] = nullptr;
    j["cpu"] = std::move(cpuj);

    j["mem"] = {
        {"total_bytes",      s.mem.total_bytes},
        {"free_bytes",       s.mem.free_bytes},
        {"available_bytes",  s.mem.available_bytes},
        {"buffers_bytes",    s.mem.buffers_bytes},
        {"cached_bytes",     s.mem.cached_bytes},
        {"used_bytes",       s.mem.total_bytes > s.mem.available_bytes
                                ? s.mem.total_bytes - s.mem.available_bytes : 0},
        {"swap_total_bytes", s.mem.swap_total_bytes},
        {"swap_free_bytes",  s.mem.swap_free_bytes},
        {"swap_used_bytes",  s.mem.swap_total_bytes > s.mem.swap_free_bytes
                                ? s.mem.swap_total_bytes - s.mem.swap_free_bytes : 0},
    };

    auto nicsJ = nlohmann::json::array();
    for (const auto& n : s.nics) {
        auto nj = nicJson(n);
        nj["rx_bps"] = nullptr;
        nj["tx_bps"] = nullptr;
        nicsJ.push_back(std::move(nj));
    }
    j["nics"] = std::move(nicsJ);

    auto fsJ = nlohmann::json::array();
    for (const auto& f : s.fs) fsJ.push_back(fsJson(f));
    j["fs"] = std::move(fsJ);

    auto diskJ = nlohmann::json::array();
    for (const auto& d : s.disks) {
        auto dj = diskJson(d);
        dj["read_bps"]  = nullptr;
        dj["write_bps"] = nullptr;
        diskJ.push_back(std::move(dj));
    }
    j["disks"] = std::move(diskJ);
    return j;
}

nlohmann::json HostMetricsReader::toJsonWithRates(const HostSnapshot& prev,
                                                   const HostSnapshot& curr) {
    nlohmann::json j = toJson(curr);
    const double dt = dtSeconds(prev, curr);

    // CPU: aggregate + per-core busy percentages.
    if (!curr.cpu.empty() && !prev.cpu.empty()) {
        j["cpu"]["total_pct"] = cpuBusyPct(prev.cpu.front(), curr.cpu.front());
        auto& per = j["cpu"]["per_core"];
        for (std::size_t i = 0; i < per.size(); ++i) {
            const std::string name = per[i].value("name", "");
            const auto* pp = findCpu(prev.cpu, name);
            per[i]["pct"] = pp ? cpuBusyPct(*pp, curr.cpu[i + 1]) : 0.0;
        }
    }

    // NICs: bytes-per-second.
    for (std::size_t i = 0; i < curr.nics.size(); ++i) {
        const auto* pp = findNic(prev.nics, curr.nics[i].name);
        if (!pp) continue;
        const auto& c = curr.nics[i];
        j["nics"][i]["rx_bps"] = c.rx_bytes >= pp->rx_bytes
            ? (c.rx_bytes - pp->rx_bytes) / dt : 0.0;
        j["nics"][i]["tx_bps"] = c.tx_bytes >= pp->tx_bytes
            ? (c.tx_bytes - pp->tx_bytes) / dt : 0.0;
    }

    // Disks: sectors → bytes at exposition time (Linux uses 512-byte
    // sectors in /proc/diskstats regardless of physical sector size).
    constexpr std::uint64_t kSectorBytes = 512;
    for (std::size_t i = 0; i < curr.disks.size(); ++i) {
        const auto* pp = findDisk(prev.disks, curr.disks[i].name);
        if (!pp) continue;
        const auto& c = curr.disks[i];
        j["disks"][i]["read_bps"] = c.sectors_read >= pp->sectors_read
            ? static_cast<double>(c.sectors_read - pp->sectors_read) * kSectorBytes / dt : 0.0;
        j["disks"][i]["write_bps"] = c.sectors_written >= pp->sectors_written
            ? static_cast<double>(c.sectors_written - pp->sectors_written) * kSectorBytes / dt : 0.0;
    }

    return j;
}

}  // namespace liveqx::metrics
