// fix26 c7 — random_output_fail scenario.
//
// Periodically (every fail_window seconds, randomized) picks one running
// channel + one of its outputs, removes the output to simulate a downstream
// failure, then re-adds it after a randomized restore_window so the
// StressRunner can verify recovery.
//
// State per scenario instance:
//   - next_fail_at_ms_:    when to trigger the next "kill"
//   - pending_:            the most recent kill that has not yet been
//                          restored (one in flight at a time, by design)
//
// Kill/restore intervals are constructor-injectable so tests can drive
// the state machine deterministically without waiting minutes.

#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "stress/Scenario.h"

namespace liveqx::stress {

struct RandomOutputFailIntervals {
    std::int64_t fail_min_ms    = 5  * 60 * 1000;   // every 5..15 minutes
    std::int64_t fail_max_ms    = 15 * 60 * 1000;
    std::int64_t restore_min_ms = 30 * 1000;        // restore after 30..120s
    std::int64_t restore_max_ms = 120 * 1000;
};

class RandomOutputFail final : public IScenario {
public:
    RandomOutputFail() = default;
    explicit RandomOutputFail(RandomOutputFailIntervals iv) : iv_(iv) {}

    std::string name() const override { return "random_output_fail"; }

    void onStart(ScenarioContext& ctx) override;
    void tick(ScenarioContext& ctx,
              std::int64_t now_ms,
              std::int64_t elapsed_ms,
              std::vector<ScenarioEvent>& out) override;
    void onFinish(ScenarioContext& ctx,
                  std::vector<ScenarioEvent>& out) override;

private:
    struct Pending {
        int            channel_id = 0;
        std::string    output_id;
        nlohmann::json saved_cfg;
        std::int64_t   restore_at_ms = 0;
    };

    void scheduleNextFail(ScenarioContext& ctx, std::int64_t now_ms);
    void doKill(ScenarioContext& ctx, std::int64_t now_ms,
                std::vector<ScenarioEvent>& out);
    void doRestore(ScenarioContext& ctx, std::int64_t now_ms,
                   std::vector<ScenarioEvent>& out);

    RandomOutputFailIntervals iv_{};
    std::int64_t              next_fail_at_ms_ = 0;
    std::optional<Pending>    pending_;
    int                       kills_   = 0;
    int                       restores_= 0;
};

}  // namespace liveqx::stress
