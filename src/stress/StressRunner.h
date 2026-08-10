// fix26 c6 — golden-path stress runner core.
//
// One StressRunner is constructed per host, holds a reference to the live
// ChannelManager, and knows how to:
//   - synthesize per-channel cfg JSON via a builder (default: synthetic
//     minimal cfg with unique ports). The builder is injectable so
//     stress_runner_cli (c13) and tests can override the cfg shape.
//   - create+play N channels, sleep duration_sec, tear them down,
//     collect ChannelMetrics from each, evaluate pass/fail, return a
//     StressReport.
//
// State machine:    Idle → Running → Idle (after thread join + report stash)
// Only one run at a time. start() refuses to launch a second run while
// the previous is still in Running.
//
// Scenarios are NOT plugged in here — c7-c9 add a Scenario interface and
// the loop that drives them. This commit lays the harness (channel raise
// + measurement + pass/fail) so scenarios slot in cleanly.

#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>

#include <nlohmann/json.hpp>

#include "stress/StressConfig.h"
#include "stress/StressReport.h"

class ChannelManager;

namespace liveqx::stress {

enum class State : std::uint8_t {
    Idle = 0,
    Running = 1,
};

class StressRunner {
public:
    // Builder turns (idx, cfg) into the JSON passed to ChannelManager::create.
    // idx is 1-based. If unset, defaultCfgBuilder() is used (see .cpp).
    using ChannelCfgBuilder =
        std::function<nlohmann::json(int idx, const StressConfig&)>;

    explicit StressRunner(ChannelManager& mgr, ChannelCfgBuilder builder = {});
    ~StressRunner();

    StressRunner(const StressRunner&)            = delete;
    StressRunner& operator=(const StressRunner&) = delete;

    using FinishedCallback = std::function<void(const StressReport&)>;

    // Async kick. Returns false if a run is already in progress or the
    // config fails validation. Validation errors are written to spdlog.
    // The optional `on_finished` callback runs on the worker thread after
    // last_report_ is stashed, just before state_ flips back to Idle —
    // used by StressService (c11) to write the report to disk.
    bool start(const StressConfig& cfg, FinishedCallback on_finished = {});

    // Cooperative stop (joins the worker thread). Safe to call when Idle.
    // The worker observes the stop_token and short-circuits the duration
    // sleep, then proceeds to the normal teardown/report path.
    void stop();

    State state() const noexcept { return state_.load(std::memory_order_acquire); }
    bool  running() const noexcept { return state() == State::Running; }

    // Compact status snapshot ("idle" | "running" + start ts + last report
    // when Idle). Safe to call concurrently with start/stop.
    nlohmann::json statusJson() const;

    // Last completed run's report. Returns null JSON if no run has
    // finished yet.
    nlohmann::json lastReportJson() const;

    // Synchronous one-shot. Used by stress_runner_cli (c13) and unit
    // tests; the async start()/stop() path delegates to this. The caller
    // owns the stop_token; passing a default-constructed token disables
    // cancellation (only the duration_sec timeout terminates the run).
    StressReport runOnce(const StressConfig& cfg, std::stop_token tok = {});

    // Default builder. Exposed so stress_runner_cli (c13) and tests can
    // wrap or replace it.
    static nlohmann::json defaultCfgBuilder(int idx, const StressConfig& cfg);

private:
    void evaluatePass(StressReport& rep, const StressConfig& cfg) const;

    // /proc/self/status VmRSS in KiB, or 0 if unavailable.
    static std::int64_t readRssKb();

    static double expectedFpsFor(const std::string& resolution);

    ChannelManager&   mgr_;
    ChannelCfgBuilder builder_;

    mutable std::mutex                mu_;
    std::atomic<State>                state_{State::Idle};
    std::optional<StressReport>       last_report_;
    std::int64_t                      started_at_ms_ = 0;
    std::optional<std::jthread>       thread_;
};

}  // namespace liveqx::stress
