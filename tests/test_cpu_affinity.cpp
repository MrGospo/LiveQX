#include <gtest/gtest.h>
#include <atomic>
#include <pthread.h>
#include <sched.h>
#include <thread>

#include "utils/CpuAffinity.h"

// Most calls into numa::* are graceful no-ops on hosts without real NUMA
// (WSL2, single-node servers, /sys-restricted containers). The tests assert
// "graceful" semantics — call returns true and program does not crash.
// Mask-content checks are conditioned on numa::numaAvailable().

TEST(CpuAffinity, BindNegativeNodeIsNoop) {
    EXPECT_TRUE(numa::bindCurrentThreadToNode(-1));
    EXPECT_TRUE(numa::bindThreadToNode(pthread_self(), -1));
}

TEST(CpuAffinity, BindMemoryNegativeNodeIsNoop) {
    char buf[4096] = {};
    EXPECT_TRUE(numa::bindMemoryToNode(buf, sizeof(buf), -1));
}

TEST(CpuAffinity, BindMemoryNullAddrIsNoop) {
    EXPECT_TRUE(numa::bindMemoryToNode(nullptr, 0, 0));
}

TEST(CpuAffinity, BindInvalidNodeReturnsFalseOrGraceful) {
    // node 9999 should never exist. On NUMA hosts → returns false (no
    // cpulist, no panic). On non-NUMA hosts → graceful no-op returning true.
    const bool res = numa::bindCurrentThreadToNode(9999);
    if (numa::numaAvailable()) {
        EXPECT_FALSE(res);
    } else {
        EXPECT_TRUE(res);
    }
}

TEST(CpuAffinity, NumaAvailableIsStable) {
    // Cached after first call — repeated reads must agree.
    const bool a = numa::numaAvailable();
    const bool b = numa::numaAvailable();
    const bool c = numa::numaAvailable();
    EXPECT_EQ(a, b);
    EXPECT_EQ(b, c);
}

TEST(CpuAffinity, BindNode0DoesNotCrashEverywhere) {
    // node 0 always exists if NUMA is available, and is graceful no-op
    // otherwise. Either way: must not crash, must not change current
    // thread's mask in a way that strands us off the allowed-set.
    cpu_set_t before;
    CPU_ZERO(&before);
    sched_getaffinity(0, sizeof(before), &before);

    EXPECT_TRUE(numa::bindCurrentThreadToNode(0));

    cpu_set_t after;
    CPU_ZERO(&after);
    sched_getaffinity(0, sizeof(after), &after);
    EXPECT_GT(CPU_COUNT(&after), 0);  // we still have *some* CPU to run on

    // Restore
    pthread_setaffinity_np(pthread_self(), sizeof(before), &before);
}

TEST(CpuAffinity, RunOnNodeNegativeRunsInline) {
    const auto caller = std::this_thread::get_id();
    std::thread::id observed;
    numa::runOnNode(-1, [&] { observed = std::this_thread::get_id(); });
    EXPECT_EQ(observed, caller);
}

TEST(CpuAffinity, RunOnNodeNonNegativeRunsInWorker) {
    if (!numa::numaAvailable()) GTEST_SKIP() << "no NUMA on host";
    const auto caller = std::this_thread::get_id();
    std::thread::id observed;
    numa::runOnNode(0, [&] { observed = std::this_thread::get_id(); });
    EXPECT_NE(observed, caller);
}

TEST(CpuAffinity, RunOnNodeReturnsValue) {
    const int v = numa::runOnNode(-1, [] { return 42; });
    EXPECT_EQ(v, 42);
}

TEST(CpuAffinity, RunOnNodePropagatesVoid) {
    std::atomic<bool> ran{false};
    numa::runOnNode(-1, [&] { ran.store(true); });
    EXPECT_TRUE(ran.load());
}
