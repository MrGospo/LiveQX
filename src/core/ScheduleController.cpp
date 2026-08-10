#include "core/ScheduleController.h"

namespace liveqx::scheduling {

ScheduleController::ScheduleController(Scheduler& sched) : sched_(sched) {}

ScheduleController::Action ScheduleController::onBoundary(int64_t now_ns) {
    const auto d = sched_.decide(now_ns);
    Action a;

    std::lock_guard<std::mutex> lk(mu_);
    if (d.scheduled) {
        if (!in_schedule_) {
            a.kind            = ActionKind::EnterSchedule;
            a.new_entry_id    = d.entry_id;
            a.playlist        = d.playlist;
            a.transition      = d.transition;
            a.loop_mode       = d.loop_mode;
            a.hard_switch     = d.hard_switch;
            a.window_end_ns   = d.window_end_ns;
            in_schedule_      = true;
            current_entry_    = d.entry_id;
            window_end_ns_    = d.window_end_ns;
        } else if (d.entry_id != current_entry_) {
            a.kind            = ActionKind::SwitchEntry;
            a.new_entry_id    = d.entry_id;
            a.playlist        = d.playlist;
            a.transition      = d.transition;
            a.loop_mode       = d.loop_mode;
            a.hard_switch     = d.hard_switch;
            a.window_end_ns   = d.window_end_ns;
            current_entry_    = d.entry_id;
            window_end_ns_    = d.window_end_ns;
        } else {
            // Same window — keep playing. Refresh window_end_ns in case the
            // entry was edited via hot-reload (start_time stayed but end_time
            // shifted, etc).
            window_end_ns_    = d.window_end_ns;
            a.kind            = ActionKind::None;
        }
    } else {
        if (in_schedule_) {
            a.kind            = ActionKind::ExitToRegular;
            in_schedule_      = false;
            current_entry_.clear();
            window_end_ns_    = 0;
        }
    }
    return a;
}

ScheduleController::Action ScheduleController::tryHardSwitch(int64_t now_ns) {
    const auto d = sched_.decide(now_ns);
    Action a;

    std::lock_guard<std::mutex> lk(mu_);
    if (!d.scheduled || !d.hard_switch) return a;

    // Same entry as already playing — soft path (boundary refresh) handles it.
    if (in_schedule_ && d.entry_id == current_entry_) {
        window_end_ns_ = d.window_end_ns;
        return a;
    }

    a.kind            = in_schedule_ ? ActionKind::SwitchEntry : ActionKind::EnterSchedule;
    a.new_entry_id    = d.entry_id;
    a.playlist        = d.playlist;
    a.transition      = d.transition;
    a.loop_mode       = d.loop_mode;
    a.hard_switch     = true;
    a.window_end_ns   = d.window_end_ns;
    in_schedule_      = true;
    current_entry_    = d.entry_id;
    window_end_ns_    = d.window_end_ns;
    return a;
}

bool ScheduleController::inScheduleMode() const noexcept {
    std::lock_guard<std::mutex> lk(mu_);
    return in_schedule_;
}

std::string ScheduleController::currentEntryId() const {
    std::lock_guard<std::mutex> lk(mu_);
    return current_entry_;
}

int64_t ScheduleController::currentWindowEndNs() const noexcept {
    std::lock_guard<std::mutex> lk(mu_);
    return window_end_ns_;
}

}  // namespace liveqx::scheduling
