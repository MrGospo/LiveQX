#pragma once
// fix26 c3 — thread-based sampler for ChannelProfiler in Sampling mode.
//
// One global ProfileSampler runs a single sleeping thread that wakes every
// `period_ms` (default 10ms) and, for every registered profiler whose mode
// is Sampling, reads currentStage() and increments the corresponding hit
// counter via recordSample(). Render threads never write counters in
// sampling mode — they only update one atomic stage enum; the sampler does
// the bookkeeping off-CPU.
//
// Profilers are registered/unregistered via raw pointer keyed on a string
// id. Lifetime is the caller's responsibility — the sampler holds nothing
// but the pointer and never dereferences it after unregister(). Registration
// is guarded by a mutex and only touched outside the sampler hot loop.

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include "metrics/ChannelProfiler.h"

namespace liveqx::profiler {

class ProfileSampler {
public:
    explicit ProfileSampler(std::chrono::milliseconds period =
                            std::chrono::milliseconds(10));
    ~ProfileSampler();

    ProfileSampler(const ProfileSampler&) = delete;
    ProfileSampler& operator=(const ProfileSampler&) = delete;

    void registerChannel(const std::string& id, ChannelProfiler* p);
    void unregisterChannel(const std::string& id);

    // Number of currently registered channels (for tests).
    std::size_t registeredCount() const;

    // Test-only: drives one sampling pass synchronously without waiting for
    // the periodic thread. Used by unit tests to make sample counts
    // deterministic.
    void sampleOnce();

private:
    void run(std::stop_token st);

    std::chrono::milliseconds period_;
    mutable std::mutex mu_;
    std::unordered_map<std::string, ChannelProfiler*> channels_;
    std::jthread thread_;
};

}  // namespace liveqx::profiler
