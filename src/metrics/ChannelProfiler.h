#pragma once
// fix26 c2 — per-channel profiler with three modes (Off/Sampling/Instrumentation).
//
// Off (default): zero overhead. enterStage/leaveStage do one relaxed atomic
// load and return. No writes anywhere on the render hot path.
//
// Sampling: enterStage stores the current stage in an atomic enum; leaveStage
// resets it to None. A separate sampler thread (added in commit 3) reads the
// atomic at ~10ms intervals and increments per-stage hit counters off-CPU.
// One atomic store per stage transition; render thread never writes counters.
//
// Instrumentation: enterStage records steady_clock::now(); leaveStage adds the
// delta to a per-stage atomic accumulator. More expensive (clock_gettime +
// atomic_add per stage) — opt-in via REST for a specific channel under
// investigation.
//
// Thread model: each channel's render loop runs on a single thread, so the t0
// timestamps are non-shared between threads. Mode switches are eventually
// consistent — the render thread sees the new mode within at most one stage
// transition.

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>

namespace liveqx::profiler {

// Five stages of the per-frame render pipeline. Order MUST match the
// public-facing JSON keys in ChannelProfile snapshot.
enum class Stage : std::uint8_t {
    None    = 0,  // not in any stage (idle / between frames)
    Decode  = 1,
    Compose = 2,
    Encode  = 3,
    Output  = 4,
    Total   = 5,  // wraps the whole frame
};

constexpr std::size_t kStageCount = 6;  // None..Total inclusive

const char* stageName(Stage s) noexcept;

enum class Mode : std::uint8_t {
    Off             = 0,
    Sampling        = 1,
    Instrumentation = 2,
};

const char* modeName(Mode m) noexcept;

// Returns Mode::Off for unknown / empty input.
Mode parseMode(std::string_view s) noexcept;

// Snapshot returned to REST callers. Either sampled_counts OR per-stage us
// fields are populated, depending on which mode was active. If both modes
// were used since last reset, both fields contain the data accumulated under
// each respective mode.
struct ChannelProfile {
    Mode mode = Mode::Off;

    // Instrumentation: accumulated us per stage.
    std::array<std::uint64_t, kStageCount> stage_us{};
    // Number of leaveStage calls per stage (instrumentation only).
    std::array<std::uint64_t, kStageCount> stage_count{};

    // Sampling: number of samples that observed each stage.
    std::array<std::uint64_t, kStageCount> sampled_hits{};

    // Wall time the profile was active (ms).
    std::int64_t active_ms = 0;
};

class ChannelProfiler {
public:
    ChannelProfiler() noexcept = default;

    // Mode control. Safe from any thread. Switching reset stage timestamp;
    // accumulated counters are preserved unless reset() is called.
    void setMode(Mode m) noexcept;
    void reset() noexcept;
    Mode mode() const noexcept { return mode_.load(std::memory_order_relaxed); }

    // Hot path. enterStage stores the stage atomically (sampling) and
    // captures t0 (instrumentation). leaveStage adds the delta and clears
    // the stage. Both are no-ops in Off mode beyond a single relaxed load.
    void enterStage(Stage s) noexcept;
    void leaveStage(Stage s) noexcept;

    // Used by ProfileSampler thread (commit 3). Reads the current stage
    // without disturbing the render thread.
    Stage currentStage() const noexcept {
        return current_stage_.load(std::memory_order_relaxed);
    }

    // Used by ProfileSampler to bump sampled hits.
    void recordSample(Stage s) noexcept;

    ChannelProfile snapshot() const;

private:
    using clock = std::chrono::steady_clock;

    std::atomic<Mode>  mode_{Mode::Off};
    std::atomic<Stage> current_stage_{Stage::None};

    // Instrumentation t0 — only the channel's render thread writes/reads,
    // so plain (non-atomic) is fine. We use atomic only because
    // setMode/reset can race with the hot path on the same field.
    std::atomic<std::int64_t> stage_t0_ns_{0};

    std::array<std::atomic<std::uint64_t>, kStageCount> stage_us_{};
    std::array<std::atomic<std::uint64_t>, kStageCount> stage_count_{};
    std::array<std::atomic<std::uint64_t>, kStageCount> sampled_hits_{};

    std::atomic<std::int64_t> active_started_ns_{0};
    std::atomic<std::int64_t> active_ms_{0};
};

}  // namespace liveqx::profiler
