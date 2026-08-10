#pragma once
#include <cstddef>
#include <exception>
#include <pthread.h>
#include <thread>
#include <type_traits>
#include <utility>

// NUMA-aware affinity helpers without a libnuma dependency. Reads
// /sys/devices/system/node/* directly and uses pthread_setaffinity_np /
// mbind syscall. Every operation is graceful: when NUMA is unavailable
// (WSL, single-node host, restricted /sys) calls become no-ops returning
// true so callers don't need to branch on host capabilities.
namespace numa {

// True if /sys/devices/system/node/online lists at least one node and is
// readable. Cached after first call.
bool numaAvailable();

// Pin caller / arbitrary thread to all CPUs of the given NUMA node,
// intersected with the process's allowed-set (sched_getaffinity).
//   node < 0                 => no-op, returns true.
//   numaAvailable() == false => no-op + once-warn, returns true.
//   invalid node / parse fail / empty intersection => warn-log, returns false.
bool bindCurrentThreadToNode(int node);
bool bindThreadToNode(pthread_t handle, int node);

// Apply MPOL_BIND to the page range covering [addr, addr+length) so that
// the kernel binds those pages to the given node, regardless of first-touch.
//   node < 0 / numaAvailable() == false => no-op, returns true.
//   syscall failure => warn-log, returns false (still safe to continue).
bool bindMemoryToNode(void* addr, std::size_t length, int node);

namespace detail {
// Common worker for runOnNode — declared here so the template body is small.
void runOnNodeImpl(int node, void (*fn)(void*), void* ctx);
}  // namespace detail

// Run fn() in a freshly-spawned thread pinned to node, return its result.
// Used so initial allocations (FramePool, Timeline, etc.) happen on a
// pinned thread → first-touch lands on the right node. The manager thread
// itself is never mutated. node < 0 just runs fn() inline.
template <typename Fn>
auto runOnNode(int node, Fn&& fn) -> std::invoke_result_t<Fn&> {
    using R = std::invoke_result_t<Fn&>;
    if (node < 0) {
        return std::forward<Fn>(fn)();
    }
    std::exception_ptr ex;
    if constexpr (std::is_void_v<R>) {
        std::thread t([&] {
            bindCurrentThreadToNode(node);
            try { fn(); } catch (...) { ex = std::current_exception(); }
        });
        t.join();
        if (ex) std::rethrow_exception(ex);
    } else {
        R result{};
        std::thread t([&] {
            bindCurrentThreadToNode(node);
            try { result = fn(); } catch (...) { ex = std::current_exception(); }
        });
        t.join();
        if (ex) std::rethrow_exception(ex);
        return result;
    }
}

}  // namespace numa
