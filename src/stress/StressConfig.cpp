#include "stress/StressConfig.h"

namespace liveqx::stress {

nlohmann::json StressConfig::toJson() const {
    return nlohmann::json{
        {"enabled",             enabled},
        {"schedule_cron",       schedule_cron},
        {"duration_sec",        duration_sec},
        {"channels",            channels},
        {"channel_resolution",  channel_resolution},
        {"outputs_per_channel", outputs_per_channel},
        {"scenarios",           scenarios},
        {"scenario_options",    scenario_options},
        {"pass_criteria", {
            {"max_fps_drop_pct",                pass.max_fps_drop_pct},
            {"max_memory_growth_pct_per_hour",  pass.max_memory_growth_pct_per_hour},
            {"max_crashes",                     pass.max_crashes},
        }},
        {"report_dir",          report_dir},
    };
}

StressConfig StressConfig::fromJson(const nlohmann::json& j) {
    StressConfig c;
    if (j.is_null() || !j.is_object()) return c;
    auto get_or = [&](const char* k, auto def) {
        using T = decltype(def);
        if (j.contains(k) && !j[k].is_null()) {
            try { return j[k].get<T>(); } catch (...) { return def; }
        }
        return def;
    };
    c.enabled            = get_or("enabled",             c.enabled);
    c.schedule_cron      = get_or("schedule_cron",       c.schedule_cron);
    c.duration_sec       = get_or("duration_sec",        c.duration_sec);
    c.channels           = get_or("channels",            c.channels);
    c.channel_resolution = get_or("channel_resolution",  c.channel_resolution);
    c.outputs_per_channel= get_or("outputs_per_channel", c.outputs_per_channel);
    c.report_dir         = get_or("report_dir",          c.report_dir);
    if (j.contains("scenarios") && j["scenarios"].is_array()) {
        c.scenarios.clear();
        for (const auto& it : j["scenarios"]) {
            if (it.is_string()) c.scenarios.push_back(it.get<std::string>());
        }
    }
    if (j.contains("scenario_options") && j["scenario_options"].is_object()) {
        c.scenario_options = j["scenario_options"];
    }
    if (j.contains("pass_criteria") && j["pass_criteria"].is_object()) {
        const auto& p = j["pass_criteria"];
        if (p.contains("max_fps_drop_pct") && p["max_fps_drop_pct"].is_number())
            c.pass.max_fps_drop_pct = p["max_fps_drop_pct"].get<double>();
        if (p.contains("max_memory_growth_pct_per_hour") &&
            p["max_memory_growth_pct_per_hour"].is_number())
            c.pass.max_memory_growth_pct_per_hour =
                p["max_memory_growth_pct_per_hour"].get<double>();
        if (p.contains("max_crashes") && p["max_crashes"].is_number_integer())
            c.pass.max_crashes = p["max_crashes"].get<int>();
    }
    return c;
}

std::optional<std::string> StressConfig::validate() const {
    if (duration_sec <= 0) return "duration_sec must be > 0";
    if (channels < 0)      return "channels must be >= 0";
    if (outputs_per_channel < 0)
        return "outputs_per_channel must be >= 0";
    if (pass.max_fps_drop_pct < 0)
        return "pass_criteria.max_fps_drop_pct must be >= 0";
    if (pass.max_memory_growth_pct_per_hour < 0)
        return "pass_criteria.max_memory_growth_pct_per_hour must be >= 0";
    if (pass.max_crashes < 0)
        return "pass_criteria.max_crashes must be >= 0";
    return std::nullopt;
}

}  // namespace liveqx::stress
