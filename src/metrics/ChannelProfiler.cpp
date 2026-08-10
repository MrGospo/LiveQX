// fix26 c2 — ChannelProfiler implementation.
//
// All methods are O(1) and lock-free. The hot path (enterStage/leaveStage)
// is dominated by one atomic load when in Off mode.

#include "metrics/ChannelProfiler.h"

#include <algorithm>
#include <string>
#include <string_view>

namespace liveqx::profiler {

namespace {

constexpr std::size_t idx(Stage s) noexcept {
    return static_cast<std::size_t>(s);
}

std::int64_t now_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

}  // namespace

const char* stageName(Stage s) noexcept {
    switch (s) {
        case Stage::None:    return "none";
        case Stage::Decode:  return "decode";
        case Stage::Compose: return "compose";
        case Stage::Encode:  return "encode";
        case Stage::Output:  return "output";
        case Stage::Total:   return "total";
    }
    return "unknown";
}

const char* modeName(Mode m) noexcept {
    switch (m) {
        case Mode::Off:             return "off";
        case Mode::Sampling:        return "sampling";
        case Mode::Instrumentation: return "instrumentation";
    }
    return "unknown";
}

Mode parseMode(std::string_view s) noexcept {
    std::string lower(s);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (lower == "off")             return Mode::Off;
    if (lower == "sampling")        return Mode::Sampling;
    if (lower == "instrumentation") return Mode::Instrumentation;
    return Mode::Off;
}

void ChannelProfiler::setMode(Mode m) noexcept {
    const auto prev = mode_.exchange(m, std::memory_order_release);
    if (prev == m) return;

    const auto now = now_ns();

    // If we were active, freeze the active duration counter.
    if (prev != Mode::Off) {
        const auto started = active_started_ns_.load(std::memory_order_relaxed);
        if (started != 0) {
            const auto delta_ms = (now - started) / 1'000'000;
            active_ms_.fetch_add(delta_ms > 0 ? delta_ms : 0,
                                 std::memory_order_relaxed);
        }
        active_started_ns_.store(0, std::memory_order_relaxed);
    }
    if (m != Mode::Off) {
        active_started_ns_.store(now, std::memory_order_relaxed);
    }

    // Reset transient hot-path state.
    current_stage_.store(Stage::None, std::memory_order_relaxed);
    stage_t0_ns_.store(0, std::memory_order_relaxed);
}

void ChannelProfiler::reset() noexcept {
    for (auto& a : stage_us_)     a.store(0, std::memory_order_relaxed);
    for (auto& a : stage_count_)  a.store(0, std::memory_order_relaxed);
    for (auto& a : sampled_hits_) a.store(0, std::memory_order_relaxed);
    active_ms_.store(0, std::memory_order_relaxed);

    if (mode_.load(std::memory_order_relaxed) != Mode::Off) {
        active_started_ns_.store(now_ns(), std::memory_order_relaxed);
    } else {
        active_started_ns_.store(0, std::memory_order_relaxed);
    }
    current_stage_.store(Stage::None, std::memory_order_relaxed);
    stage_t0_ns_.store(0, std::memory_order_relaxed);
}

void ChannelProfiler::enterStage(Stage s) noexcept {
    const auto m = mode_.load(std::memory_order_relaxed);
    if (m == Mode::Off) return;

    current_stage_.store(s, std::memory_order_relaxed);

    if (m == Mode::Instrumentation) {
        stage_t0_ns_.store(now_ns(), std::memory_order_relaxed);
    }
}

void ChannelProfiler::leaveStage(Stage s) noexcept {
    const auto m = mode_.load(std::memory_order_relaxed);
    if (m == Mode::Off) return;

    if (m == Mode::Instrumentation) {
        const auto t0 = stage_t0_ns_.load(std::memory_order_relaxed);
        if (t0 != 0) {
            const auto t1 = now_ns();
            const auto us = (t1 - t0) / 1000;
            if (us >= 0) {
                stage_us_[idx(s)].fetch_add(static_cast<std::uint64_t>(us),
                                            std::memory_order_relaxed);
                stage_count_[idx(s)].fetch_add(1, std::memory_order_relaxed);
            }
        }
        stage_t0_ns_.store(0, std::memory_order_relaxed);
    }

    current_stage_.store(Stage::None, std::memory_order_relaxed);
}

void ChannelProfiler::recordSample(Stage s) noexcept {
    sampled_hits_[idx(s)].fetch_add(1, std::memory_order_relaxed);
}

ChannelProfile ChannelProfiler::snapshot() const {
    ChannelProfile p;
    p.mode = mode_.load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < kStageCount; ++i) {
        p.stage_us[i]     = stage_us_[i].load(std::memory_order_relaxed);
        p.stage_count[i]  = stage_count_[i].load(std::memory_order_relaxed);
        p.sampled_hits[i] = sampled_hits_[i].load(std::memory_order_relaxed);
    }
    auto active = active_ms_.load(std::memory_order_relaxed);
    const auto started = active_started_ns_.load(std::memory_order_relaxed);
    if (started != 0) {
        active += (now_ns() - started) / 1'000'000;
    }
    p.active_ms = active;
    return p;
}

}  // namespace liveqx::profiler
