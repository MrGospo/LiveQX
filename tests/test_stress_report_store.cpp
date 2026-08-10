// fix26 c10 — tests for StressReportStore.

#include <cstdint>
#include <filesystem>
#include <unistd.h>

#include <gtest/gtest.h>

#include "stress/StressReport.h"
#include "stress/StressReportStore.h"

namespace fs = std::filesystem;
using liveqx::stress::StressReport;
using liveqx::stress::StressReportStore;

namespace {

fs::path uniqueDir(const std::string& tag) {
    auto base = fs::temp_directory_path() /
                ("stress_reports_test_" + tag + "_" +
                 std::to_string(::getpid()));
    std::error_code ec;
    fs::remove_all(base, ec);
    fs::create_directories(base);
    return base;
}

StressReport makeReport(std::int64_t started_at_ms, bool pass) {
    StressReport r;
    r.started_at_ms = started_at_ms;
    r.ended_at_ms   = started_at_ms + 1000;
    r.duration_sec  = 1;
    r.pass          = pass;
    r.verdict       = pass ? "pass" : "fail: synthetic";
    return r;
}

}  // namespace

TEST(StressReportStore, WriteThenReadRoundTrips) {
    auto dir = uniqueDir("rw");
    StressReportStore s(dir);

    auto r  = makeReport(1730000000000, true);
    auto id = s.write(r);
    EXPECT_FALSE(id.empty());
    EXPECT_TRUE(fs::exists(dir / (id + ".json")));

    auto j = s.read(id);
    ASSERT_TRUE(j.has_value());
    EXPECT_EQ((*j)["started_at_ms"].get<std::int64_t>(), 1730000000000);
    EXPECT_TRUE((*j)["pass"].get<bool>());

    fs::remove_all(dir);
}

TEST(StressReportStore, ListNewestFirst) {
    auto dir = uniqueDir("list");
    StressReportStore s(dir);

    s.write(makeReport(1700000000000, true));   // older
    s.write(makeReport(1730000000000, false));  // newer

    auto entries = s.list();
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_GT(entries[0].started_at_ms, entries[1].started_at_ms);
    EXPECT_FALSE(entries[0].pass);
    EXPECT_TRUE (entries[1].pass);

    fs::remove_all(dir);
}

TEST(StressReportStore, SameMinuteCollisionGetsSuffix) {
    auto dir = uniqueDir("collide");
    StressReportStore s(dir);

    auto id1 = s.write(makeReport(1730000000000, true));
    auto id2 = s.write(makeReport(1730000000000, false));   // same id base
    EXPECT_FALSE(id1.empty());
    EXPECT_FALSE(id2.empty());
    EXPECT_NE(id1, id2);

    fs::remove_all(dir);
}

TEST(StressReportStore, ReadUnknownReturnsNullopt) {
    auto dir = uniqueDir("unknown");
    StressReportStore s(dir);
    EXPECT_FALSE(s.read("does-not-exist").has_value());
    fs::remove_all(dir);
}

TEST(StressReportStore, PrunesAboveCap) {
    auto dir = uniqueDir("prune");
    StressReportStore s(dir);

    // Write kMaxReports + 5 reports, each with a distinct timestamp
    // separated by minutes so ids don't collide.
    const std::size_t total = StressReportStore::kMaxReports + 5;
    for (std::size_t i = 0; i < total; ++i) {
        std::int64_t ts = 1700000000000 + static_cast<std::int64_t>(i) * 60'000;
        s.write(makeReport(ts, true));
    }
    EXPECT_LE(s.list().size(), StressReportStore::kMaxReports);

    fs::remove_all(dir);
}
