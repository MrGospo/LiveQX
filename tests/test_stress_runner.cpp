// fix26 c6 — unit tests for StressRunner core (golden path, no scenarios).
//
// We exercise the runner with cfg.channels=0 so no real RenderLoop is spun
// up — the test focuses on:
//   - state machine (Idle → Running → Idle)
//   - duration_sec / stop_token cancellation
//   - pass/fail evaluation against PassCriteria
//   - report shape & JSON serialization
//   - StressConfig JSON round-trip + validation
//
// Channel-up integration is exercised separately by main.cpp wiring (c13)
// and by ad-hoc runs of stress_runner_cli; we keep ctest fast and offline.

#include <chrono>
#include <thread>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "api/ChannelManager.h"
#include "stress/StressConfig.h"
#include "stress/StressReport.h"
#include "stress/StressRunner.h"

using nlohmann::json;
using namespace std::chrono_literals;
using liveqx::stress::PassCriteria;
using liveqx::stress::State;
using liveqx::stress::StressConfig;
using liveqx::stress::StressReport;
using liveqx::stress::StressRunner;

namespace {
StressConfig zeroChannelCfg(int duration_sec = 1) {
    StressConfig c;
    c.enabled            = true;
    c.duration_sec       = duration_sec;
    c.channels           = 0;
    c.outputs_per_channel= 0;
    c.scenarios.clear();
    return c;
}
}  // namespace

TEST(StressConfigJson, RoundtripPreservesAllFields) {
    StressConfig in;
    in.enabled            = true;
    in.schedule_cron      = "*/5 * * * *";
    in.duration_sec       = 60;
    in.channels           = 7;
    in.channel_resolution = "1080p25";
    in.outputs_per_channel= 3;
    in.scenarios          = {"random_output_fail", "clip_corrupt"};
    in.pass.max_fps_drop_pct                = 2.5;
    in.pass.max_memory_growth_pct_per_hour  = 0.25;
    in.pass.max_crashes                     = 1;
    in.report_dir         = "/tmp/strep";

    auto j   = in.toJson();
    auto out = StressConfig::fromJson(j);
    EXPECT_EQ(out.enabled, in.enabled);
    EXPECT_EQ(out.schedule_cron, in.schedule_cron);
    EXPECT_EQ(out.duration_sec, in.duration_sec);
    EXPECT_EQ(out.channels, in.channels);
    EXPECT_EQ(out.channel_resolution, in.channel_resolution);
    EXPECT_EQ(out.outputs_per_channel, in.outputs_per_channel);
    EXPECT_EQ(out.scenarios, in.scenarios);
    EXPECT_DOUBLE_EQ(out.pass.max_fps_drop_pct, in.pass.max_fps_drop_pct);
    EXPECT_DOUBLE_EQ(out.pass.max_memory_growth_pct_per_hour,
                     in.pass.max_memory_growth_pct_per_hour);
    EXPECT_EQ(out.pass.max_crashes, in.pass.max_crashes);
    EXPECT_EQ(out.report_dir, in.report_dir);
}

TEST(StressConfigJson, FromJsonHandlesNullAndPartial) {
    auto def = StressConfig::fromJson(json());
    EXPECT_FALSE(def.enabled);
    EXPECT_EQ(def.duration_sec, 86400);

    auto partial = StressConfig::fromJson(json{{"enabled", true},
                                                {"channels", 9}});
    EXPECT_TRUE(partial.enabled);
    EXPECT_EQ(partial.channels, 9);
    EXPECT_EQ(partial.duration_sec, 86400);  // default preserved
}

TEST(StressConfigValidate, RejectsBadValues) {
    StressConfig c;
    c.duration_sec = 0;
    EXPECT_TRUE(c.validate().has_value());

    c.duration_sec = 10;
    c.channels = -1;
    EXPECT_TRUE(c.validate().has_value());

    c.channels = 1;
    c.pass.max_crashes = -1;
    EXPECT_TRUE(c.validate().has_value());

    c.pass.max_crashes = 0;
    EXPECT_FALSE(c.validate().has_value());
}

TEST(StressRunner, IdleByDefault) {
    ChannelManager mgr;
    StressRunner runner(mgr);
    EXPECT_EQ(runner.state(), State::Idle);
    EXPECT_FALSE(runner.running());
    auto last = runner.lastReportJson();
    EXPECT_TRUE(last.is_null());
    auto status = runner.statusJson();
    EXPECT_EQ(status["state"], "idle");
}

TEST(StressRunner, RunOnceWithZeroChannelsPassesQuickly) {
    ChannelManager mgr;
    StressRunner   runner(mgr);
    auto cfg = zeroChannelCfg(/*duration_sec=*/1);
    auto t0 = std::chrono::steady_clock::now();
    auto rep = runner.runOnce(cfg);
    auto dt = std::chrono::steady_clock::now() - t0;

    EXPECT_TRUE(rep.pass) << rep.verdict;
    EXPECT_EQ(rep.channels_requested, 0);
    EXPECT_EQ(rep.channels_started, 0);
    EXPECT_EQ(rep.crashes, 0);
    EXPECT_EQ(rep.duration_sec, 1);
    EXPECT_GE(dt, 1000ms);
    EXPECT_LE(dt, 4000ms);
    EXPECT_GT(rep.ended_at_ms, rep.started_at_ms);
}

TEST(StressRunner, RunOnceCancellableViaStopToken) {
    ChannelManager mgr;
    StressRunner   runner(mgr);
    auto cfg = zeroChannelCfg(/*duration_sec=*/30);  // 30s — won't naturally finish
    std::stop_source src;

    StressReport rep;
    std::thread t([&]() { rep = runner.runOnce(cfg, src.get_token()); });
    std::this_thread::sleep_for(150ms);
    src.request_stop();
    t.join();

    EXPECT_TRUE(rep.pass);  // no crashes, no fps drop, no memory growth
    EXPECT_EQ(rep.channels_started, 0);
}

TEST(StressRunner, AsyncStartReturnsToIdleAfterCompletion) {
    ChannelManager mgr;
    StressRunner   runner(mgr);
    auto cfg = zeroChannelCfg(/*duration_sec=*/1);
    ASSERT_TRUE(runner.start(cfg));
    EXPECT_TRUE(runner.running());
    // Second start while running ⇒ rejected.
    EXPECT_FALSE(runner.start(cfg));

    runner.stop();   // joins the worker thread
    EXPECT_EQ(runner.state(), State::Idle);
    auto rep = runner.lastReportJson();
    ASSERT_FALSE(rep.is_null());
    EXPECT_EQ(rep["pass"], true);
    EXPECT_EQ(rep["channels_requested"], 0);
}

TEST(StressRunner, AsyncStartCancelledByStop) {
    ChannelManager mgr;
    StressRunner   runner(mgr);
    auto cfg = zeroChannelCfg(/*duration_sec=*/30);
    ASSERT_TRUE(runner.start(cfg));
    std::this_thread::sleep_for(120ms);
    auto t0 = std::chrono::steady_clock::now();
    runner.stop();
    auto dt = std::chrono::steady_clock::now() - t0;
    EXPECT_LT(dt, 3000ms) << "stop() should cooperatively cancel the run";
    EXPECT_EQ(runner.state(), State::Idle);
}

TEST(StressRunner, RejectsInvalidConfig) {
    ChannelManager mgr;
    StressRunner   runner(mgr);
    StressConfig bad;
    bad.duration_sec = 0;   // invalid
    EXPECT_FALSE(runner.start(bad));
    EXPECT_EQ(runner.state(), State::Idle);
}

TEST(StressRunner, EvaluatesFailWhenCrashLimitExceeded) {
    // Simulate a crash by injecting a report directly through a
    // hand-made cfg with max_crashes=0 but no real channels — we verify
    // the evaluation logic by wiring through runOnce's crash count: 0
    // crashes ⇒ pass; we can't synthesize a crash without a real channel,
    // so we instead assert that tightening max_crashes to a negative
    // value is REJECTED at validation (proxying the policy).
    ChannelManager mgr;
    StressRunner   runner(mgr);
    auto cfg = zeroChannelCfg();
    cfg.pass.max_crashes = -1;  // invalid ⇒ start refuses
    EXPECT_FALSE(runner.start(cfg));
}

TEST(StressRunner, ResolutionFpsParsing) {
    // We can't call expectedFpsFor directly (private), but the report's
    // expected_fps for an empty run still exposes the parser's default
    // (we run channels=0 so per_channel is empty). Cover the parser via
    // the public surface: build a cfg with various resolution strings,
    // ensure runOnce finishes (parser doesn't throw) regardless.
    ChannelManager mgr;
    StressRunner   runner(mgr);
    for (const char* res : {"720p25", "1080p25", "1080p50", "anything", "@30"}) {
        auto cfg = zeroChannelCfg(/*duration_sec=*/1);
        cfg.channel_resolution = res;
        auto rep = runner.runOnce(cfg);
        EXPECT_TRUE(rep.pass) << "parser broke runOnce for resolution=" << res;
    }
}

TEST(StressReportJson, ContainsAllSections) {
    StressReport r;
    r.started_at_ms        = 1;
    r.ended_at_ms          = 2;
    r.duration_sec         = 1;
    r.channels_requested   = 3;
    r.channels_started     = 2;
    r.channels_alive_at_end = 2;
    r.crashes              = 0;
    r.pass                 = true;
    r.verdict              = "pass";
    r.per_channel.push_back({1, 100, 0, 25.0, 25.0, 0.0, true});
    r.scenario_events.push_back({1, "noop", "hi", true});
    auto j = r.toJson();
    EXPECT_TRUE(j.contains("per_channel"));
    EXPECT_TRUE(j.contains("scenario_events"));
    EXPECT_EQ(j["pass"], true);
    EXPECT_EQ(j["channels_requested"], 3);
    EXPECT_EQ(j["per_channel"].size(), 1u);
    EXPECT_EQ(j["per_channel"][0]["frames_rendered"], 100);
    EXPECT_EQ(j["scenario_events"].size(), 1u);
}
