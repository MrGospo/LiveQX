#pragma once
#include <cstdint>
#include <mutex>
#include <string>

#include "core/Scheduler.h"
#include "transitions/ITransition.h"

namespace liveqx::scheduling {

// Owns the REGULAR ↔ SCHEDULE state machine for a single channel. Lives next
// to ChannelInstance — given a Scheduler, it watches clip boundaries and
// returns an Action describing what needs to happen to the Timeline:
//
//   None          — same source as before, no swap needed.
//   EnterSchedule — was REGULAR, now SCHEDULE — swap Timeline to the entry's
//                   playlist.
//   ExitToRegular — was SCHEDULE, now REGULAR — caller restores the regular
//                   playlist (saved by the operator separately).
//   SwitchEntry   — was SCHEDULE entry A, now SCHEDULE entry B — swap to B's
//                   playlist.
//
// All decisions are made on call to onBoundary(now_ns), which is called from
// the boundary dispatcher thread (one per channel). Single producer; readers
// of currentEntryId / currentWindowEndNs can be on any thread.
class ScheduleController {
public:
    explicit ScheduleController(Scheduler& sched);

    enum class ActionKind { None, EnterSchedule, ExitToRegular, SwitchEntry };

    struct Action {
        ActionKind               kind = ActionKind::None;
        std::string              new_entry_id;
        std::vector<std::string> playlist;
        TransitionConfig         transition;
        LoopMode                 loop_mode    = LoopMode::Loop;
        bool                     hard_switch  = false;
        int64_t                  window_end_ns = 0;
    };

    // Drive the state machine forward by one boundary event. now_ns is the
    // wall-clock time at which the boundary was observed (RenderLoop measures
    // it via system_clock at boundary detection — never cached).
    Action onBoundary(int64_t now_ns);

    // Polled at fixed cadence (~1Hz) by ChannelInstance. Returns a non-None
    // action *only* when the active scheduler decision has hard_switch=true
    // and changes the current entry (entering or switching). Soft windows
    // wait for the natural boundary via onBoundary().
    Action tryHardSwitch(int64_t now_ns);

    bool        inScheduleMode() const noexcept;
    std::string currentEntryId() const;          // empty when in regular mode
    int64_t     currentWindowEndNs() const noexcept;  // 0 when in regular mode

private:
    Scheduler&            sched_;
    mutable std::mutex    mu_;
    bool                  in_schedule_     = false;
    std::string           current_entry_;
    int64_t               window_end_ns_   = 0;
};

}  // namespace liveqx::scheduling
