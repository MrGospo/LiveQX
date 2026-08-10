// fix26 c3 — ProfileSampler implementation.
//
// One std::jthread sleeps in chunks (the period itself, capped at 100ms so
// shutdown is responsive) and on each wake holds the registration mutex
// briefly to read the channel map. The hot operation against each profiler
// is one atomic load (currentStage) + one atomic_fetch_add (recordSample).
// Profilers in Off or Instrumentation mode are skipped.

#include "metrics/ProfileSampler.h"

#include <chrono>

namespace liveqx::profiler {

ProfileSampler::ProfileSampler(std::chrono::milliseconds period)
    : period_(period) {
    thread_ = std::jthread([this](std::stop_token st) { run(st); });
}

ProfileSampler::~ProfileSampler() {
    if (thread_.joinable()) {
        thread_.request_stop();
        thread_.join();
    }
}

void ProfileSampler::registerChannel(const std::string& id, ChannelProfiler* p) {
    if (!p) return;
    std::lock_guard lk(mu_);
    channels_[id] = p;
}

void ProfileSampler::unregisterChannel(const std::string& id) {
    std::lock_guard lk(mu_);
    channels_.erase(id);
}

std::size_t ProfileSampler::registeredCount() const {
    std::lock_guard lk(mu_);
    return channels_.size();
}

void ProfileSampler::sampleOnce() {
    std::lock_guard lk(mu_);
    for (auto& [_, p] : channels_) {
        if (!p) continue;
        if (p->mode() != Mode::Sampling) continue;
        const auto stage = p->currentStage();
        p->recordSample(stage);
    }
}

void ProfileSampler::run(std::stop_token st) {
    using namespace std::chrono;
    // Cap the per-iteration sleep so a long period_ doesn't delay shutdown.
    const auto chunk = std::min(period_, milliseconds(100));
    auto last_tick = steady_clock::now();
    while (!st.stop_requested()) {
        std::this_thread::sleep_for(chunk);
        if (st.stop_requested()) break;
        const auto now = steady_clock::now();
        if (now - last_tick < period_) continue;
        last_tick = now;
        sampleOnce();
    }
}

}  // namespace liveqx::profiler
