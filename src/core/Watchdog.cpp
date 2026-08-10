#include "core/Watchdog.h"
#include <algorithm>
#include "metrics/ChannelHealth.h"
#include "metrics/ChannelMetrics.h"
#include "utils/Log.h"

using namespace std::chrono;

Watchdog::Watchdog() = default;

Watchdog::~Watchdog() { stop(); }

void Watchdog::start() {
    if (running_.exchange(true)) return;
    thread_ = std::jthread([this](std::stop_token st) { run(st); });
    LOG_INFO("Watchdog started");
}

void Watchdog::stop() {
    if (!running_.exchange(false)) return;
    thread_.request_stop();
    if (thread_.joinable()) thread_.join();
}

void Watchdog::registerChannel(std::shared_ptr<ChannelMetrics> metrics,
                               std::shared_ptr<ChannelHealth>  health,
                               OutputHealthProbe                outputs_probe) {
    std::lock_guard lk(mtx_);
    channels_.push_back({std::move(metrics), std::move(health),
                         std::move(outputs_probe)});
}

void Watchdog::unregisterChannel(const std::shared_ptr<ChannelMetrics>& metrics) {
    std::lock_guard lk(mtx_);
    channels_.erase(
        std::remove_if(channels_.begin(), channels_.end(),
                       [&](const Entry& e) { return e.metrics == metrics; }),
        channels_.end());
}

void Watchdog::evaluateOnce(nanoseconds now) {
    std::vector<Entry> snapshot;
    {
        std::lock_guard lk(mtx_);
        snapshot = channels_;  // shallow copy of shared_ptrs
    }
    const std::int64_t now_ns = now.count();
    for (auto& e : snapshot) {
        const auto s = e.metrics->snapshot();
        // Skip channels that haven't produced a tick yet (just started, or
        // never started). Otherwise heartbeat_age would be ~now_ns and the
        // channel would flip to Failed immediately.
        if (s.last_tick_ns == 0) continue;
        const std::int64_t age = now_ns - s.last_tick_ns;
        OutputHealthSummary outs;
        if (e.outputs_probe) outs = e.outputs_probe();
        e.health->evaluate(s, now_ns, age, outs);
    }
}

void Watchdog::run(std::stop_token st) {
    while (!st.stop_requested() && running_.load(std::memory_order_relaxed)) {
        const auto now = steady_clock::now().time_since_epoch();
        evaluateOnce(duration_cast<nanoseconds>(now));

        // Sleep in 100ms slices so stop() returns promptly.
        for (int i = 0; i < 10 && !st.stop_requested() && running_.load(); ++i)
            std::this_thread::sleep_for(milliseconds(100));
    }
}
