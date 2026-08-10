// fix26 c11 — front-end aggregator that owns the runner, scheduler, and
// report store, and exposes the methods the REST layer (ControlApi) calls.
//
// One StressService per host. Constructed by main.cpp (c13) with the
// initial StressConfig loaded from state.db; subsequent PUT
// /api/stress/config calls flow through setConfig which:
//   - validates the JSON
//   - swaps the in-memory config under a mutex
//   - re-arms the scheduler if schedule_cron changed
//   - invokes a caller-registered persistence callback (so the owner can
//     write to state.db without StressService knowing about SQLite)
//
// The scheduler ticks the runner via runner_.start(...) on each cron
// match. A FinishedCallback writes the StressReport to the on-disk
// report store.

#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "stress/StressConfig.h"
#include "stress/StressRunner.h"
#include "stress/StressScheduler.h"
#include "stress/StressReportStore.h"

class ChannelManager;
namespace liveqx::events { class EventBus; }

namespace liveqx::stress {

class StressService {
public:
    using PersistCallback = std::function<void(const StressConfig&)>;

    StressService(ChannelManager& mgr,
                  StressConfig initial_cfg,
                  StressRunner::ChannelCfgBuilder builder = {},
                  liveqx::events::EventBus* events = nullptr);
    ~StressService();

    StressService(const StressService&)            = delete;
    StressService& operator=(const StressService&) = delete;

    // ── Config ──────────────────────────────────────────────────────────
    StressConfig config() const;
    nlohmann::json configJson() const;

    // Returns true on success. On failure, *err carries a human-readable
    // reason and the in-memory config is left untouched.
    bool setConfigJson(const nlohmann::json& body, std::string* err);

    // Owner registers this once (typically main.cpp persists to state.db).
    void setPersistCallback(PersistCallback cb);

    // ── Runner control ──────────────────────────────────────────────────
    // Returns true on successful kick; false if already running, config
    // is invalid, or master `enabled` is false.
    bool startNow(std::string* err = nullptr);

    void stopNow();

    nlohmann::json statusJson() const;

    // ── Reports ─────────────────────────────────────────────────────────
    // Returns null JSON if no report dir is configured.
    nlohmann::json listReportsJson() const;

    // Returns nullopt for unknown id or unreadable file.
    std::optional<nlohmann::json> readReportJson(const std::string& id) const;

    // Returns true if deleted, false if not found.
    bool removeReport(const std::string& id);

    // ── Snapshot for /api/metrics ───────────────────────────────────────
    struct MetricsSnapshot {
        bool         running           = false;
        std::int64_t runs_total        = 0;
        std::int64_t passes_total      = 0;
        std::int64_t failures_total    = 0;
        std::int64_t last_started_ms   = 0;
        std::int64_t last_ended_ms     = 0;
        bool         last_pass         = false;
        std::size_t  reports_count     = 0;
        bool         scheduler_armed   = false;
    };
    MetricsSnapshot metricsSnapshot() const;

private:
    void rearmSchedulerLocked();
    void onRunStarted(const StressConfig& cfg);
    void onRunFinished(const StressReport& rep);

    ChannelManager&                     mgr_;
    StressRunner::ChannelCfgBuilder     builder_;
    liveqx::events::EventBus*   events_;

    mutable std::mutex                  mu_;
    StressConfig                        cfg_;
    PersistCallback                     persist_;

    std::unique_ptr<StressRunner>       runner_;
    std::unique_ptr<StressScheduler>    scheduler_;
    std::unique_ptr<StressReportStore>  store_;

    std::string                         last_report_id_;

    // Aggregate counters for /api/metrics — incremented on every finish.
    std::int64_t                        runs_total_      = 0;
    std::int64_t                        passes_total_    = 0;
    std::int64_t                        failures_total_  = 0;
    std::int64_t                        last_started_ms_ = 0;
    std::int64_t                        last_ended_ms_   = 0;
    bool                                last_pass_       = false;
};

}  // namespace liveqx::stress
