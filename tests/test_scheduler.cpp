#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "core/ScheduleEntry.h"
#include "core/Scheduler.h"

using nlohmann::json;
using namespace liveqx::scheduling;

namespace {

// 2026-05-04 is a Monday in UTC. We anchor every relative test to it.
//   `date -u -d '2026-05-04 00:00:00 UTC' +%s` → 1777852800
constexpr int64_t kMonday2026May04UtcMidnight = 1777852800LL * 1'000'000'000LL;

ScheduleEntry mkDaily(std::string id,
                      const std::string& start, const std::string& end,
                      int priority = 100) {
    ScheduleEntry e;
    e.id = std::move(id);
    e.playlist = {"ads/a.mp4"};
    e.recurrence.kind        = RecurrenceKind::Daily;
    e.recurrence.start_time  = parseTimeOfDay(start);
    e.recurrence.end_time    = parseTimeOfDay(end);
    e.priority               = priority;
    return e;
}

ScheduleEntry mkWeekly(std::string id,
                       std::vector<int> days,
                       const std::string& start, const std::string& end,
                       int priority = 100) {
    auto e = mkDaily(std::move(id), start, end, priority);
    e.recurrence.kind         = RecurrenceKind::Weekly;
    e.recurrence.days_of_week = std::move(days);
    return e;
}

ScheduleEntry mkOnce(std::string id,
                     int64_t start_ns, int64_t end_ns,
                     int priority = 100) {
    ScheduleEntry e;
    e.id = std::move(id);
    e.playlist = {"once.mp4"};
    e.recurrence.kind        = RecurrenceKind::Once;
    e.recurrence.start_at_ns = start_ns;
    e.recurrence.end_at_ns   = end_ns;
    e.priority               = priority;
    return e;
}

}  // namespace

// ─── decide() basics ──────────────────────────────────────────────────────────

TEST(Scheduler, EmptyReturnsRegular) {
    Scheduler s({});
    auto d = s.decide(kMonday2026May04UtcMidnight);
    EXPECT_FALSE(d.scheduled);
}

TEST(Scheduler, DailyWindowActive) {
    std::vector<ScheduleEntry> es;
    es.push_back(mkDaily("daily-10", "10:00", "11:00"));
    Scheduler s(std::move(es));

    const int64_t at_10_30 = kMonday2026May04UtcMidnight + 10 * 3600 * 1'000'000'000LL
                                                         + 30 * 60   * 1'000'000'000LL;
    auto d = s.decide(at_10_30);
    EXPECT_TRUE(d.scheduled);
    EXPECT_EQ(d.entry_id, "daily-10");
    EXPECT_EQ(d.window_end_ns,
              kMonday2026May04UtcMidnight + 11 * 3600 * 1'000'000'000LL);
}

TEST(Scheduler, DailyOutsideWindowReturnsRegular) {
    std::vector<ScheduleEntry> es;
    es.push_back(mkDaily("daily-10", "10:00", "11:00"));
    Scheduler s(std::move(es));

    const int64_t at_09_30 = kMonday2026May04UtcMidnight + 9 * 3600 * 1'000'000'000LL
                                                         + 30 * 60  * 1'000'000'000LL;
    EXPECT_FALSE(s.decide(at_09_30).scheduled);

    const int64_t at_11_30 = kMonday2026May04UtcMidnight + 11 * 3600 * 1'000'000'000LL
                                                         + 30 * 60   * 1'000'000'000LL;
    EXPECT_FALSE(s.decide(at_11_30).scheduled);
}

TEST(Scheduler, EndExclusiveStartInclusive) {
    std::vector<ScheduleEntry> es;
    es.push_back(mkDaily("d", "10:00", "11:00"));
    Scheduler s(std::move(es));

    const int64_t exact_start = kMonday2026May04UtcMidnight + 10 * 3600 * 1'000'000'000LL;
    EXPECT_TRUE(s.decide(exact_start).scheduled);

    const int64_t exact_end = kMonday2026May04UtcMidnight + 11 * 3600 * 1'000'000'000LL;
    EXPECT_FALSE(s.decide(exact_end).scheduled);
}

// ─── weekly ───────────────────────────────────────────────────────────────────

TEST(Scheduler, WeeklyMatchesOnAllowedDay) {
    std::vector<ScheduleEntry> es;
    es.push_back(mkWeekly("w-mon", {1}, "10:00", "11:00"));
    Scheduler s(std::move(es));

    const int64_t mon_10 = kMonday2026May04UtcMidnight + 10 * 3600 * 1'000'000'000LL;
    EXPECT_TRUE(s.decide(mon_10).scheduled);

    // Tuesday 10:00 — same time, weekday 2, not in days
    const int64_t tue_10 = mon_10 + 86400 * 1'000'000'000LL;
    EXPECT_FALSE(s.decide(tue_10).scheduled);
}

TEST(Scheduler, WeeklyWeekendsOnly) {
    std::vector<ScheduleEntry> es;
    es.push_back(mkWeekly("weekend", {6, 7}, "08:00", "09:00"));
    Scheduler s(std::move(es));

    const int64_t mon_8 = kMonday2026May04UtcMidnight + 8 * 3600 * 1'000'000'000LL;
    EXPECT_FALSE(s.decide(mon_8).scheduled);
    const int64_t sat_8 = mon_8 + 5 * 86400 * 1'000'000'000LL;
    EXPECT_TRUE(s.decide(sat_8).scheduled);
    const int64_t sun_8 = mon_8 + 6 * 86400 * 1'000'000'000LL;
    EXPECT_TRUE(s.decide(sun_8).scheduled);
}

// ─── monthly ──────────────────────────────────────────────────────────────────

TEST(Scheduler, MonthlyFebruaryWithDay31Skips) {
    ScheduleEntry e;
    e.id = "m31";
    e.playlist = {"a.mp4"};
    e.recurrence.kind          = RecurrenceKind::Monthly;
    e.recurrence.days_of_month = {31};
    e.recurrence.start_time    = parseTimeOfDay("10:00");
    e.recurrence.end_time      = parseTimeOfDay("11:00");
    Scheduler s({e});

    // 2026-02-28 10:00 — last day of Feb, not 31, must NOT match.
    const int64_t feb28 = parseIsoUtcNs("2026-02-28T10:00:00Z");
    EXPECT_FALSE(s.decide(feb28).scheduled);

    // 2026-03-31 10:00 — must match.
    const int64_t mar31 = parseIsoUtcNs("2026-03-31T10:00:00Z");
    EXPECT_TRUE(s.decide(mar31).scheduled);
}

// ─── once ─────────────────────────────────────────────────────────────────────

TEST(Scheduler, OnceWindowExact) {
    const int64_t s_ns = parseIsoUtcNs("2026-12-31T22:00:00Z");
    const int64_t e_ns = parseIsoUtcNs("2027-01-01T02:00:00Z");
    Scheduler s({mkOnce("ny", s_ns, e_ns)});

    EXPECT_FALSE(s.decide(s_ns - 1).scheduled);
    EXPECT_TRUE (s.decide(s_ns).scheduled);
    EXPECT_TRUE (s.decide((s_ns + e_ns) / 2).scheduled);
    EXPECT_FALSE(s.decide(e_ns).scheduled);
    EXPECT_FALSE(s.decide(e_ns + 1).scheduled);
}

// ─── priority + tiebreak ──────────────────────────────────────────────────────

TEST(Scheduler, HigherPriorityWinsOverlap) {
    std::vector<ScheduleEntry> es;
    es.push_back(mkDaily("low",  "10:00", "11:00",  50));
    es.push_back(mkDaily("high", "10:00", "10:30", 200));
    Scheduler s(std::move(es));

    const int64_t at_10_15 = kMonday2026May04UtcMidnight + 10 * 3600 * 1'000'000'000LL
                                                         + 15 * 60   * 1'000'000'000LL;
    auto d = s.decide(at_10_15);
    EXPECT_TRUE(d.scheduled);
    EXPECT_EQ(d.entry_id, "high");
}

TEST(Scheduler, EqualPriorityTieBrokenByIdLex) {
    std::vector<ScheduleEntry> es;
    es.push_back(mkDaily("zzz", "10:00", "11:00", 100));
    es.push_back(mkDaily("aaa", "10:00", "11:00", 100));
    Scheduler s(std::move(es));

    const int64_t t = kMonday2026May04UtcMidnight + 10 * 3600 * 1'000'000'000LL;
    EXPECT_EQ(s.decide(t).entry_id, "aaa");
}

// ─── effective_from / effective_to ────────────────────────────────────────────

TEST(Scheduler, EffectiveRangeGate) {
    auto e = mkDaily("d", "10:00", "11:00");
    e.effective_from = DateOnly{2026, 6, 1};
    Scheduler s({e});

    const int64_t may_5_10 = parseIsoUtcNs("2026-05-05T10:00:00Z");
    EXPECT_FALSE(s.decide(may_5_10).scheduled);
    const int64_t jun_5_10 = parseIsoUtcNs("2026-06-05T10:00:00Z");
    EXPECT_TRUE (s.decide(jun_5_10).scheduled);
}

// ─── hot reload via setEntries ────────────────────────────────────────────────

TEST(Scheduler, SetEntriesSwapsAtomically) {
    Scheduler s({mkDaily("a", "10:00", "11:00")});
    const int64_t at_10 = kMonday2026May04UtcMidnight + 10 * 3600 * 1'000'000'000LL;
    EXPECT_EQ(s.decide(at_10).entry_id, "a");

    s.setEntries({mkDaily("b", "10:00", "11:00")});
    EXPECT_EQ(s.decide(at_10).entry_id, "b");

    s.setEntries({});
    EXPECT_FALSE(s.decide(at_10).scheduled);
}

// ─── upcoming ─────────────────────────────────────────────────────────────────

TEST(Scheduler, UpcomingFindsFirstActivation) {
    std::vector<ScheduleEntry> es;
    es.push_back(mkDaily("d10", "10:00", "11:00"));
    es.push_back(mkDaily("d12", "12:00", "12:30"));
    Scheduler s(std::move(es));

    const int64_t at_09 = kMonday2026May04UtcMidnight + 9 * 3600 * 1'000'000'000LL;
    auto up = s.upcoming(at_09, 7200);  // next 2h
    ASSERT_EQ(up.size(), 1u);
    EXPECT_EQ(up[0].entry_id, "d10");
    EXPECT_EQ(up[0].starts_at_ns,
              kMonday2026May04UtcMidnight + 10 * 3600 * 1'000'000'000LL);
}

TEST(Scheduler, UpcomingSortedByStart) {
    std::vector<ScheduleEntry> es;
    es.push_back(mkDaily("late",  "12:00", "12:30"));
    es.push_back(mkDaily("early", "10:00", "10:30"));
    Scheduler s(std::move(es));

    const int64_t at_09 = kMonday2026May04UtcMidnight + 9 * 3600 * 1'000'000'000LL;
    auto up = s.upcoming(at_09, 4 * 3600);
    ASSERT_EQ(up.size(), 2u);
    EXPECT_EQ(up[0].entry_id, "early");
    EXPECT_EQ(up[1].entry_id, "late");
}

TEST(Scheduler, UpcomingHorizonGate) {
    std::vector<ScheduleEntry> es;
    es.push_back(mkDaily("far", "23:30", "23:45"));
    Scheduler s(std::move(es));

    const int64_t at_09 = kMonday2026May04UtcMidnight + 9 * 3600 * 1'000'000'000LL;
    EXPECT_TRUE (s.upcoming(at_09, 24 * 3600).size() == 1);
    EXPECT_TRUE (s.upcoming(at_09, 3600).empty());
}

TEST(Scheduler, UpcomingWeeklyJumpsToNextValidDay) {
    std::vector<ScheduleEntry> es;
    es.push_back(mkWeekly("sat", {6}, "10:00", "11:00"));
    Scheduler s(std::move(es));

    const int64_t mon_09 = kMonday2026May04UtcMidnight + 9 * 3600 * 1'000'000'000LL;
    auto up = s.upcoming(mon_09, 7 * 86400);
    ASSERT_EQ(up.size(), 1u);
    // Saturday = day 5 after Monday; expect Mon midnight + 5*86400 + 10h
    EXPECT_EQ(up[0].starts_at_ns,
              kMonday2026May04UtcMidnight
                + 5 * 86400 * 1'000'000'000LL
                + 10 * 3600 * 1'000'000'000LL);
}

// ─── statusJson ───────────────────────────────────────────────────────────────

TEST(Scheduler, StatusJsonReflectsActiveOrRegular) {
    Scheduler s({mkDaily("d", "10:00", "11:00")});
    const int64_t inside  = kMonday2026May04UtcMidnight + 10 * 3600 * 1'000'000'000LL;
    const int64_t outside = kMonday2026May04UtcMidnight + 12 * 3600 * 1'000'000'000LL;

    auto j_in  = s.statusJson(inside);
    EXPECT_EQ(j_in["mode"], "schedule");
    EXPECT_EQ(j_in["entry_id"], "d");

    auto j_out = s.statusJson(outside);
    EXPECT_EQ(j_out["mode"], "regular");
    EXPECT_TRUE(j_out["entry_id"].is_null());
}

// ─── snapshot semantics: in-flight decide sees old snapshot ───────────────────

TEST(Scheduler, EntriesSnapshotIsImmutable) {
    Scheduler s({mkDaily("a", "10:00", "11:00")});
    auto snapshot1 = s.entries();
    s.setEntries({mkDaily("b", "12:00", "13:00")});
    auto snapshot2 = s.entries();

    ASSERT_EQ(snapshot1.size(), 1u);
    EXPECT_EQ(snapshot1[0].id, "a");
    ASSERT_EQ(snapshot2.size(), 1u);
    EXPECT_EQ(snapshot2[0].id, "b");
}
