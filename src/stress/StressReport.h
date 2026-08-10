// fix26 — final report from a single stress run.
//
// Emitted by StressRunner::runOnce, persisted by StressReportStore (c10),
// and surfaced as JSON via /api/stress/reports/{id} (c11).

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace liveqx::stress {

struct ChannelStats {
    int           id              = 0;
    std::uint64_t frames_rendered = 0;
    std::uint64_t frames_dropped  = 0;
    double        actual_fps      = 0.0;
    double        expected_fps    = 25.0;  // resolution-derived
    double        fps_drop_pct    = 0.0;   // (expected - actual)/expected * 100
    bool          alive           = true;  // false ⇒ counted as crash
};

struct ScenarioEvent {
    std::int64_t  ts_ms = 0;
    std::string   scenario;     // e.g. "random_output_fail"
    std::string   detail;       // free-form
    bool          ok = true;    // false ⇒ scenario itself errored
};

struct StressReport {
    std::int64_t started_at_ms = 0;
    std::int64_t ended_at_ms   = 0;
    int          duration_sec  = 0;

    int channels_requested    = 0;
    int channels_started      = 0;
    int channels_alive_at_end = 0;
    int crashes               = 0;

    std::vector<ChannelStats>  per_channel;
    std::vector<ScenarioEvent> scenario_events;

    double worst_fps_drop_pct          = 0.0;
    double memory_growth_pct_per_hour  = 0.0;
    std::int64_t rss_kb_start = 0;
    std::int64_t rss_kb_end   = 0;

    bool        pass    = false;
    std::string verdict;  // "pass" | "fail: <reasons>"
    std::vector<std::string> fail_reasons;

    nlohmann::json toJson() const;
};

}  // namespace liveqx::stress
