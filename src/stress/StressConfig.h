// fix26 — config for the periodic stress run.
//
// Lives in state.db (per-host config, NOT per-channel) and is editable via
// PUT /api/stress/config. The {enabled} master switch decides whether the
// runner+scheduler are constructed at all.

#pragma once

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace liveqx::stress {

struct PassCriteria {
    // Per-channel "(expected_fps - actual_fps) / expected_fps * 100" must
    // stay BELOW this. >=1.0% drop on any channel ⇒ fail.
    double max_fps_drop_pct = 1.0;

    // RSS growth measured between run start and end, normalized to per-hour.
    // Above this ⇒ fail (probable leak).
    double max_memory_growth_pct_per_hour = 0.1;

    // Channels that died during the run (started ⇒ not alive at end and not
    // stopped by us). Above this ⇒ fail. Default 0 = zero crashes tolerated.
    int max_crashes = 0;
};

struct StressConfig {
    bool enabled = false;

    // Internal cron parser (fix26 c10) — minimal "m h dom mon dow".
    std::string schedule_cron = "0 2 * * *";   // daily 02:00

    int duration_sec = 86400;                  // 24h
    int channels     = 50;
    std::string channel_resolution = "720p25";
    int outputs_per_channel = 5;

    // Names of scenarios to run during the stress (c7-c9). Unknown names are
    // ignored with a warning so a future runner is forward-compatible with
    // configs from older binaries.
    std::vector<std::string> scenarios;

    // Per-scenario opaque options keyed by scenario name. Used by
    // scenarios that need wiring data the factory can't synthesize
    // (e.g. clip_corrupt needs a list of candidate file paths). A
    // scenario with no options key defaults to its zero-arg constructor.
    nlohmann::json scenario_options = nlohmann::json::object();

    PassCriteria pass{};

    std::string report_dir = "state/stress/reports";

    nlohmann::json toJson() const;
    static StressConfig fromJson(const nlohmann::json& j);

    // nullopt = valid. Otherwise a short reason ("duration_sec must be >0",
    // "channels must be >=0", etc.).
    std::optional<std::string> validate() const;
};

}  // namespace liveqx::stress
