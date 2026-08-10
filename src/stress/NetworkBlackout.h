// fix26 c9 — network_blackout scenario.
//
// Periodically applies a 100%-loss qdisc to one of the configured network
// interfaces via `tc qdisc add dev <iface> root netem loss 100%` to simulate
// an upstream/downstream black-hole, then removes the rule after a randomized
// blackout_window so the runner can verify reconnection logic.
//
// Privileged operations only: requires CAP_NET_ADMIN (effectively root). When
// not running as root the scenario stays inert — every kill attempt emits an
// ok=false event explaining the skip and re-schedules. This keeps the rest of
// the stress run going on hosts where the runner lacks privileges (CI, dev
// laptops) without false-failing the run.
//
// Option wiring (ScenarioFactory.cpp):
//   cfg.scenario_options["network_blackout"] = {
//       "interfaces": ["eth0", "eth1"]   // required, non-empty
//   }
//
// State per scenario instance:
//   - next_fail_at_ms_  → when to trigger the next blackout
//   - pending_          → the active blackout still to be cleared
//
// Intervals are constructor-injectable so tests can drive the state machine
// deterministically without waiting minutes.

#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "stress/Scenario.h"

namespace liveqx::stress {

struct NetworkBlackoutIntervals {
    std::int64_t fail_min_ms     = 10 * 60 * 1000;   // every 10..30 minutes
    std::int64_t fail_max_ms     = 30 * 60 * 1000;
    std::int64_t blackout_min_ms =      30 * 1000;   // 30..120s outage
    std::int64_t blackout_max_ms =     120 * 1000;
};

class NetworkBlackout final : public IScenario {
public:
    // Hooks for tests: TcRunner returns the shell exit code (0 on success);
    // PrivCheck returns true if the runner thinks it can do tc operations.
    // Defaults call into the real system (geteuid + std::system("tc ...")).
    using TcRunner  = std::function<int(const std::string& cmd)>;
    using PrivCheck = std::function<bool()>;

    explicit NetworkBlackout(std::vector<std::string> interfaces,
                             NetworkBlackoutIntervals iv = {},
                             TcRunner  runner = {},
                             PrivCheck priv   = {});

    std::string name() const override { return "network_blackout"; }

    void onStart(ScenarioContext& ctx) override;
    void tick(ScenarioContext& ctx,
              std::int64_t now_ms,
              std::int64_t elapsed_ms,
              std::vector<ScenarioEvent>& out) override;
    void onFinish(ScenarioContext& ctx,
                  std::vector<ScenarioEvent>& out) override;

private:
    struct Pending {
        std::string  iface;
        std::int64_t restore_at_ms = 0;
    };

    void scheduleNext(ScenarioContext& ctx, std::int64_t now_ms);
    void doKill(ScenarioContext& ctx, std::int64_t now_ms,
                std::vector<ScenarioEvent>& out);
    void doRestore(ScenarioContext& ctx, std::int64_t now_ms,
                   std::vector<ScenarioEvent>& out);

    std::vector<std::string>  interfaces_;
    NetworkBlackoutIntervals  iv_{};
    TcRunner                  runner_;
    PrivCheck                 priv_;
    std::int64_t              next_fail_at_ms_ = 0;
    std::optional<Pending>    pending_;
    int                       kills_    = 0;
    int                       restores_ = 0;
    bool                      privileged_ = false;
};

}  // namespace liveqx::stress
