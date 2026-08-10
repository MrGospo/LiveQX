#include "core/Scheduler.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include <date/tz.h>

namespace liveqx::scheduling {

namespace {

constexpr std::int64_t kSecondsPerDay = 86400;
constexpr std::int64_t kNanosPerSec   = 1'000'000'000LL;

// fix33 B — IANA-aware local-time calendar.
//
// Раньше Scheduler пользовался localtime_r/mktime, что привязывало решения
// к OS-локали процесса. Теперь HH:MM интерпретируются в channel_tz через
// date::zoned_time из tzdata. UTC, IANA-имя или невалидное → UTC
// (валидация перенесена на caller'а, см. isValidIanaTimezone).
//
// Используется Howard Hinnant date (`<date/tz.h>`), а не std::chrono tzdb,
// потому что libstdc++ 12 (Debian 12 / Ubuntu 22.04) tzdb ещё не имеет.
// API 1-в-1 с std::chrono: time_zone/locate_zone/to_local/to_sys/choose.
const date::time_zone* resolveTz(const std::string& name) {
    if (name.empty()) {
        try { return date::locate_zone("UTC"); }
        catch (const std::runtime_error&) { return nullptr; }
    }
    try { return date::locate_zone(name); }
    catch (const std::runtime_error&) {
        try { return date::locate_zone("UTC"); }
        catch (const std::runtime_error&) { return nullptr; }
    }
}

using SysNs   = date::sys_time<std::chrono::nanoseconds>;
using LocalNs = date::local_time<std::chrono::nanoseconds>;

SysNs nsToSys(std::int64_t ns) {
    return SysNs{std::chrono::nanoseconds{ns}};
}

struct LocalCal {
    date::year_month_day ymd;
    int  iso_weekday{};                // 1=Mon … 7=Sun
};

LocalCal localCalendar(std::int64_t now_ns, const date::time_zone* tz) {
    const auto sys = nsToSys(now_ns);
    if (!tz) {
        // tzdata недоступна — fallback на UTC через sys_days напрямую.
        const auto days = std::chrono::floor<date::days>(sys);
        const date::year_month_day ymd{days};
        const date::weekday wd{days};
        return {ymd, static_cast<int>(wd.iso_encoding())};
    }
    const auto local = tz->to_local(sys);
    const auto local_days = std::chrono::floor<date::days>(local);
    const date::year_month_day ymd{local_days};
    const date::weekday wd{local_days};
    return {ymd, static_cast<int>(wd.iso_encoding())};
}

// Unix-ns начала локального дня, который содержит now_ns. Для DST-границ
// берём choose::earliest — нормальная ситуация это single offset, на edge'ах
// (ambiguous fall-back / nonexistent spring-forward) выбираем ранний вариант.
std::int64_t localMidnightNs(std::int64_t now_ns,
                             const date::time_zone* tz) {
    const auto cal = localCalendar(now_ns, tz);
    const date::local_days local_midnight{cal.ymd};
    if (!tz) {
        return date::sys_seconds{
                   date::sys_days{cal.ymd}}
                   .time_since_epoch().count() *
               kNanosPerSec;
    }
    const auto sys = tz->to_sys(local_midnight, date::choose::earliest);
    return std::chrono::time_point_cast<std::chrono::seconds>(sys)
               .time_since_epoch().count() *
           kNanosPerSec;
}

bool effectiveRangeOk(const ScheduleEntry& e, const LocalCal& cal) {
    DateOnly today{
        static_cast<int>(cal.ymd.year()),
        static_cast<int>(static_cast<unsigned>(cal.ymd.month())),
        static_cast<int>(static_cast<unsigned>(cal.ymd.day()))};
    if (e.effective_from && !(*e.effective_from <= today)) return false;
    if (e.effective_to   && !(today <= *e.effective_to))   return false;
    return true;
}

struct Match { bool in_window = false; std::int64_t window_end_ns = 0; };

bool dayMatchesRecurrence(const Recurrence& r, const LocalCal& cal) {
    switch (r.kind) {
        case RecurrenceKind::Once:
            return true;  // not used — once is matched purely by absolute ns.
        case RecurrenceKind::Daily:
            return true;
        case RecurrenceKind::Weekly: {
            return std::find(r.days_of_week.begin(), r.days_of_week.end(),
                             cal.iso_weekday) != r.days_of_week.end();
        }
        case RecurrenceKind::Monthly: {
            const int dom = static_cast<int>(static_cast<unsigned>(cal.ymd.day()));
            return std::find(r.days_of_month.begin(), r.days_of_month.end(), dom)
                   != r.days_of_month.end();
        }
    }
    return false;
}

Match matchEntry(const ScheduleEntry& e, std::int64_t now_ns,
                 const date::time_zone* tz) {
    if (e.recurrence.kind == RecurrenceKind::Once) {
        if (now_ns >= e.recurrence.start_at_ns &&
            now_ns <  e.recurrence.end_at_ns) {
            const auto cal = localCalendar(now_ns, tz);
            if (!effectiveRangeOk(e, cal)) return {};
            return {true, e.recurrence.end_at_ns};
        }
        return {};
    }

    const auto cal = localCalendar(now_ns, tz);
    if (!effectiveRangeOk(e, cal)) return {};
    if (!dayMatchesRecurrence(e.recurrence, cal)) return {};

    const std::int64_t midnight = localMidnightNs(now_ns, tz);
    const std::int64_t start_ns = midnight +
        static_cast<std::int64_t>(e.recurrence.start_time.seconds_since_midnight()) * kNanosPerSec;
    const std::int64_t end_ns = midnight +
        static_cast<std::int64_t>(e.recurrence.end_time.seconds_since_midnight()) * kNanosPerSec;

    if (now_ns >= start_ns && now_ns < end_ns) return {true, end_ns};
    return {};
}

struct FutureWindow {
    std::int64_t start_ns = 0;
    std::int64_t end_ns   = 0;
};

std::optional<FutureWindow> nextActivation(const ScheduleEntry& e,
                                           std::int64_t now_ns,
                                           std::int64_t horizon_ns,
                                           const date::time_zone* tz) {
    if (e.recurrence.kind == RecurrenceKind::Once) {
        if (e.recurrence.start_at_ns > now_ns &&
            e.recurrence.start_at_ns - now_ns <= horizon_ns) {
            const auto cal = localCalendar(e.recurrence.start_at_ns, tz);
            if (!effectiveRangeOk(e, cal)) return std::nullopt;
            return FutureWindow{e.recurrence.start_at_ns, e.recurrence.end_at_ns};
        }
        return std::nullopt;
    }

    const std::int64_t midnight_today = localMidnightNs(now_ns, tz);
    const std::int64_t start_off_ns =
        static_cast<std::int64_t>(e.recurrence.start_time.seconds_since_midnight()) * kNanosPerSec;
    const std::int64_t end_off_ns =
        static_cast<std::int64_t>(e.recurrence.end_time.seconds_since_midnight()) * kNanosPerSec;

    for (int day_idx = 0; day_idx < 400; ++day_idx) {
        // Берём midnight через локальную математику — добавление 24h может
        // съезжать в DST-зонах. Для теста-grade корректности перечитываем
        // local-midnight по сдвинутому instant'у.
        const std::int64_t probe_ns =
            midnight_today + day_idx * kSecondsPerDay * kNanosPerSec;
        const std::int64_t midnight = localMidnightNs(probe_ns, tz);
        if (midnight - now_ns > horizon_ns + kSecondsPerDay * kNanosPerSec)
            return std::nullopt;
        const std::int64_t start_ns = midnight + start_off_ns;
        if (start_ns <= now_ns) continue;
        if (start_ns - now_ns > horizon_ns) return std::nullopt;

        const auto cal = localCalendar(midnight, tz);
        if (!effectiveRangeOk(e, cal)) continue;
        if (!dayMatchesRecurrence(e.recurrence, cal)) continue;
        return FutureWindow{start_ns, midnight + end_off_ns};
    }
    return std::nullopt;
}

}  // namespace

Scheduler::Scheduler(std::vector<ScheduleEntry> entries, std::string channel_tz)
    : entries_(std::make_shared<const std::vector<ScheduleEntry>>(std::move(entries))),
      channel_tz_(std::move(channel_tz)) {}

void Scheduler::setEntries(std::vector<ScheduleEntry> entries) {
    auto next = std::make_shared<const std::vector<ScheduleEntry>>(std::move(entries));
    std::lock_guard<std::mutex> lk(mu_);
    entries_ = std::move(next);
}

void Scheduler::setChannelTz(std::string channel_tz) {
    std::lock_guard<std::mutex> lk(mu_);
    channel_tz_ = std::move(channel_tz);
}

std::vector<ScheduleEntry> Scheduler::entries() const {
    std::shared_ptr<const std::vector<ScheduleEntry>> snap;
    {
        std::lock_guard<std::mutex> lk(mu_);
        snap = entries_;
    }
    return *snap;
}

Scheduler::Decision Scheduler::decide(std::int64_t now_ns) const {
    std::shared_ptr<const std::vector<ScheduleEntry>> snap;
    std::string tz_name;
    {
        std::lock_guard<std::mutex> lk(mu_);
        snap     = entries_;
        tz_name  = channel_tz_;
    }
    const auto* tz = resolveTz(tz_name);

    const ScheduleEntry* best = nullptr;
    std::int64_t         best_window_end = 0;
    for (const auto& e : *snap) {
        const auto m = matchEntry(e, now_ns, tz);
        if (!m.in_window) continue;
        if (best == nullptr ||
            e.priority > best->priority ||
            (e.priority == best->priority && e.id < best->id)) {
            best            = &e;
            best_window_end = m.window_end_ns;
        }
    }

    Decision d;
    if (best != nullptr) {
        d.scheduled     = true;
        d.entry_id      = best->id;
        d.playlist      = best->playlist;
        d.transition    = best->transition;
        d.loop_mode     = best->loop_mode;
        d.hard_switch   = best->hard_switch;
        d.window_end_ns = best_window_end;
    }
    return d;
}

std::vector<Scheduler::Upcoming> Scheduler::upcoming(std::int64_t now_ns,
                                                    std::int64_t within_sec) const {
    std::shared_ptr<const std::vector<ScheduleEntry>> snap;
    std::string tz_name;
    {
        std::lock_guard<std::mutex> lk(mu_);
        snap    = entries_;
        tz_name = channel_tz_;
    }
    const auto* tz = resolveTz(tz_name);
    const std::int64_t horizon_ns = within_sec * kNanosPerSec;

    std::vector<Upcoming> out;
    out.reserve(snap->size());
    for (const auto& e : *snap) {
        if (auto fw = nextActivation(e, now_ns, horizon_ns, tz); fw)
            out.push_back({e.id, fw->start_ns, fw->end_ns});
    }
    std::sort(out.begin(), out.end(),
              [](const Upcoming& a, const Upcoming& b) {
                  if (a.starts_at_ns != b.starts_at_ns)
                      return a.starts_at_ns < b.starts_at_ns;
                  return a.entry_id < b.entry_id;
              });
    return out;
}

nlohmann::json Scheduler::statusJson(std::int64_t now_ns) const {
    const auto d = decide(now_ns);
    nlohmann::json j;
    j["mode"] = d.scheduled ? "schedule" : "regular";
    if (d.scheduled) {
        j["entry_id"]      = d.entry_id;
        j["window_end_ns"] = d.window_end_ns;
    } else {
        j["entry_id"]      = nullptr;
        j["window_end_ns"] = nullptr;
    }
    return j;
}

}  // namespace liveqx::scheduling
