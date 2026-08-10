// fix26 c10 — tests for the internal cron parser.

#include <ctime>

#include <gtest/gtest.h>

#include "stress/CronExpr.h"

using liveqx::stress::CronExpr;

namespace {

// Build a UTC time_t from individual fields (Y/M/D h:m:s).
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

TEST(CronExpr, RejectsWrongFieldCount) {
    std::string err;
    EXPECT_FALSE(CronExpr::parse("0 2 * *", &err).has_value());
    EXPECT_NE(err.find("5 fields"), std::string::npos);
}

TEST(CronExpr, AllStarsMatchAlways) {
    auto c = CronExpr::parse("* * * * *");
    ASSERT_TRUE(c.has_value());
    EXPECT_TRUE(c->matches(utc(2026, 5, 7, 0,  0)));
    EXPECT_TRUE(c->matches(utc(2026, 5, 7, 12, 34)));
    EXPECT_TRUE(c->matches(utc(2027, 1, 1, 23, 59)));
}

TEST(CronExpr, DailyTwoAm) {
    auto c = CronExpr::parse("0 2 * * *");
    ASSERT_TRUE(c.has_value());
    // 2026-05-07 was a Thursday.
    EXPECT_TRUE (c->matches(utc(2026, 5, 7, 2,  0)));
    EXPECT_FALSE(c->matches(utc(2026, 5, 7, 2,  1)));
    EXPECT_FALSE(c->matches(utc(2026, 5, 7, 3,  0)));
    EXPECT_TRUE (c->matches(utc(2026, 5, 8, 2,  0)));
}

TEST(CronExpr, EveryFiveMinutes) {
    auto c = CronExpr::parse("*/5 * * * *");
    ASSERT_TRUE(c.has_value());
    EXPECT_TRUE (c->matches(utc(2026, 5, 7, 12,  0)));
    EXPECT_TRUE (c->matches(utc(2026, 5, 7, 12,  5)));
    EXPECT_FALSE(c->matches(utc(2026, 5, 7, 12,  6)));
    EXPECT_TRUE (c->matches(utc(2026, 5, 7, 12, 55)));
}

TEST(CronExpr, RangesAndLists) {
    auto c = CronExpr::parse("0,30 9-17 * * *");
    ASSERT_TRUE(c.has_value());
    EXPECT_TRUE (c->matches(utc(2026, 5, 7,  9,  0)));
    EXPECT_TRUE (c->matches(utc(2026, 5, 7,  9, 30)));
    EXPECT_FALSE(c->matches(utc(2026, 5, 7,  9, 15)));
    EXPECT_TRUE (c->matches(utc(2026, 5, 7, 17,  0)));
    EXPECT_FALSE(c->matches(utc(2026, 5, 7, 18,  0)));
    EXPECT_FALSE(c->matches(utc(2026, 5, 7,  8, 30)));
}

TEST(CronExpr, WeekdayOnly) {
    // 2026-05-07 = Thursday (dow=4); 2026-05-09 = Saturday (dow=6).
    auto c = CronExpr::parse("0 9 * * 1-5");
    ASSERT_TRUE(c.has_value());
    EXPECT_TRUE (c->matches(utc(2026, 5, 7, 9, 0)));   // Thursday
    EXPECT_FALSE(c->matches(utc(2026, 5, 9, 9, 0)));   // Saturday
    EXPECT_FALSE(c->matches(utc(2026, 5,10, 9, 0)));   // Sunday
}

TEST(CronExpr, DomAndDowOredWhenBothRestricted) {
    // Standard cron: when dom AND dow are both restricted, fire if EITHER
    // matches. dom=1 OR dow=0(Sun).
    auto c = CronExpr::parse("0 9 1 * 0");
    ASSERT_TRUE(c.has_value());
    EXPECT_TRUE (c->matches(utc(2026, 5, 1, 9, 0)));   // dom=1, Friday
    EXPECT_TRUE (c->matches(utc(2026, 5,10, 9, 0)));   // Sunday
    EXPECT_FALSE(c->matches(utc(2026, 5, 7, 9, 0)));   // Thursday, dom=7
}

TEST(CronExpr, OutOfRangeFieldsRejected) {
    EXPECT_FALSE(CronExpr::parse("60 * * * *").has_value());
    EXPECT_FALSE(CronExpr::parse("* 24 * * *").has_value());
    EXPECT_FALSE(CronExpr::parse("* * 0 * *").has_value());
    EXPECT_FALSE(CronExpr::parse("* * 32 * *").has_value());
    EXPECT_FALSE(CronExpr::parse("* * * 13 *").has_value());
    EXPECT_FALSE(CronExpr::parse("* * * * 7").has_value());
    EXPECT_FALSE(CronExpr::parse("garbage * * * *").has_value());
}
