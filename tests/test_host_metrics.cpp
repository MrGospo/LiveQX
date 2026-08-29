#include <gtest/gtest.h>

#include "metrics/HostMetrics.h"

using namespace liveqx::metrics;

// ── Live /proc read on Linux ───────────────────────────────────────────────
// Basic smoke test — the machine running the suite must have populated
// /proc/stat, /proc/meminfo etc. Any Linux CI passes; non-Linux is
// unsupported by the module and returns an empty snapshot.

#if defined(__linux__)

TEST(HostMetrics, SamplePopulatesCoreFields) {
    const auto s = HostMetricsReader::sample();
    EXPECT_GT(s.sampled_at_unix_ms, 0);
    EXPECT_GT(s.uptime_seconds,     0);
    EXPECT_GT(s.mem.total_bytes,    0u);
    EXPECT_GT(s.mem.available_bytes, 0u);
    ASSERT_FALSE(s.cpu.empty());              // at least the aggregate line
    EXPECT_EQ(s.cpu.front().name, "cpu");
    // At least one filesystem visible on a real box (root '/' is always
    // mounted on Linux CI). Loopback rootfs (initramfs) presents as
    // "rootfs"/"tmpfs" and would be filtered — the test bed must have
    // a real block-backed root.
    if (!s.fs.empty()) {
        EXPECT_GT(s.fs.front().total_bytes, 0u);
    }
}

#endif  // __linux__

// ── JSON schema stability ──────────────────────────────────────────────────
// The UI hard-codes these key paths. If a rename or restructure lands,
// the test breaks first — before the UI silently shows "—" everywhere.

TEST(HostMetrics, SnapshotJsonHasExpectedKeys) {
    HostSnapshot s;
    s.sampled_at_unix_ms = 1000;
    s.cpu.push_back({"cpu", 10, 0, 5, 100, 2, 0, 0, 0});
    s.mem.total_bytes     = 16ull * 1024 * 1024 * 1024;
    s.mem.available_bytes =  8ull * 1024 * 1024 * 1024;
    s.nics.push_back({"eth0", true, 1000, 10, 0, 0, 500, 5, 0, 0});

    const auto j = HostMetricsReader::toJson(s);
    ASSERT_TRUE(j.contains("cpu"));
    ASSERT_TRUE(j["cpu"].contains("aggregate"));
    ASSERT_TRUE(j["cpu"].contains("per_core"));
    EXPECT_TRUE(j["cpu"]["total_pct"].is_null());     // no rate w/o prev

    ASSERT_TRUE(j.contains("mem"));
    EXPECT_EQ(j["mem"]["total_bytes"].get<std::uint64_t>(), s.mem.total_bytes);
    EXPECT_EQ(j["mem"]["used_bytes"].get<std::uint64_t>(),
              s.mem.total_bytes - s.mem.available_bytes);

    ASSERT_TRUE(j.contains("nics"));
    ASSERT_EQ(j["nics"].size(), 1u);
    EXPECT_EQ(j["nics"][0]["name"], "eth0");
    EXPECT_TRUE(j["nics"][0]["rx_bps"].is_null());
    EXPECT_TRUE(j["nics"][0]["tx_bps"].is_null());
}

// ── CPU busy percentage: computed from jiffies delta ───────────────────────

TEST(HostMetrics, CpuPctZeroForIdenticalSnapshots) {
    HostSnapshot prev, curr;
    prev.sampled_at_unix_ms = 0;
    curr.sampled_at_unix_ms = 1000;
    HostCpuLine c{"cpu", 100, 0, 50, 5000, 10, 0, 0, 0};
    prev.cpu.push_back(c);
    curr.cpu.push_back(c);
    const auto j = HostMetricsReader::toJsonWithRates(prev, curr);
    EXPECT_DOUBLE_EQ(j["cpu"]["total_pct"].get<double>(), 0.0);
}

TEST(HostMetrics, CpuPctAllBusyIs100) {
    HostSnapshot prev, curr;
    prev.sampled_at_unix_ms = 0;
    curr.sampled_at_unix_ms = 1000;
    // busy jiffies advance by 100; idle stays flat → 100% busy
    prev.cpu.push_back({"cpu", 0,   0, 0,   1000, 0, 0, 0, 0});
    curr.cpu.push_back({"cpu", 100, 0, 0,   1000, 0, 0, 0, 0});
    const auto j = HostMetricsReader::toJsonWithRates(prev, curr);
    EXPECT_DOUBLE_EQ(j["cpu"]["total_pct"].get<double>(), 100.0);
}

TEST(HostMetrics, CpuPctHalfBusy) {
    HostSnapshot prev, curr;
    prev.sampled_at_unix_ms = 0;
    curr.sampled_at_unix_ms = 1000;
    // +50 busy, +50 idle over the interval → 50%
    prev.cpu.push_back({"cpu", 0,  0, 0,  0,  0, 0, 0, 0});
    curr.cpu.push_back({"cpu", 50, 0, 0,  50, 0, 0, 0, 0});
    const auto j = HostMetricsReader::toJsonWithRates(prev, curr);
    EXPECT_DOUBLE_EQ(j["cpu"]["total_pct"].get<double>(), 50.0);
}

TEST(HostMetrics, CpuPctCounterResetClampsToZero) {
    // If /proc/stat rewinds (kernel bug / container restart), curr <= prev.
    // We must not emit a negative or absurd percentage.
    HostSnapshot prev, curr;
    prev.sampled_at_unix_ms = 0;
    curr.sampled_at_unix_ms = 1000;
    prev.cpu.push_back({"cpu", 1000, 0, 1000, 5000, 0, 0, 0, 0});
    curr.cpu.push_back({"cpu",   10, 0,   10,   50, 0, 0, 0, 0});
    const auto j = HostMetricsReader::toJsonWithRates(prev, curr);
    EXPECT_DOUBLE_EQ(j["cpu"]["total_pct"].get<double>(), 0.0);
}

// ── Per-core CPU: aligned by name, not index ───────────────────────────────

TEST(HostMetrics, PerCoreMatchedByName) {
    HostSnapshot prev, curr;
    prev.sampled_at_unix_ms = 0;
    curr.sampled_at_unix_ms = 1000;
    prev.cpu.push_back({"cpu",  100, 0, 100, 800, 0, 0, 0, 0});
    prev.cpu.push_back({"cpu0",  50, 0,  50, 400, 0, 0, 0, 0});
    prev.cpu.push_back({"cpu1",  50, 0,  50, 400, 0, 0, 0, 0});
    curr.cpu.push_back({"cpu",  200, 0, 200, 800, 0, 0, 0, 0});
    curr.cpu.push_back({"cpu0", 150, 0, 150, 400, 0, 0, 0, 0});  // fully busy
    curr.cpu.push_back({"cpu1",  50, 0,  50, 800, 0, 0, 0, 0});  // fully idle

    const auto j = HostMetricsReader::toJsonWithRates(prev, curr);
    ASSERT_EQ(j["cpu"]["per_core"].size(), 2u);
    // Order is preserved from sample(), which is deterministic-by-file-order.
    EXPECT_EQ(j["cpu"]["per_core"][0]["name"], "cpu0");
    EXPECT_DOUBLE_EQ(j["cpu"]["per_core"][0]["pct"].get<double>(), 100.0);
    EXPECT_EQ(j["cpu"]["per_core"][1]["name"], "cpu1");
    EXPECT_DOUBLE_EQ(j["cpu"]["per_core"][1]["pct"].get<double>(),   0.0);
}

// ── NIC throughput ─────────────────────────────────────────────────────────

TEST(HostMetrics, NicBpsIsBytesDeltaOverSeconds) {
    HostSnapshot prev, curr;
    prev.sampled_at_unix_ms = 0;
    curr.sampled_at_unix_ms = 2000;  // 2-second window
    prev.nics.push_back({"eth0", true, 1000, 0, 0, 0,  500, 0, 0, 0});
    curr.nics.push_back({"eth0", true, 3000, 0, 0, 0, 1500, 0, 0, 0});
    const auto j = HostMetricsReader::toJsonWithRates(prev, curr);
    EXPECT_DOUBLE_EQ(j["nics"][0]["rx_bps"].get<double>(), 1000.0);  // 2000B / 2s
    EXPECT_DOUBLE_EQ(j["nics"][0]["tx_bps"].get<double>(),  500.0);  // 1000B / 2s
}

TEST(HostMetrics, NicBpsCounterResetIsZero) {
    HostSnapshot prev, curr;
    prev.sampled_at_unix_ms = 0;
    curr.sampled_at_unix_ms = 1000;
    prev.nics.push_back({"eth0", true, 10000, 0, 0, 0, 5000, 0, 0, 0});
    curr.nics.push_back({"eth0", true,     1, 0, 0, 0,    1, 0, 0, 0});
    const auto j = HostMetricsReader::toJsonWithRates(prev, curr);
    EXPECT_DOUBLE_EQ(j["nics"][0]["rx_bps"].get<double>(), 0.0);
    EXPECT_DOUBLE_EQ(j["nics"][0]["tx_bps"].get<double>(), 0.0);
}

TEST(HostMetrics, DiskIoBpsUses512ByteSectors) {
    HostSnapshot prev, curr;
    prev.sampled_at_unix_ms = 0;
    curr.sampled_at_unix_ms = 1000;
    prev.disks.push_back({"sda", 0, 0, 0, 0});
    // +2 sectors read = +1024 bytes over 1s → 1024 B/s
    curr.disks.push_back({"sda", 1, 2, 1, 4});
    const auto j = HostMetricsReader::toJsonWithRates(prev, curr);
    EXPECT_DOUBLE_EQ(j["disks"][0]["read_bps"].get<double>(),  1024.0);
    EXPECT_DOUBLE_EQ(j["disks"][0]["write_bps"].get<double>(), 2048.0);
}
