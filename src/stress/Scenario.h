// fix26 c7 — pluggable scenario interface for failure injection during a
// stress run. StressRunner owns the scenarios for the duration of a run
// and ticks them once per second (configurable). Each scenario maintains
// its own internal state (timers, RNG, pending restores, etc.).

#pragma once

#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "stress/StressReport.h"

class ChannelManager;

namespace liveqx::stress {

struct ScenarioContext {
    ChannelManager*  mgr            = nullptr;
    std::vector<int> channel_ids;
    std::int64_t     run_started_ms = 0;
    std::mt19937_64  rng;          // seeded by StressRunner per-run
};

class IScenario {
public:
    virtual ~IScenario() = default;

    // Stable identifier — also the name used in StressConfig.scenarios.
    virtual std::string name() const = 0;

    // Called once before the first tick. Default is no-op.
    virtual void onStart(ScenarioContext& /*ctx*/) {}

    // Called periodically by the runner (default cadence: 1 Hz). The
    // scenario decides whether to act based on internal timers/RNG.
    // now_ms = wall-clock; elapsed_ms = ms since run start. Any events
    // (kills/restores/errors) are appended to `out`.
    virtual void tick(ScenarioContext& ctx,
                      std::int64_t  now_ms,
                      std::int64_t  elapsed_ms,
                      std::vector<ScenarioEvent>& out) = 0;

    // Called once after the duration ends OR cancellation. Scenarios
    // should restore any in-flight failures here (e.g. re-add outputs
    // that are still suspended) so the StressRunner teardown sees a
    // clean state.
    virtual void onFinish(ScenarioContext& /*ctx*/,
                          std::vector<ScenarioEvent>& /*out*/) {}
};

// Factory: returns nullptr for unknown names so the caller can warn
// instead of failing the run (forward-compat with newer cfgs).
//
// Some scenarios (e.g. clip_corrupt) need extra wiring data — paths,
// interface names, etc. — that doesn't fit into a name-only registry.
// Callers pass per-scenario options as JSON; if a required option is
// missing the factory returns nullptr with a warning.
std::unique_ptr<IScenario> makeScenario(const std::string& name,
                                         const nlohmann::json& options = {});

}  // namespace liveqx::stress
