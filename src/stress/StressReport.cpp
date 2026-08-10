#include "stress/StressReport.h"

namespace liveqx::stress {

nlohmann::json StressReport::toJson() const {
    nlohmann::json per = nlohmann::json::array();
    for (const auto& c : per_channel) {
        per.push_back({
            {"id",              c.id},
            {"frames_rendered", c.frames_rendered},
            {"frames_dropped",  c.frames_dropped},
            {"actual_fps",      c.actual_fps},
            {"expected_fps",    c.expected_fps},
            {"fps_drop_pct",    c.fps_drop_pct},
            {"alive",           c.alive},
        });
    }
    nlohmann::json events = nlohmann::json::array();
    for (const auto& e : scenario_events) {
        events.push_back({
            {"ts_ms",    e.ts_ms},
            {"scenario", e.scenario},
            {"detail",   e.detail},
            {"ok",       e.ok},
        });
    }
    return nlohmann::json{
        {"started_at_ms",         started_at_ms},
        {"ended_at_ms",           ended_at_ms},
        {"duration_sec",          duration_sec},
        {"channels_requested",    channels_requested},
        {"channels_started",      channels_started},
        {"channels_alive_at_end", channels_alive_at_end},
        {"crashes",               crashes},
        {"per_channel",           per},
        {"scenario_events",       events},
        {"worst_fps_drop_pct",    worst_fps_drop_pct},
        {"memory_growth_pct_per_hour", memory_growth_pct_per_hour},
        {"rss_kb_start",          rss_kb_start},
        {"rss_kb_end",            rss_kb_end},
        {"pass",                  pass},
        {"verdict",               verdict},
        {"fail_reasons",          fail_reasons},
    };
}

}  // namespace liveqx::stress
