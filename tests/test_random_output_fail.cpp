// fix26 c7 — unit tests for RandomOutputFail scenario.
//
// Drives the scenario directly against a real ChannelManager + a stopped
// channel pre-populated with two outputs. The scenario doesn't care
// whether the channel is "running" — it only manipulates output config
// via mgr.outputsJson / addOutput / removeOutput, which all work on
// stopped channels too. This keeps the test offline (no network, no
// codec spinup) while still exercising the real ChannelManager surface.
//
// Intervals are tightened to milliseconds so each test runs in a few ms.

#include <chrono>
#include <thread>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "api/ChannelManager.h"
#include "stress/RandomOutputFail.h"
#include "stress/Scenario.h"

using nlohmann::json;
using liveqx::stress::IScenario;
using liveqx::stress::makeScenario;
using liveqx::stress::RandomOutputFail;
using liveqx::stress::RandomOutputFailIntervals;
using liveqx::stress::ScenarioContext;
using liveqx::stress::ScenarioEvent;

namespace {

json minimalCfgWithTwoOutputs(int id) {
    return json{
        {"id",        id},
        {"name",      "stress-source"},
        {"resolution","320x240"},
        {"fps",       25},
        {"bitrate",   1'000'000},
        {"preset",    "ultrafast"},
        {"outputs", {
            {{"id", "out-A"}, {"type", "srt"},
             {"port", 49600}, {"latency_ms", 200}},
            {{"id", "out-B"}, {"type", "srt"},
             {"port", 49601}, {"latency_ms", 200}},
        }},
    };
}

ScenarioContext makeCtx(ChannelManager* mgr,
                        const std::vector<int>& ids,
                        std::int64_t started_ms,
                        std::uint64_t seed) {
    ScenarioContext ctx;
    ctx.mgr            = mgr;
    ctx.channel_ids    = ids;
    ctx.run_started_ms = started_ms;
    ctx.rng.seed(seed);
    return ctx;
}

}  // namespace

TEST(RandomOutputFail, NoActionBeforeFirstFailWindow) {
    ChannelManager mgr;
    ASSERT_EQ(mgr.create(minimalCfgWithTwoOutputs(7)),
              ChannelManager::Result::Ok);

    RandomOutputFailIntervals iv{5000, 5000, 200, 200};  // fail at 5s exactly
    RandomOutputFail sc(iv);
    auto ctx = makeCtx(&mgr, {7}, /*started_ms=*/0, /*seed=*/123);
    sc.onStart(ctx);

    std::vector<ScenarioEvent> evs;
    sc.tick(ctx, /*now_ms=*/100, /*elapsed_ms=*/100, evs);
    EXPECT_TRUE(evs.empty());
    sc.tick(ctx, /*now_ms=*/4999, /*elapsed_ms=*/4999, evs);
    EXPECT_TRUE(evs.empty());

    auto outs = mgr.outputsJson(7);
    ASSERT_TRUE(outs.is_array());
    EXPECT_EQ(outs.size(), 2u);  // nothing killed yet
}

TEST(RandomOutputFail, FiresKillThenRestoreCycle) {
    ChannelManager mgr;
    ASSERT_EQ(mgr.create(minimalCfgWithTwoOutputs(8)),
              ChannelManager::Result::Ok);

    // fail in 1ms; restore 5ms later — deterministic for the test.
    RandomOutputFailIntervals iv{1, 1, 5, 5};
    RandomOutputFail sc(iv);
    auto ctx = makeCtx(&mgr, {8}, /*started_ms=*/0, /*seed=*/42);
    sc.onStart(ctx);

    // First tick at t=2ms -> beyond next_fail (1ms) -> kill.
    std::vector<ScenarioEvent> evs;
    sc.tick(ctx, /*now_ms=*/2, /*elapsed_ms=*/2, evs);
    ASSERT_EQ(evs.size(), 1u);
    EXPECT_TRUE(evs[0].ok) << evs[0].detail;
    EXPECT_NE(evs[0].detail.find("killed output"), std::string::npos);

    auto outs_after_kill = mgr.outputsJson(8);
    ASSERT_TRUE(outs_after_kill.is_array());
    EXPECT_EQ(outs_after_kill.size(), 1u);  // one output removed

    // Tick before restore window passes -> no-op.
    sc.tick(ctx, /*now_ms=*/3, /*elapsed_ms=*/3, evs);
    EXPECT_EQ(evs.size(), 1u);

    // Tick at t=10ms (>2ms kill + 5ms restore) -> restore fires, then a
    // new kill could schedule too, but next_fail is +1ms so it likely fires
    // again on the same tick. We assert at least the restore happened.
    sc.tick(ctx, /*now_ms=*/10, /*elapsed_ms=*/10, evs);
    bool saw_restore = false;
    for (const auto& e : evs) {
        if (e.detail.find("restored output") != std::string::npos) saw_restore = true;
    }
    EXPECT_TRUE(saw_restore);

    auto outs_after_restore = mgr.outputsJson(8);
    ASSERT_TRUE(outs_after_restore.is_array());
    // restore added back -> count returns to 2 (we already may have
    // killed again on this same tick, so accept either 1 or 2).
    EXPECT_GE(outs_after_restore.size(), 1u);
    EXPECT_LE(outs_after_restore.size(), 2u);
}

TEST(RandomOutputFail, OnFinishRestoresPendingKill) {
    ChannelManager mgr;
    ASSERT_EQ(mgr.create(minimalCfgWithTwoOutputs(9)),
              ChannelManager::Result::Ok);

    RandomOutputFailIntervals iv{1, 1, /*restore_min*/100000, /*restore_max*/100000};
    RandomOutputFail sc(iv);
    auto ctx = makeCtx(&mgr, {9}, 0, 1);
    sc.onStart(ctx);
    std::vector<ScenarioEvent> evs;
    sc.tick(ctx, 5, 5, evs);  // kill — restore_at far in future
    ASSERT_EQ(mgr.outputsJson(9).size(), 1u);

    // Run ends — onFinish must restore.
    sc.onFinish(ctx, evs);
    EXPECT_EQ(mgr.outputsJson(9).size(), 2u);
}

TEST(RandomOutputFail, EmptyChannelListIsNoOp) {
    ChannelManager mgr;
    RandomOutputFailIntervals iv{1, 1, 1, 1};
    RandomOutputFail sc(iv);
    auto ctx = makeCtx(&mgr, /*ids=*/{}, 0, 7);
    sc.onStart(ctx);
    std::vector<ScenarioEvent> evs;
    sc.tick(ctx, 100, 100, evs);
    // Either no event, or an event with ok=true that simply re-schedules.
    // Either way, no crash and no manager mutation.
    EXPECT_EQ(evs.size(), 0u);
}

TEST(RandomOutputFail, ChannelWithoutOutputsLogsBadEvent) {
    ChannelManager mgr;
    auto cfg = json{
        {"id", 10}, {"name","empty"},
        {"resolution","320x240"}, {"fps",25},
        {"bitrate",1'000'000}, {"preset","ultrafast"},
        {"output", {{"port",49700},{"latency_ms",200}}},  // legacy single output
    };
    ASSERT_EQ(mgr.create(cfg), ChannelManager::Result::Ok);
    // Remove the only output so outputsJson returns [].
    auto outs = mgr.outputsJson(10);
    ASSERT_TRUE(outs.is_array());
    if (!outs.empty() && outs[0].contains("id") && outs[0]["id"].is_string()) {
        mgr.removeOutput(10, outs[0]["id"].get<std::string>());
    }

    RandomOutputFailIntervals iv{1, 1, 5, 5};
    RandomOutputFail sc(iv);
    auto ctx = makeCtx(&mgr, {10}, 0, 17);
    sc.onStart(ctx);
    std::vector<ScenarioEvent> evs;
    sc.tick(ctx, 5, 5, evs);
    ASSERT_EQ(evs.size(), 1u);
    EXPECT_FALSE(evs[0].ok);
    EXPECT_NE(evs[0].detail.find("no outputs"), std::string::npos);
}

TEST(ScenarioFactory, KnownAndUnknownNames) {
    auto good = makeScenario("random_output_fail");
    ASSERT_NE(good, nullptr);
    EXPECT_EQ(good->name(), "random_output_fail");
    auto bad = makeScenario("not_a_thing");
    EXPECT_EQ(bad, nullptr);
}
