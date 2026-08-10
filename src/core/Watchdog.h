#pragma once
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>
#include <vector>

#include "metrics/ChannelHealth.h"

struct ChannelMetrics;

// Single-process watchdog. One thread polls all registered channels at 1Hz,
// computes heartbeat age and metric snapshots, and drives each channel's
// ChannelHealth state machine. The watchdog itself does not log or react —
// ChannelHealth owns the state transitions and emits LOG_*. Putting this on
// its own thread is essential: if a render thread is stuck inside a syscall,
// nothing on that thread (including a timer) will run.
class Watchdog {
public:
    Watchdog();
    ~Watchdog();

    void start();
    void stop();

    // Probe that returns the channel's per-output health summary. Called
    // from the watchdog thread on every tick — the implementation must be
    // thread-safe (OutputManager already is). Pass {} to register a channel
    // without an outputs probe; ChannelHealth will then ignore the signal.
    using OutputHealthProbe = std::function<OutputHealthSummary()>;

    void registerChannel(std::shared_ptr<ChannelMetrics> metrics,
                         std::shared_ptr<ChannelHealth>  health,
                         OutputHealthProbe                outputs_probe = {});

    // Removes the entry whose metrics ptr matches. No-op if absent.
    void unregisterChannel(const std::shared_ptr<ChannelMetrics>& metrics);

    // Runs one evaluation pass. Exposed for tests; production calls happen
    // from the watchdog thread at 1Hz.
    void evaluateOnce(std::chrono::nanoseconds now);

private:
    void run(std::stop_token st);

    struct Entry {
        std::shared_ptr<ChannelMetrics> metrics;
        std::shared_ptr<ChannelHealth>  health;
        OutputHealthProbe               outputs_probe;
    };

    std::mutex          mtx_;
    std::vector<Entry>  channels_;
    std::atomic<bool>   running_{false};
    std::jthread        thread_;
};
