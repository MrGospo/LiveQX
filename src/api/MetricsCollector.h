#pragma once
#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

class ChannelManager;
class ProcessMetrics;

namespace liveqx::api     { struct ChannelScope; }
namespace liveqx::gateway { class GatewayManager; }
namespace liveqx::stress  { class StressService; }
namespace liveqx::plugins { class PluginManager; }
namespace liveqx::mounts  { class MountManager; }

// Build-time identity injected from CMake via LIVEQX_VERSION /
// LIVEQX_COMMIT / LIVEQX_BUILD_TIME. Stored as plain strings
// so callers can override in tests.
struct BuildInfo {
    std::string version;       // e.g. "0.16.0"
    std::string commit;        // short git sha or "unknown"
    std::string build_time;    // ISO-8601 UTC, e.g. "2026-05-06T10:00:00Z"
    std::string build_type;    // CMake build type — "Release"/"Debug"/...
};

// Walks the live ChannelManager and ProcessMetrics readers and renders the
// Prometheus 0.0.4 text exposition for /api/metrics. Stateless — collects
// fresh on every call. The expected scrape cadence is 15s; at 100 channels ×
// ~30 metrics this stays under a millisecond.
class MetricsCollector {
public:
    // gateways may be nullptr — gateway_* Prometheus families and the
    // top-level "gateways" array in /api/status are then simply omitted.
    MetricsCollector(const ChannelManager& mgr,
                     ProcessMetrics&       process,
                     BuildInfo             build,
                     const liveqx::gateway::GatewayManager* gateways = nullptr,
                     const liveqx::stress::StressService*   stress   = nullptr,
                     const liveqx::plugins::PluginManager*  plugins  = nullptr,
                     liveqx::mounts::MountManager*          mounts   = nullptr);

    // text/plain; version=0.0.4 exposition format.
    //
    // `scope==nullptr` → full view (anonymous Prometheus scrape, tests).
    // `scope!=nullptr` → per-channel families and gateway families are
    // filtered to what the caller is allowed to see; cross-channel
    // summary gauges are recomputed from the visible subset.
    std::string renderPrometheus(
        const liveqx::api::ChannelScope* scope = nullptr) const;

    // /api/version response body. Static build identity + runtime feature
    // flags. fix19/21/22/28/29 each flip their own flag as they land.
    nlohmann::json renderVersion() const;

    // /api/status JSON aggregate: build/process/per-channel/summary in one
    // hop, designed so a UI dashboard can render its full first frame on
    // a single GET. Richer than Prometheus (includes clip names, output
    // states, content_sync ages) but no historical series.
    //
    // `scope` semantics match `renderPrometheus`.
    nlohmann::json renderStatus(
        const liveqx::api::ChannelScope* scope = nullptr) const;

private:
    const ChannelManager&                          mgr_;
    ProcessMetrics&                                process_;
    BuildInfo                                      build_;
    const liveqx::gateway::GatewayManager* gateways_;
    const liveqx::stress::StressService*   stress_;
    const liveqx::plugins::PluginManager*  plugins_;
    liveqx::mounts::MountManager*          mounts_;
};
