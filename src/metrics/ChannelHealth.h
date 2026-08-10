#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include "metrics/ChannelMetrics.h"

namespace liveqx::events { class EventBus; }

enum class HealthState : std::uint8_t {
    Running  = 0,
    Degraded = 1,
    Failed   = 2,
};

const char* healthStateName(HealthState s) noexcept;

// Snapshot of per-channel output fan-out health (fix12 c7). Watchdog
// fills this from OutputManager every tick. healthy/total = 0/0 means
// "no information" (channel without outputs or probe absent) and is
// neutral — does not contribute to degrade/fail signals.
struct OutputHealthSummary {
    int healthy = 0;
    int total   = 0;
};

// Per-channel health state machine. Transitions are driven by Watchdog
// invoking evaluate() at 1Hz. Public state load is atomic and safe from any
// thread (REST API, etc.). evaluate() must be called from a single thread.
class ChannelHealth {
public:
    ChannelHealth(std::string channel_id, int target_fps);

    HealthState        state()       const noexcept { return state_.load(std::memory_order_acquire); }
    std::int64_t       enteredAtNs() const noexcept { return entered_at_ns_.load(std::memory_order_relaxed); }
    const std::string& channelId()   const noexcept { return channel_id_; }
    int                targetFps()   const noexcept { return target_fps_; }

    // Watchdog-thread-only entry point.
    //  s                — recent metrics snapshot
    //  now_ns           — monotonic now, single read shared by all channels
    //  heartbeat_age_ns — now_ns - metrics.last_tick_ns
    //  outputs          — per-channel output fan-out summary; default
    //                     (healthy=0, total=0) keeps existing call sites
    //                     unchanged and contributes no degrade/fail signal.
    void evaluate(const ChannelMetricsSnapshot& s,
                  std::int64_t now_ns,
                  std::int64_t heartbeat_age_ns,
                  OutputHealthSummary outputs = {}) noexcept;

    // Reset to Running and clear bookkeeping. Called by ChannelInstance on
    // play() after a stop/play cycle, so a stale Failed state from the
    // previous run does not bleed into the new one.
    void reset() noexcept;

    // fix23: inject the process-wide EventBus + the integer channel id
    // used by SSE role filtering (channel_grants are keyed by int id, not
    // the "ch{id}-{name}" debug string in channel_id_). nullptr disables.
    void setEventBus(liveqx::events::EventBus* bus,
                     int channel_id_int) noexcept {
        event_bus_         = bus;
        channel_id_int_    = channel_id_int;
    }

private:
    void transitionTo(HealthState target, std::int64_t now_ns, const char* reason) noexcept;

    std::string                 channel_id_;
    int                         target_fps_;
    std::atomic<HealthState>    state_{HealthState::Running};
    std::atomic<std::int64_t>   entered_at_ns_{0};

    // Watchdog-thread local bookkeeping.
    std::int64_t last_clean_ns_      = 0;   // start of current clean streak
    std::int64_t first_eval_ns_      = -1;  // -1 = not yet seeded (now_ns can legitimately be 0)

    liveqx::events::EventBus* event_bus_       = nullptr;  // fix23
    int                                channel_id_int_ = -1;       // fix23 — for SSE role filtering
};
