#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "transitions/ITransition.h"

namespace liveqx::scheduling {

enum class RecurrenceKind { Once, Daily, Weekly, Monthly };

enum class LoopMode { Loop, PlayOnceThenIdle };

// HH:MM in channel-local time. seconds_since_midnight() folds it into
// a single sortable scalar.
struct TimeOfDay {
    int hours   = 0;
    int minutes = 0;

    int seconds_since_midnight() const noexcept { return hours * 3600 + minutes * 60; }
};

// YYYY-MM-DD calendar date in channel-local time. Used only for the optional
// effective_from/effective_to gates; never combined with a time.
struct DateOnly {
    int year  = 0;
    int month = 0;
    int day   = 0;

    bool operator<=(const DateOnly& other) const noexcept {
        if (year  != other.year)  return year  < other.year;
        if (month != other.month) return month < other.month;
        return day <= other.day;
    }
};

struct Recurrence {
    RecurrenceKind kind = RecurrenceKind::Once;

    // Used for kind == Once. Inclusive start, exclusive end. UTC.
    int64_t start_at_ns = 0;
    int64_t end_at_ns   = 0;

    // Used for kind == Daily | Weekly | Monthly. Window in channel-local
    // time-of-day. end_time may be < start_time to mean "wraps over midnight"
    // — currently rejected by validate(), reserved for future.
    TimeOfDay start_time;
    TimeOfDay end_time;

    // Weekly only. 1=Mon … 7=Sun (ISO 8601). Non-empty after parsing.
    std::vector<int> days_of_week;

    // Monthly only. 1..31. Days that don't exist in a given month (Feb 31)
    // are silently skipped at decide() time.
    std::vector<int> days_of_month;
};

struct ScheduleEntry {
    std::string              id;
    std::vector<std::string> playlist;     // clip paths, resolved by ClipFactory at activation
    TransitionConfig         transition;
    Recurrence               recurrence;

    // Optional gates on top of the recurrence — entry is dormant outside
    // [effective_from, effective_to]. Both are inclusive.
    std::optional<DateOnly> effective_from;
    std::optional<DateOnly> effective_to;

    int      priority    = 100;            // 0..1000, higher wins
    LoopMode loop_mode   = LoopMode::Loop;
    bool     hard_switch = false;          // cut the currently-playing clip on window start
};

// Throws std::invalid_argument with a human-readable message describing the
// offending field. Caller (REST handler) maps to HTTP 400.
ScheduleEntry parseEntry(const nlohmann::json& j);

// Parses an array of entries and validates that ids are unique.
std::vector<ScheduleEntry> parseSchedule(const nlohmann::json& arr);

// Round-trips entries back to JSON. Format matches what parseEntry accepts,
// so PUT → GET → PUT is stable.
nlohmann::json serializeEntry(const ScheduleEntry& e);
nlohmann::json serializeSchedule(const std::vector<ScheduleEntry>& entries);

// Helpers exposed for tests.
TimeOfDay parseTimeOfDay(const std::string& hhmm);     // throws on bad format
DateOnly  parseDateOnly(const std::string& iso_date);  // throws on bad format
int64_t   parseIsoUtcNs(const std::string& iso);       // YYYY-MM-DDThh:mm:ssZ → unix ns

}  // namespace liveqx::scheduling
