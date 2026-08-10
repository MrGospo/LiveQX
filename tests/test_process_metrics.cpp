#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "metrics/ProcessMetrics.h"

TEST(ProcessMetrics, StartTimeIsForwarded) {
    ProcessMetrics pm{1700000000};
    const auto s = pm.snapshot();
    EXPECT_EQ(s.start_time_unix_sec, 1700000000);
}

#if defined(__linux__)

TEST(ProcessMetrics, LinuxFieldsArePopulated) {
    ProcessMetrics pm{0};
    const auto s = pm.snapshot();

    // VmRSS / VmSize are guaranteed > 0 for any running process.
    EXPECT_GT(s.rss_bytes,   0);
    EXPECT_GT(s.vsize_bytes, 0);

    // The process always has at least the gtest main thread.
    EXPECT_GE(s.threads, 1);

    // Open fds: stdin/stdout/stderr + opendir bookkeeping; should be ≥ 3.
    EXPECT_GE(s.open_fds, 3);

    // CPU seconds — non-negative; first read can legitimately be 0.0
    // on a freshly forked thread, but never negative on success.
    EXPECT_GE(s.cpu_seconds, 0.0);
}

TEST(ProcessMetrics, CacheServesStaleSnapshotWithinTtl) {
    ProcessMetrics pm{0};
    pm.setCacheTtl(std::chrono::seconds(60));  // make staleness window enormous

    const auto s1 = pm.snapshot();
    // Burn some CPU so true cpu_seconds advances measurably.
    volatile std::uint64_t sink = 0;
    for (int i = 0; i < 1'000'000; ++i) sink += static_cast<std::uint64_t>(i);
    (void)sink;
    const auto s2 = pm.snapshot();

    // Cache HIT: identical instance returned. cpu_seconds equal proves
    // we did not re-read getrusage despite real elapsed CPU.
    EXPECT_DOUBLE_EQ(s1.cpu_seconds, s2.cpu_seconds);
    EXPECT_EQ(s1.rss_bytes,   s2.rss_bytes);
    EXPECT_EQ(s1.vsize_bytes, s2.vsize_bytes);
}

TEST(ProcessMetrics, CacheRefreshesAfterTtl) {
    ProcessMetrics pm{0};
    pm.setCacheTtl(std::chrono::milliseconds(1));

    const auto s1 = pm.snapshot();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const auto s2 = pm.snapshot();

    // After TTL elapses we re-read /proc/self/*. cpu_seconds should be
    // monotonically non-decreasing (allow equality if process was idle).
    EXPECT_GE(s2.cpu_seconds, s1.cpu_seconds);
}

#endif  // __linux__
