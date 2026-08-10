#include <gtest/gtest.h>

#include "core/ScheduleController.h"
#include "core/Scheduler.h"

using namespace liveqx::scheduling;

namespace {

constexpr int64_t kMonday2026May04UtcMidnight = 1777852800LL * 1'000'000'000LL;

ScheduleEntry mkDaily(std::string id,
                      const std::string& start, const std::string& end,
                      int priority = 100) {
    ScheduleEntry e;
    e.id = std::move(id);
    e.playlist = {"a.mp4"};
    e.recurrence.kind        = RecurrenceKind::Daily;
    e.recurrence.start_time  = parseTimeOfDay(start);
    e.recurrence.end_time    = parseTimeOfDay(end);
    e.priority               = priority;
    return e;
}

}  // namespace

TEST(ScheduleController, NoActionWhenAlreadyRegular) {
    Scheduler s({});
    ScheduleController c(s);
    auto a = c.onBoundary(kMonday2026May04UtcMidnight + 9 * 3600 * 1'000'000'000LL);
    EXPECT_EQ(a.kind, ScheduleController::ActionKind::None);
    EXPECT_FALSE(c.inScheduleMode());
}

TEST(ScheduleController, EntersOnFirstActiveWindow) {
    Scheduler s({mkDaily("d", "10:00", "11:00")});
    ScheduleController c(s);

    const int64_t at_10 = kMonday2026May04UtcMidnight + 10 * 3600 * 1'000'000'000LL;
    auto a = c.onBoundary(at_10);
    EXPECT_EQ(a.kind, ScheduleController::ActionKind::EnterSchedule);
    EXPECT_EQ(a.new_entry_id, "d");
    EXPECT_EQ(a.window_end_ns,
              kMonday2026May04UtcMidnight + 11 * 3600 * 1'000'000'000LL);
    EXPECT_TRUE(c.inScheduleMode());
    EXPECT_EQ(c.currentEntryId(), "d");
}

TEST(ScheduleController, NoActionWithinSameWindow) {
    Scheduler s({mkDaily("d", "10:00", "11:00")});
    ScheduleController c(s);

    const int64_t at_10 = kMonday2026May04UtcMidnight + 10 * 3600 * 1'000'000'000LL;
    c.onBoundary(at_10);  // EnterSchedule

    const int64_t at_10_15 = at_10 + 15 * 60 * 1'000'000'000LL;
    auto a = c.onBoundary(at_10_15);
    EXPECT_EQ(a.kind, ScheduleController::ActionKind::None);
    EXPECT_TRUE(c.inScheduleMode());
}

TEST(ScheduleController, ExitsWhenWindowEnds) {
    Scheduler s({mkDaily("d", "10:00", "11:00")});
    ScheduleController c(s);

    const int64_t at_10 = kMonday2026May04UtcMidnight + 10 * 3600 * 1'000'000'000LL;
    c.onBoundary(at_10);

    const int64_t at_11 = kMonday2026May04UtcMidnight + 11 * 3600 * 1'000'000'000LL;
    auto a = c.onBoundary(at_11);
    EXPECT_EQ(a.kind, ScheduleController::ActionKind::ExitToRegular);
    EXPECT_FALSE(c.inScheduleMode());
    EXPECT_EQ(c.currentEntryId(), "");
    EXPECT_EQ(c.currentWindowEndNs(), 0);
}

TEST(ScheduleController, SwitchesBetweenEntries) {
    std::vector<ScheduleEntry> es;
    es.push_back(mkDaily("low",  "10:00", "12:00",  50));
    es.push_back(mkDaily("hi",   "10:30", "11:30", 200));
    Scheduler s(std::move(es));
    ScheduleController c(s);

    const int64_t at_10_15 = kMonday2026May04UtcMidnight + 10 * 3600 * 1'000'000'000LL
                                                         + 15 * 60   * 1'000'000'000LL;
    auto a1 = c.onBoundary(at_10_15);
    EXPECT_EQ(a1.kind, ScheduleController::ActionKind::EnterSchedule);
    EXPECT_EQ(a1.new_entry_id, "low");

    const int64_t at_10_45 = kMonday2026May04UtcMidnight + 10 * 3600 * 1'000'000'000LL
                                                         + 45 * 60   * 1'000'000'000LL;
    auto a2 = c.onBoundary(at_10_45);
    EXPECT_EQ(a2.kind, ScheduleController::ActionKind::SwitchEntry);
    EXPECT_EQ(a2.new_entry_id, "hi");
    EXPECT_EQ(c.currentEntryId(), "hi");

    const int64_t at_11_45 = kMonday2026May04UtcMidnight + 11 * 3600 * 1'000'000'000LL
                                                         + 45 * 60   * 1'000'000'000LL;
    auto a3 = c.onBoundary(at_11_45);
    // hi ended; low still active until 12:00 → switch back to low
    EXPECT_EQ(a3.kind, ScheduleController::ActionKind::SwitchEntry);
    EXPECT_EQ(a3.new_entry_id, "low");
}

TEST(ScheduleController, RefreshesWindowEndOnHotReload) {
    Scheduler s({mkDaily("d", "10:00", "11:00")});
    ScheduleController c(s);

    const int64_t at_10 = kMonday2026May04UtcMidnight + 10 * 3600 * 1'000'000'000LL;
    c.onBoundary(at_10);  // enters; window_end = 11:00
    const int64_t expected_11 = kMonday2026May04UtcMidnight + 11 * 3600 * 1'000'000'000LL;
    EXPECT_EQ(c.currentWindowEndNs(), expected_11);

    // Operator extends end_time → 12:00 via hot-reload
    s.setEntries({mkDaily("d", "10:00", "12:00")});
    const int64_t at_10_05 = at_10 + 5 * 60 * 1'000'000'000LL;
    auto a = c.onBoundary(at_10_05);
    EXPECT_EQ(a.kind, ScheduleController::ActionKind::None);
    const int64_t expected_12 = kMonday2026May04UtcMidnight + 12 * 3600 * 1'000'000'000LL;
    EXPECT_EQ(c.currentWindowEndNs(), expected_12);
}

TEST(ScheduleController, HardSwitchPropagated) {
    auto e = mkDaily("d", "10:00", "11:00");
    e.hard_switch = true;
    Scheduler s({e});
    ScheduleController c(s);

    const int64_t at_10 = kMonday2026May04UtcMidnight + 10 * 3600 * 1'000'000'000LL;
    auto a = c.onBoundary(at_10);
    EXPECT_EQ(a.kind, ScheduleController::ActionKind::EnterSchedule);
    EXPECT_TRUE(a.hard_switch);
}

// ─── tryHardSwitch poll path ──────────────────────────────────────────────────

TEST(ScheduleController, HardSwitchPolledFiresEnter) {
    auto e = mkDaily("d", "10:00", "11:00");
    e.hard_switch = true;
    Scheduler s({e});
    ScheduleController c(s);

    const int64_t before = kMonday2026May04UtcMidnight + 9 * 3600 * 1'000'000'000LL;
    EXPECT_EQ(c.tryHardSwitch(before).kind, ScheduleController::ActionKind::None);
    EXPECT_FALSE(c.inScheduleMode());

    const int64_t at_10 = kMonday2026May04UtcMidnight + 10 * 3600 * 1'000'000'000LL;
    auto a = c.tryHardSwitch(at_10);
    EXPECT_EQ(a.kind, ScheduleController::ActionKind::EnterSchedule);
    EXPECT_TRUE(c.inScheduleMode());
    EXPECT_EQ(c.currentEntryId(), "d");
}

TEST(ScheduleController, HardSwitchPolledIgnoresSoftEntries) {
    auto e = mkDaily("d", "10:00", "11:00");  // hard_switch defaults to false
    Scheduler s({e});
    ScheduleController c(s);

    const int64_t at_10 = kMonday2026May04UtcMidnight + 10 * 3600 * 1'000'000'000LL;
    EXPECT_EQ(c.tryHardSwitch(at_10).kind, ScheduleController::ActionKind::None);
    EXPECT_FALSE(c.inScheduleMode());
}

TEST(ScheduleController, HardSwitchPolledNoOpInsideSameEntry) {
    auto e = mkDaily("d", "10:00", "11:00");
    e.hard_switch = true;
    Scheduler s({e});
    ScheduleController c(s);

    const int64_t at_10 = kMonday2026May04UtcMidnight + 10 * 3600 * 1'000'000'000LL;
    c.tryHardSwitch(at_10);  // EnterSchedule

    const int64_t at_10_30 = at_10 + 30 * 60 * 1'000'000'000LL;
    EXPECT_EQ(c.tryHardSwitch(at_10_30).kind, ScheduleController::ActionKind::None);
    EXPECT_TRUE(c.inScheduleMode());
}

TEST(ScheduleController, HardSwitchPolledSwitchesBetweenHardEntries) {
    std::vector<ScheduleEntry> es;
    auto e1 = mkDaily("low",  "10:00", "12:00",  50);
    e1.hard_switch = true;
    auto e2 = mkDaily("hi",   "10:30", "11:30", 200);
    e2.hard_switch = true;
    es.push_back(e1);
    es.push_back(e2);
    Scheduler s(std::move(es));
    ScheduleController c(s);

    const int64_t at_10_15 = kMonday2026May04UtcMidnight + 10 * 3600 * 1'000'000'000LL
                                                         + 15 * 60   * 1'000'000'000LL;
    EXPECT_EQ(c.tryHardSwitch(at_10_15).kind, ScheduleController::ActionKind::EnterSchedule);
    EXPECT_EQ(c.currentEntryId(), "low");

    const int64_t at_10_45 = at_10_15 + 30 * 60 * 1'000'000'000LL;
    auto a = c.tryHardSwitch(at_10_45);
    EXPECT_EQ(a.kind, ScheduleController::ActionKind::SwitchEntry);
    EXPECT_EQ(c.currentEntryId(), "hi");
}
