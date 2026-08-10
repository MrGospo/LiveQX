// fix26 c10 — tests for StressScheduler.
//
// We don't depend on real wall-clock or thread-timing in these tests:
// pokeForTest drives the dedup logic synchronously with synthetic times.

#include <atomic>
#include <ctime>

#include <gtest/gtest.h>

#include "stress/StressScheduler.h"

using liveqx::stress::StressScheduler;

namespace {

std::time_t utc(int y, int mon, int d, int h, int m, int s = 0) {
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon  = mon - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min  = m;
    tm.tm_sec  = s;
    return timegm(&tm);
}

}  // namespace

TEST(StressScheduler, FiresOnceWhenCronMatches) {
    std::atomic<int> calls{0};
    StressScheduler s("0 2 * * *", [&] { calls.fetch_add(1); });

    EXPECT_TRUE (s.pokeForTest(utc(2026, 5, 7, 2, 0)));   // matches, first time
    EXPECT_FALSE(s.pokeForTest(utc(2026, 5, 7, 2, 0)));   // same minute -> dedup
    EXPECT_FALSE(s.pokeForTest(utc(2026, 5, 7, 2, 0, 30)));// same minute, +30s
    EXPECT_FALSE(s.pokeForTest(utc(2026, 5, 7, 2, 1)));   // not a match
    EXPECT_TRUE (s.pokeForTest(utc(2026, 5, 8, 2, 0)));   // next day -> fires again
    EXPECT_EQ(calls.load(), 2);
}

TEST(StressScheduler, NoFireWhenCronInvalidOnConstruct) {
    std::atomic<int> calls{0};
    StressScheduler s("garbage cron", [&] { calls.fetch_add(1); });
    EXPECT_FALSE(s.pokeForTest(utc(2026, 5, 7, 2, 0)));
    EXPECT_EQ(calls.load(), 0);
}

TEST(StressScheduler, SetCronHotReplaces) {
    std::atomic<int> calls{0};
    StressScheduler s("0 2 * * *", [&] { calls.fetch_add(1); });

    // Original schedule fires at 02:00.
    EXPECT_TRUE(s.pokeForTest(utc(2026, 5, 7, 2, 0)));
    EXPECT_EQ(calls.load(), 1);

    std::string err;
    ASSERT_TRUE(s.setCron("0 3 * * *", &err)) << err;
    EXPECT_EQ(s.cron(), "0 3 * * *");

    // Old schedule no longer matches at 02:00 on a different day.
    EXPECT_FALSE(s.pokeForTest(utc(2026, 5, 8, 2, 0)));
    EXPECT_EQ(calls.load(), 1);
    // New schedule fires at 03:00.
    EXPECT_TRUE(s.pokeForTest(utc(2026, 5, 8, 3, 0)));
    EXPECT_EQ(calls.load(), 2);
}

TEST(StressScheduler, SetCronRejectsInvalidExpression) {
    StressScheduler s("0 2 * * *", [] {});
    std::string err;
    EXPECT_FALSE(s.setCron("not a cron", &err));
    EXPECT_FALSE(err.empty());
    // Old schedule still in place.
    EXPECT_EQ(s.cron(), "0 2 * * *");
}
