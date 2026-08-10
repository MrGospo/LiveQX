#include "metrics/ChannelHealth.h"
#include "metrics/ChannelMetrics.h"
#include <gtest/gtest.h>

namespace {
constexpr std::int64_t S = 1'000'000'000LL;  // 1 second in ns

ChannelMetricsSnapshot clean_snapshot(int target_fps = 25) {
    ChannelMetricsSnapshot s;
    s.rendered_10s   = static_cast<std::uint64_t>(target_fps) * 10;  // healthy fps
    s.last_tick_ns   = 1;  // anything non-zero
    return s;
}
}  // namespace

TEST(ChannelHealth, StartsRunning) {
    ChannelHealth h("1", 25);
    EXPECT_EQ(h.state(), HealthState::Running);
}

TEST(ChannelHealth, DegradesOnUnderrun) {
    ChannelHealth h("1", 25);
    auto s = clean_snapshot();
    s.underruns_10s = 1;
    h.evaluate(s, 10 * S, 0);
    EXPECT_EQ(h.state(), HealthState::Degraded);
}

TEST(ChannelHealth, DegradesOnExcessiveDrops) {
    ChannelHealth h("1", 25);
    auto s = clean_snapshot();
    s.drops_10s = 11;
    h.evaluate(s, 10 * S, 0);
    EXPECT_EQ(h.state(), HealthState::Degraded);
}

TEST(ChannelHealth, FailsImmediatelyOnHeartbeatLoss) {
    ChannelHealth h("1", 25);
    auto s = clean_snapshot();
    h.evaluate(s, 10 * S, 6 * S);  // heartbeat 6s old
    EXPECT_EQ(h.state(), HealthState::Failed);
}

TEST(ChannelHealth, FailsOnFpsStarvation) {
    ChannelHealth h("1", 25);
    ChannelMetricsSnapshot s;
    s.rendered_10s = 50;  // 5 fps over 10s — way below 0.5×25 = 12.5
    s.last_tick_ns = 1;
    // First evaluate seeds grace window; need a second eval after >=10s.
    h.evaluate(s, 0, 0);
    h.evaluate(s, 11 * S, 0);
    EXPECT_EQ(h.state(), HealthState::Failed);
}

TEST(ChannelHealth, FpsGraceSkipsEarlyEvaluation) {
    ChannelHealth h("1", 25);
    ChannelMetricsSnapshot s;
    s.rendered_10s = 25;  // only 1s of data, would falsely flag fps_starved
    s.last_tick_ns = 1;
    h.evaluate(s, 0, 0);       // first eval seeds grace
    h.evaluate(s, 1 * S, 0);   // 1s in — still within grace
    EXPECT_EQ(h.state(), HealthState::Running);
    h.evaluate(s, 5 * S, 0);   // 5s in — still within grace
    EXPECT_EQ(h.state(), HealthState::Running);
}

TEST(ChannelHealth, DegradedPromotesToFailedAfter30s) {
    ChannelHealth h("1", 25);
    auto bad = clean_snapshot();
    bad.underruns_10s = 1;
    h.evaluate(bad, 0, 0);
    ASSERT_EQ(h.state(), HealthState::Degraded);

    // 31s of continued degraded signal
    h.evaluate(bad, 31 * S, 0);
    EXPECT_EQ(h.state(), HealthState::Failed);
}

TEST(ChannelHealth, RecoversAfter30sClean) {
    ChannelHealth h("1", 25);
    auto bad = clean_snapshot();
    bad.underruns_10s = 1;
    h.evaluate(bad, 0, 0);
    ASSERT_EQ(h.state(), HealthState::Degraded);

    auto good = clean_snapshot();
    h.evaluate(good, 1 * S, 0);   // first clean tick
    EXPECT_EQ(h.state(), HealthState::Degraded);
    h.evaluate(good, 31 * S, 0);  // 30s of clean
    EXPECT_EQ(h.state(), HealthState::Running);
}

TEST(ChannelHealth, FailedRecoversOneStep) {
    ChannelHealth h("1", 25);
    auto bad = clean_snapshot();
    bad.underruns_10s = 1;
    h.evaluate(bad, 0,        6 * S);  // heartbeat → Failed
    ASSERT_EQ(h.state(), HealthState::Failed);

    auto good = clean_snapshot();
    h.evaluate(good, 1 * S, 0);
    h.evaluate(good, 31 * S, 0);
    EXPECT_EQ(h.state(), HealthState::Degraded);  // not Running yet
}

TEST(ChannelHealth, BadSignalDuringRecoveryResetsStreak) {
    ChannelHealth h("1", 25);
    auto bad = clean_snapshot();
    bad.underruns_10s = 1;
    h.evaluate(bad, 0, 0);
    auto good = clean_snapshot();
    h.evaluate(good, 20 * S, 0);
    h.evaluate(bad,  21 * S, 0);  // hiccup resets clean streak
    h.evaluate(good, 22 * S, 0);
    h.evaluate(good, 30 * S, 0);  // only 8s clean since hiccup
    EXPECT_EQ(h.state(), HealthState::Degraded);
    h.evaluate(good, 60 * S, 0);  // 38s clean since hiccup
    EXPECT_EQ(h.state(), HealthState::Running);
}

// ── fix12 c7: Degraded on partial output failure ─────────────────────────────

TEST(ChannelHealthOutputs, DegradedWhenSomeOutputsDown) {
    ChannelHealth h("1", 25);
    auto s = clean_snapshot();
    h.evaluate(s, 10 * S, 0, OutputHealthSummary{1, 2});
    EXPECT_EQ(h.state(), HealthState::Degraded);
}

TEST(ChannelHealthOutputs, FailedWhenAllOutputsDown) {
    ChannelHealth h("1", 25);
    auto s = clean_snapshot();
    h.evaluate(s, 10 * S, 0, OutputHealthSummary{0, 2});
    EXPECT_EQ(h.state(), HealthState::Failed);
}

TEST(ChannelHealthOutputs, RunningWhenAllOutputsHealthy) {
    ChannelHealth h("1", 25);
    auto s = clean_snapshot();
    h.evaluate(s, 10 * S, 0, OutputHealthSummary{2, 2});
    EXPECT_EQ(h.state(), HealthState::Running);
}

TEST(ChannelHealthOutputs, ZeroTotalIsNeutral) {
    // total=0 means "no probe data" — must not trigger Degraded/Failed
    // even though healthy < total trivially holds (0 < 0 false here).
    ChannelHealth h("1", 25);
    auto s = clean_snapshot();
    h.evaluate(s, 10 * S, 0, OutputHealthSummary{0, 0});
    EXPECT_EQ(h.state(), HealthState::Running);
}

TEST(ChannelHealthOutputs, RecoversToRunningWhenAllOutputsBackUp) {
    ChannelHealth h("1", 25);
    auto s = clean_snapshot();
    h.evaluate(s, 0,        0, OutputHealthSummary{1, 2});  // → Degraded
    EXPECT_EQ(h.state(), HealthState::Degraded);
    h.evaluate(s, 1 * S,    0, OutputHealthSummary{2, 2});  // outputs back
    h.evaluate(s, 31 * S,   0, OutputHealthSummary{2, 2});  // 30s clean
    EXPECT_EQ(h.state(), HealthState::Running);
}

TEST(ChannelHealthOutputs, AllDownPromotesFromDegradedToFailed) {
    ChannelHealth h("1", 25);
    auto s = clean_snapshot();
    h.evaluate(s, 0,    0, OutputHealthSummary{1, 2});  // → Degraded
    h.evaluate(s, 1*S,  0, OutputHealthSummary{0, 2});  // all down → Failed
    EXPECT_EQ(h.state(), HealthState::Failed);
}
