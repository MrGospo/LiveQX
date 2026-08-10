#include "core/ScheduleEntry.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <set>
#include <sstream>
#include <stdexcept>

namespace liveqx::scheduling {

namespace {

[[noreturn]] void bad(const std::string& msg) {
    throw std::invalid_argument(msg);
}

bool isDigits(const std::string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(),
        [](unsigned char c) { return std::isdigit(c) != 0; });
}

TransitionType parseTransitionType(const std::string& s) {
    if (s == "crossfade")  return TransitionType::CrossFade;
    if (s == "wipe_left")  return TransitionType::WipeLeft;
    if (s == "wipe_right") return TransitionType::WipeRight;
    if (s == "wipe_up")    return TransitionType::WipeUp;
    if (s == "wipe_down")  return TransitionType::WipeDown;
    if (s == "push_left")  return TransitionType::PushLeft;
    if (s == "push_right") return TransitionType::PushRight;
    if (s == "push_up")    return TransitionType::PushUp;
    if (s == "push_down")  return TransitionType::PushDown;
    if (s == "dissolve")   return TransitionType::Dissolve;
    if (s == "fade_black" || s == "fade_to_black") return TransitionType::FadeToBlack;
    if (s == "hardcut" || s == "hard_cut") return TransitionType::HardCut;
    bad("unknown transition.type: '" + s + "'");
}

TransitionMode parseTransitionMode(const std::string& s) {
    if (s == "hardcut" || s == "hard_cut") return TransitionMode::HardCut;
    if (s == "live_mix" || s == "live")    return TransitionMode::LiveMix;
    if (s == "freeze_fade")                return TransitionMode::FreezeFade;
    bad("unknown transition.mode: '" + s + "'");
}

const char* transitionTypeName(TransitionType t) {
    switch (t) {
        case TransitionType::CrossFade: return "crossfade";
        case TransitionType::WipeLeft:  return "wipe_left";
        case TransitionType::WipeRight: return "wipe_right";
        case TransitionType::WipeUp:    return "wipe_up";
        case TransitionType::WipeDown:  return "wipe_down";
        case TransitionType::PushLeft:  return "push_left";
        case TransitionType::PushRight: return "push_right";
        case TransitionType::PushUp:    return "push_up";
        case TransitionType::PushDown:  return "push_down";
        case TransitionType::Dissolve:    return "dissolve";
        case TransitionType::FadeToBlack: return "fade_black";
        case TransitionType::HardCut:     return "hardcut";
    }
    return "hardcut";
}

Easing parseEasing(const std::string& s) {
    if (s == "linear")       return Easing::Linear;
    if (s == "ease_in")      return Easing::EaseIn;
    if (s == "ease_out")     return Easing::EaseOut;
    if (s == "ease_in_out")  return Easing::EaseInOut;
    bad("unknown transition.easing: '" + s +
        "' (expected linear|ease_in|ease_out|ease_in_out)");
}

const char* easingName(Easing e) {
    switch (e) {
        case Easing::Linear:    return "linear";
        case Easing::EaseIn:    return "ease_in";
        case Easing::EaseOut:   return "ease_out";
        case Easing::EaseInOut: return "ease_in_out";
    }
    return "linear";
}

const char* transitionModeName(TransitionMode m) {
    switch (m) {
        case TransitionMode::HardCut:    return "hard_cut";
        case TransitionMode::FreezeFade: return "freeze_fade";
        case TransitionMode::LiveMix:    return "live_mix";
    }
    return "freeze_fade";
}

LoopMode parseLoopMode(const std::string& s) {
    if (s == "loop")                return LoopMode::Loop;
    if (s == "play_once_then_idle") return LoopMode::PlayOnceThenIdle;
    bad("unknown loop_mode: '" + s + "' (expected loop|play_once_then_idle)");
}

const char* loopModeName(LoopMode m) {
    return m == LoopMode::Loop ? "loop" : "play_once_then_idle";
}

RecurrenceKind parseRecurrenceKind(const std::string& s) {
    if (s == "once")    return RecurrenceKind::Once;
    if (s == "daily")   return RecurrenceKind::Daily;
    if (s == "weekly")  return RecurrenceKind::Weekly;
    if (s == "monthly") return RecurrenceKind::Monthly;
    bad("unknown recurrence.kind: '" + s + "' (expected once|daily|weekly|monthly)");
}

const char* recurrenceKindName(RecurrenceKind k) {
    switch (k) {
        case RecurrenceKind::Once:    return "once";
        case RecurrenceKind::Daily:   return "daily";
        case RecurrenceKind::Weekly:  return "weekly";
        case RecurrenceKind::Monthly: return "monthly";
    }
    return "once";
}

}  // namespace

TimeOfDay parseTimeOfDay(const std::string& hhmm) {
    if (hhmm.size() != 5 || hhmm[2] != ':')
        bad("bad time format: '" + hhmm + "' (expected HH:MM)");
    const auto h = hhmm.substr(0, 2), m = hhmm.substr(3, 2);
    if (!isDigits(h) || !isDigits(m))
        bad("bad time format: '" + hhmm + "' (non-digit)");
    TimeOfDay t;
    t.hours = std::stoi(h);
    t.minutes = std::stoi(m);
    if (t.hours < 0 || t.hours > 23 || t.minutes < 0 || t.minutes > 59)
        bad("time out of range: '" + hhmm + "'");
    return t;
}

DateOnly parseDateOnly(const std::string& iso_date) {
    // YYYY-MM-DD
    if (iso_date.size() != 10 || iso_date[4] != '-' || iso_date[7] != '-')
        bad("bad date format: '" + iso_date + "' (expected YYYY-MM-DD)");
    const auto y = iso_date.substr(0, 4), m = iso_date.substr(5, 2),
               d = iso_date.substr(8, 2);
    if (!isDigits(y) || !isDigits(m) || !isDigits(d))
        bad("bad date format: '" + iso_date + "' (non-digit)");
    DateOnly out;
    out.year  = std::stoi(y);
    out.month = std::stoi(m);
    out.day   = std::stoi(d);
    if (out.month < 1 || out.month > 12 || out.day < 1 || out.day > 31)
        bad("date out of range: '" + iso_date + "'");
    return out;
}

int64_t parseIsoUtcNs(const std::string& iso) {
    // YYYY-MM-DDThh:mm:ssZ — strict, UTC only. Fractional seconds not supported
    // (we only need second precision for window edges).
    if (iso.size() < 20 || iso[4] != '-' || iso[7] != '-' ||
        iso[10] != 'T' || iso[13] != ':' || iso[16] != ':' ||
        (iso.back() != 'Z' && iso.size() != 20))
        bad("bad UTC datetime: '" + iso + "' (expected YYYY-MM-DDThh:mm:ssZ)");

    std::tm tm{};
    tm.tm_year = std::stoi(iso.substr(0, 4)) - 1900;
    tm.tm_mon  = std::stoi(iso.substr(5, 2)) - 1;
    tm.tm_mday = std::stoi(iso.substr(8, 2));
    tm.tm_hour = std::stoi(iso.substr(11, 2));
    tm.tm_min  = std::stoi(iso.substr(14, 2));
    tm.tm_sec  = std::stoi(iso.substr(17, 2));

    if (tm.tm_mon < 0 || tm.tm_mon > 11 ||
        tm.tm_mday < 1 || tm.tm_mday > 31 ||
        tm.tm_hour < 0 || tm.tm_hour > 23 ||
        tm.tm_min  < 0 || tm.tm_min  > 59 ||
        tm.tm_sec  < 0 || tm.tm_sec  > 60)
        bad("UTC datetime field out of range: '" + iso + "'");

    // timegm = UTC mktime, no DST, no tzdata lookup.
    const std::time_t t = ::timegm(&tm);
    if (t == static_cast<std::time_t>(-1))
        bad("invalid UTC datetime: '" + iso + "'");
    return static_cast<int64_t>(t) * 1'000'000'000LL;
}

ScheduleEntry parseEntry(const nlohmann::json& j) {
    if (!j.is_object()) bad("schedule entry must be an object");

    ScheduleEntry e;

    // id
    if (!j.contains("id") || !j["id"].is_string() || j["id"].get<std::string>().empty())
        bad("schedule entry missing non-empty 'id'");
    e.id = j["id"].get<std::string>();

    // playlist
    if (!j.contains("playlist") || !j["playlist"].is_array() || j["playlist"].empty())
        bad("schedule entry '" + e.id + "': 'playlist' must be a non-empty array");
    for (const auto& p : j["playlist"]) {
        if (!p.is_string() || p.get<std::string>().empty())
            bad("schedule entry '" + e.id + "': playlist item must be a non-empty string");
        e.playlist.push_back(p.get<std::string>());
    }

    // transition (optional, defaults to crossfade/freeze_fade/2.0)
    if (j.contains("transition")) {
        const auto& t = j["transition"];
        if (!t.is_object())
            bad("schedule entry '" + e.id + "': 'transition' must be an object");
        if (t.contains("type"))
            e.transition.type = parseTransitionType(t["type"].get<std::string>());
        if (t.contains("mode"))
            e.transition.mode = parseTransitionMode(t["mode"].get<std::string>());
        if (t.contains("duration")) {
            if (!t["duration"].is_number())
                bad("schedule entry '" + e.id + "': transition.duration must be a number");
            e.transition.duration_sec = t["duration"].get<double>();
            if (e.transition.duration_sec < 0.0)
                bad("schedule entry '" + e.id + "': transition.duration must be >= 0");
        }
        if (t.contains("easing"))
            e.transition.easing = parseEasing(t["easing"].get<std::string>());
    }

    // recurrence (required)
    if (!j.contains("recurrence") || !j["recurrence"].is_object())
        bad("schedule entry '" + e.id + "': missing 'recurrence' object");
    const auto& r = j["recurrence"];
    if (!r.contains("kind") || !r["kind"].is_string())
        bad("schedule entry '" + e.id + "': recurrence.kind missing");
    e.recurrence.kind = parseRecurrenceKind(r["kind"].get<std::string>());

    if (e.recurrence.kind == RecurrenceKind::Once) {
        if (!r.contains("start_at") || !r.contains("end_at"))
            bad("schedule entry '" + e.id + "': 'once' requires start_at and end_at");
        e.recurrence.start_at_ns = parseIsoUtcNs(r["start_at"].get<std::string>());
        e.recurrence.end_at_ns   = parseIsoUtcNs(r["end_at"].get<std::string>());
        if (e.recurrence.end_at_ns <= e.recurrence.start_at_ns)
            bad("schedule entry '" + e.id + "': end_at must be > start_at");
    } else {
        if (!r.contains("start_time") || !r.contains("end_time"))
            bad("schedule entry '" + e.id + "': '" +
                std::string(recurrenceKindName(e.recurrence.kind)) +
                "' requires start_time and end_time");
        e.recurrence.start_time = parseTimeOfDay(r["start_time"].get<std::string>());
        e.recurrence.end_time   = parseTimeOfDay(r["end_time"].get<std::string>());
        if (e.recurrence.start_time.seconds_since_midnight() >=
            e.recurrence.end_time.seconds_since_midnight())
            bad("schedule entry '" + e.id +
                "': start_time must be < end_time (overnight windows not yet supported)");

        if (e.recurrence.kind == RecurrenceKind::Weekly) {
            if (!r.contains("days_of_week") || !r["days_of_week"].is_array() ||
                r["days_of_week"].empty())
                bad("schedule entry '" + e.id +
                    "': 'weekly' requires non-empty days_of_week");
            std::set<int> seen;
            for (const auto& d : r["days_of_week"]) {
                if (!d.is_number_integer())
                    bad("schedule entry '" + e.id +
                        "': days_of_week items must be integers");
                const int v = d.get<int>();
                if (v < 1 || v > 7)
                    bad("schedule entry '" + e.id +
                        "': days_of_week values must be 1..7 (1=Mon)");
                if (!seen.insert(v).second)
                    bad("schedule entry '" + e.id +
                        "': days_of_week contains duplicate " + std::to_string(v));
                e.recurrence.days_of_week.push_back(v);
            }
            std::sort(e.recurrence.days_of_week.begin(), e.recurrence.days_of_week.end());
        }

        if (e.recurrence.kind == RecurrenceKind::Monthly) {
            if (!r.contains("days_of_month") || !r["days_of_month"].is_array() ||
                r["days_of_month"].empty())
                bad("schedule entry '" + e.id +
                    "': 'monthly' requires non-empty days_of_month");
            std::set<int> seen;
            for (const auto& d : r["days_of_month"]) {
                if (!d.is_number_integer())
                    bad("schedule entry '" + e.id +
                        "': days_of_month items must be integers");
                const int v = d.get<int>();
                if (v < 1 || v > 31)
                    bad("schedule entry '" + e.id +
                        "': days_of_month values must be 1..31");
                if (!seen.insert(v).second)
                    bad("schedule entry '" + e.id +
                        "': days_of_month contains duplicate " + std::to_string(v));
                e.recurrence.days_of_month.push_back(v);
            }
            std::sort(e.recurrence.days_of_month.begin(), e.recurrence.days_of_month.end());
        }
    }

    // effective_from / effective_to (optional)
    if (j.contains("effective_from"))
        e.effective_from = parseDateOnly(j["effective_from"].get<std::string>());
    if (j.contains("effective_to"))
        e.effective_to = parseDateOnly(j["effective_to"].get<std::string>());
    if (e.effective_from && e.effective_to &&
        !(*e.effective_from <= *e.effective_to))
        bad("schedule entry '" + e.id + "': effective_from must be <= effective_to");

    // priority (optional, default 100, range 0..1000)
    if (j.contains("priority")) {
        if (!j["priority"].is_number_integer())
            bad("schedule entry '" + e.id + "': priority must be an integer");
        e.priority = j["priority"].get<int>();
        if (e.priority < 0 || e.priority > 1000)
            bad("schedule entry '" + e.id + "': priority must be 0..1000");
    }

    // loop_mode (optional)
    if (j.contains("loop_mode"))
        e.loop_mode = parseLoopMode(j["loop_mode"].get<std::string>());

    // hard_switch (optional)
    if (j.contains("hard_switch")) {
        if (!j["hard_switch"].is_boolean())
            bad("schedule entry '" + e.id + "': hard_switch must be boolean");
        e.hard_switch = j["hard_switch"].get<bool>();
    }

    return e;
}

std::vector<ScheduleEntry> parseSchedule(const nlohmann::json& arr) {
    if (!arr.is_array())
        bad("schedule must be an array");
    std::vector<ScheduleEntry> entries;
    entries.reserve(arr.size());
    std::set<std::string> ids;
    for (const auto& item : arr) {
        auto e = parseEntry(item);
        if (!ids.insert(e.id).second)
            bad("duplicate schedule entry id: '" + e.id + "'");
        entries.push_back(std::move(e));
    }
    return entries;
}

nlohmann::json serializeEntry(const ScheduleEntry& e) {
    nlohmann::json out;
    out["id"]       = e.id;
    out["playlist"] = e.playlist;
    out["transition"] = {
        {"type",     transitionTypeName(e.transition.type)},
        {"mode",     transitionModeName(e.transition.mode)},
        {"duration", e.transition.duration_sec},
        {"easing",   easingName(e.transition.easing)},
    };

    auto fmtIso = [](int64_t unix_ns) {
        const std::time_t t = static_cast<std::time_t>(unix_ns / 1'000'000'000LL);
        std::tm tm{};
        ::gmtime_r(&t, &tm);
        char buf[64];  // 21 needed; oversized to silence -Wformat-truncation
        std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                      tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                      tm.tm_hour, tm.tm_min, tm.tm_sec);
        return std::string(buf);
    };

    nlohmann::json r;
    r["kind"] = recurrenceKindName(e.recurrence.kind);
    if (e.recurrence.kind == RecurrenceKind::Once) {
        r["start_at"] = fmtIso(e.recurrence.start_at_ns);
        r["end_at"]   = fmtIso(e.recurrence.end_at_ns);
    } else {
        char buf[6];
        std::snprintf(buf, sizeof(buf), "%02d:%02d",
                      e.recurrence.start_time.hours,
                      e.recurrence.start_time.minutes);
        r["start_time"] = std::string(buf);
        std::snprintf(buf, sizeof(buf), "%02d:%02d",
                      e.recurrence.end_time.hours,
                      e.recurrence.end_time.minutes);
        r["end_time"] = std::string(buf);
        if (e.recurrence.kind == RecurrenceKind::Weekly)
            r["days_of_week"] = e.recurrence.days_of_week;
        if (e.recurrence.kind == RecurrenceKind::Monthly)
            r["days_of_month"] = e.recurrence.days_of_month;
    }
    out["recurrence"] = r;

    auto fmtDate = [](const DateOnly& d) {
        char buf[11];
        std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", d.year, d.month, d.day);
        return std::string(buf);
    };
    if (e.effective_from) out["effective_from"] = fmtDate(*e.effective_from);
    if (e.effective_to)   out["effective_to"]   = fmtDate(*e.effective_to);

    out["priority"]    = e.priority;
    out["loop_mode"]   = loopModeName(e.loop_mode);
    out["hard_switch"] = e.hard_switch;
    return out;
}

nlohmann::json serializeSchedule(const std::vector<ScheduleEntry>& entries) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& e : entries) arr.push_back(serializeEntry(e));
    return arr;
}

}  // namespace liveqx::scheduling
