#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "core/ScheduleEntry.h"

using nlohmann::json;
using namespace liveqx::scheduling;

// ─── Time / date primitive parsers ────────────────────────────────────────────

TEST(ScheduleEntryParse, TimeOfDayValid) {
    auto t = parseTimeOfDay("14:30");
    EXPECT_EQ(t.hours, 14);
    EXPECT_EQ(t.minutes, 30);
    EXPECT_EQ(t.seconds_since_midnight(), 14 * 3600 + 30 * 60);
}

TEST(ScheduleEntryParse, TimeOfDayRejectsBadFormat) {
    EXPECT_THROW(parseTimeOfDay("1:30"),    std::invalid_argument);
    EXPECT_THROW(parseTimeOfDay("14-30"),   std::invalid_argument);
    EXPECT_THROW(parseTimeOfDay("aa:bb"),   std::invalid_argument);
    EXPECT_THROW(parseTimeOfDay("25:00"),   std::invalid_argument);
    EXPECT_THROW(parseTimeOfDay("14:60"),   std::invalid_argument);
    EXPECT_THROW(parseTimeOfDay(""),        std::invalid_argument);
}

TEST(ScheduleEntryParse, DateOnlyValid) {
    auto d = parseDateOnly("2026-05-31");
    EXPECT_EQ(d.year, 2026);
    EXPECT_EQ(d.month, 5);
    EXPECT_EQ(d.day, 31);
}

TEST(ScheduleEntryParse, DateOnlyRejectsBadFormat) {
    EXPECT_THROW(parseDateOnly("2026/05/31"), std::invalid_argument);
    EXPECT_THROW(parseDateOnly("26-5-31"),    std::invalid_argument);
    EXPECT_THROW(parseDateOnly("2026-13-01"), std::invalid_argument);
    EXPECT_THROW(parseDateOnly("2026-12-32"), std::invalid_argument);
}

TEST(ScheduleEntryParse, IsoUtcParsesUnixEpoch) {
    EXPECT_EQ(parseIsoUtcNs("1970-01-01T00:00:00Z"), 0);
    EXPECT_EQ(parseIsoUtcNs("1970-01-01T00:00:01Z"), 1'000'000'000LL);
    // 2026-05-03T00:00:00Z = 1777766400 unix seconds (verified via `date -u`)
    EXPECT_EQ(parseIsoUtcNs("2026-05-03T00:00:00Z"), 1777766400LL * 1'000'000'000LL);
}

TEST(ScheduleEntryParse, IsoUtcRejectsBadFormat) {
    EXPECT_THROW(parseIsoUtcNs("2026-05-03"),              std::invalid_argument);
    EXPECT_THROW(parseIsoUtcNs("2026-05-03 14:00:00Z"),    std::invalid_argument);
    EXPECT_THROW(parseIsoUtcNs("2026-05-03T14:00:00+0300"), std::invalid_argument);
    EXPECT_THROW(parseIsoUtcNs("2026-13-01T00:00:00Z"),    std::invalid_argument);
}

// ─── Entry: minimum viable Once ───────────────────────────────────────────────

TEST(ScheduleEntryParse, OnceMinimal) {
    json j = {
        {"id", "ny-2026"},
        {"playlist", {"specials/ny.mp4"}},
        {"recurrence", {
            {"kind", "once"},
            {"start_at", "2026-12-31T22:00:00Z"},
            {"end_at",   "2027-01-01T02:00:00Z"},
        }},
    };
    auto e = parseEntry(j);
    EXPECT_EQ(e.id, "ny-2026");
    ASSERT_EQ(e.playlist.size(), 1u);
    EXPECT_EQ(e.playlist[0], "specials/ny.mp4");
    EXPECT_EQ(e.recurrence.kind, RecurrenceKind::Once);
    EXPECT_EQ(e.priority, 100);
    EXPECT_EQ(e.loop_mode, LoopMode::Loop);
    EXPECT_FALSE(e.hard_switch);
}

TEST(ScheduleEntryParse, OnceRequiresEndAfterStart) {
    json j = {
        {"id", "x"}, {"playlist", {"a.mp4"}},
        {"recurrence", {
            {"kind", "once"},
            {"start_at", "2026-01-01T00:00:00Z"},
            {"end_at",   "2026-01-01T00:00:00Z"},
        }},
    };
    EXPECT_THROW(parseEntry(j), std::invalid_argument);
}

TEST(ScheduleEntryParse, OnceMissingFieldsRejected) {
    json j = {
        {"id", "x"}, {"playlist", {"a.mp4"}},
        {"recurrence", {{"kind", "once"}}},
    };
    EXPECT_THROW(parseEntry(j), std::invalid_argument);
}

// ─── Entry: weekly with days_of_week ──────────────────────────────────────────

TEST(ScheduleEntryParse, WeeklyValid) {
    json j = {
        {"id", "advert-1430"},
        {"playlist", {"ads/a.mp4", "ads/b.mp4"}},
        {"transition", {{"type", "hardcut"}, {"mode", "hard_cut"}}},
        {"recurrence", {
            {"kind", "weekly"},
            {"days_of_week", {1, 2, 3, 4, 5}},
            {"start_time", "14:00"},
            {"end_time",   "14:30"},
        }},
        {"priority", 200},
        {"loop_mode", "play_once_then_idle"},
        {"hard_switch", true},
    };
    auto e = parseEntry(j);
    EXPECT_EQ(e.recurrence.kind, RecurrenceKind::Weekly);
    EXPECT_EQ(e.recurrence.days_of_week, (std::vector<int>{1, 2, 3, 4, 5}));
    EXPECT_EQ(e.recurrence.start_time.seconds_since_midnight(), 14 * 3600);
    EXPECT_EQ(e.transition.type, TransitionType::HardCut);
    EXPECT_EQ(e.transition.mode, TransitionMode::HardCut);
    EXPECT_EQ(e.priority, 200);
    EXPECT_EQ(e.loop_mode, LoopMode::PlayOnceThenIdle);
    EXPECT_TRUE(e.hard_switch);
}

TEST(ScheduleEntryParse, WeeklyRejectsEmptyDays) {
    json j = {
        {"id", "x"}, {"playlist", {"a.mp4"}},
        {"recurrence", {
            {"kind", "weekly"},
            {"days_of_week", json::array()},
            {"start_time", "14:00"}, {"end_time", "15:00"},
        }},
    };
    EXPECT_THROW(parseEntry(j), std::invalid_argument);
}

TEST(ScheduleEntryParse, WeeklyRejectsBadDayValue) {
    json j = {
        {"id", "x"}, {"playlist", {"a.mp4"}},
        {"recurrence", {
            {"kind", "weekly"},
            {"days_of_week", {0, 8}},
            {"start_time", "14:00"}, {"end_time", "15:00"},
        }},
    };
    EXPECT_THROW(parseEntry(j), std::invalid_argument);
}

TEST(ScheduleEntryParse, WeeklyRejectsDuplicateDay) {
    json j = {
        {"id", "x"}, {"playlist", {"a.mp4"}},
        {"recurrence", {
            {"kind", "weekly"},
            {"days_of_week", {1, 1}},
            {"start_time", "14:00"}, {"end_time", "15:00"},
        }},
    };
    EXPECT_THROW(parseEntry(j), std::invalid_argument);
}

// ─── Entry: monthly ───────────────────────────────────────────────────────────

TEST(ScheduleEntryParse, MonthlyValid) {
    json j = {
        {"id", "billing"}, {"playlist", {"info.mp4"}},
        {"recurrence", {
            {"kind", "monthly"},
            {"days_of_month", {1, 15, 31}},
            {"start_time", "08:00"}, {"end_time", "08:05"},
        }},
    };
    auto e = parseEntry(j);
    EXPECT_EQ(e.recurrence.kind, RecurrenceKind::Monthly);
    EXPECT_EQ(e.recurrence.days_of_month, (std::vector<int>{1, 15, 31}));
}

TEST(ScheduleEntryParse, MonthlyRejectsBadDay) {
    json j = {
        {"id", "x"}, {"playlist", {"a.mp4"}},
        {"recurrence", {
            {"kind", "monthly"},
            {"days_of_month", {32}},
            {"start_time", "08:00"}, {"end_time", "09:00"},
        }},
    };
    EXPECT_THROW(parseEntry(j), std::invalid_argument);
}

// ─── Entry: daily + start/end ordering ────────────────────────────────────────

TEST(ScheduleEntryParse, DailyValid) {
    json j = {
        {"id", "x"}, {"playlist", {"a.mp4"}},
        {"recurrence", {
            {"kind", "daily"},
            {"start_time", "06:00"}, {"end_time", "07:00"},
        }},
    };
    auto e = parseEntry(j);
    EXPECT_EQ(e.recurrence.kind, RecurrenceKind::Daily);
}

TEST(ScheduleEntryParse, RejectsStartGEEnd) {
    json j = {
        {"id", "x"}, {"playlist", {"a.mp4"}},
        {"recurrence", {
            {"kind", "daily"},
            {"start_time", "10:00"}, {"end_time", "10:00"},
        }},
    };
    EXPECT_THROW(parseEntry(j), std::invalid_argument);

    j["recurrence"]["end_time"] = "09:59";
    EXPECT_THROW(parseEntry(j), std::invalid_argument);
}

// ─── Effective range ──────────────────────────────────────────────────────────

TEST(ScheduleEntryParse, EffectiveRangeValid) {
    json j = {
        {"id", "x"}, {"playlist", {"a.mp4"}},
        {"recurrence", {
            {"kind", "daily"},
            {"start_time", "10:00"}, {"end_time", "11:00"},
        }},
        {"effective_from", "2026-01-01"},
        {"effective_to",   "2026-12-31"},
    };
    auto e = parseEntry(j);
    ASSERT_TRUE(e.effective_from.has_value());
    ASSERT_TRUE(e.effective_to.has_value());
    EXPECT_EQ(e.effective_from->year, 2026);
    EXPECT_EQ(e.effective_to->month, 12);
}

TEST(ScheduleEntryParse, EffectiveRangeRejectsInverted) {
    json j = {
        {"id", "x"}, {"playlist", {"a.mp4"}},
        {"recurrence", {
            {"kind", "daily"},
            {"start_time", "10:00"}, {"end_time", "11:00"},
        }},
        {"effective_from", "2026-12-31"},
        {"effective_to",   "2026-01-01"},
    };
    EXPECT_THROW(parseEntry(j), std::invalid_argument);
}

// ─── Required fields ──────────────────────────────────────────────────────────

TEST(ScheduleEntryParse, EmptyPlaylistRejected) {
    json j = {
        {"id", "x"}, {"playlist", json::array()},
        {"recurrence", {
            {"kind", "daily"},
            {"start_time", "10:00"}, {"end_time", "11:00"},
        }},
    };
    EXPECT_THROW(parseEntry(j), std::invalid_argument);
}

TEST(ScheduleEntryParse, MissingIdRejected) {
    json j = {
        {"playlist", {"a.mp4"}},
        {"recurrence", {
            {"kind", "daily"},
            {"start_time", "10:00"}, {"end_time", "11:00"},
        }},
    };
    EXPECT_THROW(parseEntry(j), std::invalid_argument);
}

TEST(ScheduleEntryParse, PriorityOutOfRangeRejected) {
    json j = {
        {"id", "x"}, {"playlist", {"a.mp4"}},
        {"recurrence", {
            {"kind", "daily"},
            {"start_time", "10:00"}, {"end_time", "11:00"},
        }},
        {"priority", 1001},
    };
    EXPECT_THROW(parseEntry(j), std::invalid_argument);
}

// ─── parseSchedule (array) ────────────────────────────────────────────────────

TEST(ScheduleEntryParse, ArrayParsesMultiple) {
    json arr = json::array({
        {
            {"id", "a"}, {"playlist", {"x.mp4"}},
            {"recurrence", {
                {"kind", "daily"},
                {"start_time", "10:00"}, {"end_time", "11:00"},
            }},
        },
        {
            {"id", "b"}, {"playlist", {"y.mp4"}},
            {"recurrence", {
                {"kind", "daily"},
                {"start_time", "12:00"}, {"end_time", "13:00"},
            }},
        },
    });
    auto out = parseSchedule(arr);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].id, "a");
    EXPECT_EQ(out[1].id, "b");
}

TEST(ScheduleEntryParse, ArrayRejectsDuplicateIds) {
    json arr = json::array({
        {
            {"id", "dup"}, {"playlist", {"x.mp4"}},
            {"recurrence", {
                {"kind", "daily"},
                {"start_time", "10:00"}, {"end_time", "11:00"},
            }},
        },
        {
            {"id", "dup"}, {"playlist", {"y.mp4"}},
            {"recurrence", {
                {"kind", "daily"},
                {"start_time", "12:00"}, {"end_time", "13:00"},
            }},
        },
    });
    EXPECT_THROW(parseSchedule(arr), std::invalid_argument);
}

// ─── Round-trip serialize → parse ────────────────────────────────────────────

TEST(ScheduleEntrySerialize, OnceRoundTrip) {
    json j = {
        {"id", "ny-2026"}, {"playlist", {"specials/ny.mp4"}},
        {"recurrence", {
            {"kind", "once"},
            {"start_at", "2026-12-31T22:00:00Z"},
            {"end_at",   "2027-01-01T02:00:00Z"},
        }},
        {"priority", 200},
    };
    auto e1 = parseEntry(j);
    auto j2 = serializeEntry(e1);
    auto e2 = parseEntry(j2);
    EXPECT_EQ(e2.id, e1.id);
    EXPECT_EQ(e2.recurrence.start_at_ns, e1.recurrence.start_at_ns);
    EXPECT_EQ(e2.recurrence.end_at_ns,   e1.recurrence.end_at_ns);
    EXPECT_EQ(e2.priority, e1.priority);
}

// fix20 — push_{left,right,up,down} transition strings round-trip through
// both the parser and the serializer. parseTransitionType/transitionTypeName
// must agree on the spelling so the schedule survives a save/restore cycle.
TEST(ScheduleEntryParse, PushTransitionTypesRoundTrip) {
    const std::pair<std::string, TransitionType> cases[] = {
        {"push_left",  TransitionType::PushLeft},
        {"push_right", TransitionType::PushRight},
        {"push_up",    TransitionType::PushUp},
        {"push_down",  TransitionType::PushDown},
    };
    for (const auto& [name, expected] : cases) {
        json j = {
            {"id", "feed"}, {"playlist", {"a.mp4"}},
            {"transition", {{"type", name}, {"duration", 0.5}}},
            {"recurrence", {
                {"kind", "once"},
                {"start_at", "2026-05-04T20:00:00Z"},
                {"end_at",   "2026-05-04T21:00:00Z"},
            }},
        };
        auto e = parseEntry(j);
        EXPECT_EQ(e.transition.type, expected) << "parse: " << name;

        auto round = parseEntry(serializeEntry(e));
        EXPECT_EQ(round.transition.type, expected) << "round-trip: " << name;
    }
}

// fix20 — easing field round-trips through parser/serializer. Default
// (no field on the JSON) yields Linear; explicit values map by name.
TEST(ScheduleEntryParse, EasingRoundTrip) {
    const std::pair<std::string, Easing> cases[] = {
        {"linear",      Easing::Linear},
        {"ease_in",     Easing::EaseIn},
        {"ease_out",    Easing::EaseOut},
        {"ease_in_out", Easing::EaseInOut},
    };
    for (const auto& [name, expected] : cases) {
        json j = {
            {"id", "feed"}, {"playlist", {"a.mp4"}},
            {"transition", {{"type", "crossfade"}, {"duration", 0.5},
                            {"easing", name}}},
            {"recurrence", {
                {"kind", "once"},
                {"start_at", "2026-05-04T20:00:00Z"},
                {"end_at",   "2026-05-04T21:00:00Z"},
            }},
        };
        auto e = parseEntry(j);
        EXPECT_EQ(e.transition.easing, expected) << "parse: " << name;

        auto round = parseEntry(serializeEntry(e));
        EXPECT_EQ(round.transition.easing, expected) << "round-trip: " << name;
    }
}

TEST(ScheduleEntryParse, EasingDefaultsToLinear) {
    json j = {
        {"id", "feed"}, {"playlist", {"a.mp4"}},
        {"transition", {{"type", "crossfade"}, {"duration", 0.5}}},
        {"recurrence", {
            {"kind", "once"},
            {"start_at", "2026-05-04T20:00:00Z"},
            {"end_at",   "2026-05-04T21:00:00Z"},
        }},
    };
    auto e = parseEntry(j);
    EXPECT_EQ(e.transition.easing, Easing::Linear);
}

TEST(ScheduleEntryParse, EasingRejectsUnknownName) {
    json j = {
        {"id", "feed"}, {"playlist", {"a.mp4"}},
        {"transition", {{"type", "crossfade"}, {"easing", "bouncy"}}},
        {"recurrence", {
            {"kind", "once"},
            {"start_at", "2026-05-04T20:00:00Z"},
            {"end_at",   "2026-05-04T21:00:00Z"},
        }},
    };
    EXPECT_THROW(parseEntry(j), std::invalid_argument);
}

// fix13 c7 — fade_black transition string round-trips through the parser.
TEST(ScheduleEntryParse, FadeBlackTransitionType) {
    json j1 = {
        {"id", "feed"}, {"playlist", {"a.mp4"}},
        {"transition", {{"type", "fade_black"}, {"duration", 1.0}}},
        {"recurrence", {
            {"kind", "once"},
            {"start_at", "2026-05-04T20:00:00Z"},
            {"end_at",   "2026-05-04T21:00:00Z"},
        }},
    };
    auto e = parseEntry(j1);
    EXPECT_EQ(e.transition.type, TransitionType::FadeToBlack);

    // Long-form spelling is also accepted
    json j2 = j1;
    j2["transition"]["type"] = "fade_to_black";
    auto e2 = parseEntry(j2);
    EXPECT_EQ(e2.transition.type, TransitionType::FadeToBlack);

    // Round-trip via serialize/parse
    auto round = parseEntry(serializeEntry(e));
    EXPECT_EQ(round.transition.type, TransitionType::FadeToBlack);
}

TEST(ScheduleEntrySerialize, WeeklyRoundTrip) {
    json j = {
        {"id", "advert"}, {"playlist", {"a.mp4", "b.mp4"}},
        {"transition", {{"type", "crossfade"}, {"mode", "live_mix"}, {"duration", 1.5}}},
        {"recurrence", {
            {"kind", "weekly"},
            {"days_of_week", {1, 3, 5}},
            {"start_time", "09:30"}, {"end_time", "10:00"},
        }},
        {"effective_from", "2026-02-01"},
        {"hard_switch", true},
    };
    auto e1 = parseEntry(j);
    auto j2 = serializeEntry(e1);
    auto e2 = parseEntry(j2);
    EXPECT_EQ(e2.playlist, e1.playlist);
    EXPECT_EQ(e2.transition.duration_sec, 1.5);
    EXPECT_EQ(e2.transition.mode, TransitionMode::LiveMix);
    EXPECT_EQ(e2.recurrence.days_of_week, e1.recurrence.days_of_week);
    EXPECT_TRUE(e2.hard_switch);
    EXPECT_EQ(e2.effective_from->month, 2);
}
