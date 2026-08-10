#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/ScheduleEntry.h"
#include "transitions/ITransition.h"

namespace liveqx::scheduling {

// Pure-function decision about what should be playing at a given wall-clock
// instant. `decide()` is referentially transparent — given the same entries
// and now_ns, it returns the same Decision.
//
// Hot-reload model: setEntries() atomically swaps the underlying list under
// a writer lock. decide() takes a shared_ptr snapshot under the same lock,
// then operates on the snapshot lock-free. Producer side (Timeline) calls
// decide() at clip boundaries — typical rate is one call every few seconds,
// so the brief lock is cheap.
//
// Time zone: HH:MM окна интерпретируются в channel_tz (IANA имя) через
// date::zoned_time (Howard Hinnant tzdb — переносимо на libstdc++ 12+).
// Невалидная зона → UTC fallback. setChannelTz() атомарно подменяет зону
// под mu_ — следующий decide()/upcoming() уже читает новое значение
// (для inherit-каналов при смене серверной TZ).
class Scheduler {
public:
    explicit Scheduler(std::vector<ScheduleEntry> entries,
                       std::string channel_tz = "UTC");

    // Atomically replace the entry list. Existing decide() calls in flight
    // see the previous snapshot until they return.
    void setEntries(std::vector<ScheduleEntry> entries);

    // Hot-swap channel TZ. Используется при смене серверной TZ для каналов,
    // которые наследуют её (ChannelInstance::applyServerTimezoneChange).
    void setChannelTz(std::string channel_tz);

    // Snapshot of the current entries, for GET /schedule.
    std::vector<ScheduleEntry> entries() const;

    struct Decision {
        bool        scheduled    = false;     // false = no active window — caller falls back
        std::string entry_id;                  // valid only when scheduled
        std::vector<std::string> playlist;    // clip paths in order
        TransitionConfig transition;
        LoopMode    loop_mode    = LoopMode::Loop;
        bool        hard_switch  = false;
        int64_t     window_end_ns = 0;        // when the active window ends (exclusive)
    };

    // Pure given the current entry snapshot. Threadsafe — internal lock
    // protects the snapshot pointer load only.
    Decision decide(int64_t now_ns) const;

    // Returns the earliest activation time strictly after `now_ns` for each
    // entry, sorted ascending. Used by GET /schedule/upcoming and tests.
    // Capped at `within_sec`; entries that wouldn't fire in that window are
    // omitted.
    struct Upcoming {
        std::string entry_id;
        int64_t     starts_at_ns = 0;
        int64_t     ends_at_ns   = 0;
    };
    std::vector<Upcoming> upcoming(int64_t now_ns, int64_t within_sec) const;

    // Diagnostics for GET /schedule/active.
    nlohmann::json statusJson(int64_t now_ns) const;

private:
    // Snapshot held by atomic shared_ptr — readers grab a copy, writers swap.
    std::shared_ptr<const std::vector<ScheduleEntry>> entries_;
    mutable std::mutex                                mu_;
    std::string                                       channel_tz_;
};

}  // namespace liveqx::scheduling
