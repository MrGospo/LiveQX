// fix26 c2 — ChannelProfiler unit tests.
//
// Covers Off/Sampling/Instrumentation transitions, hot-path no-op semantics
// in Off mode, accumulator preservation across mode switches, reset(), and
// snapshot consistency.

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "metrics/ChannelProfiler.h"

namespace prof = liveqx::profiler;

TEST(ChannelProfilerNames, StageNames) {
    EXPECT_STREQ(prof::stageName(prof::Stage::None),    "none");
    EXPECT_STREQ(prof::stageName(prof::Stage::Decode),  "decode");
    EXPECT_STREQ(prof::stageName(prof::Stage::Compose), "compose");
    EXPECT_STREQ(prof::stageName(prof::Stage::Encode),  "encode");
    EXPECT_STREQ(prof::stageName(prof::Stage::Output),  "output");
    EXPECT_STREQ(prof::stageName(prof::Stage::Total),   "total");
}

TEST(ChannelProfilerNames, ModeParse) {
    EXPECT_EQ(prof::parseMode("off"),             prof::Mode::Off);
    EXPECT_EQ(prof::parseMode("sampling"),        prof::Mode::Sampling);
    EXPECT_EQ(prof::parseMode("INSTRUMENTATION"), prof::Mode::Instrumentation);
    EXPECT_EQ(prof::parseMode("Sampling"),        prof::Mode::Sampling);
    EXPECT_EQ(prof::parseMode(""),                prof::Mode::Off);
    EXPECT_EQ(prof::parseMode("garbage"),         prof::Mode::Off);
}

TEST(ChannelProfiler, DefaultIsOff) {
    prof::ChannelProfiler p;
    EXPECT_EQ(p.mode(), prof::Mode::Off);
    EXPECT_EQ(p.currentStage(), prof::Stage::None);
}

TEST(ChannelProfiler, OffEnterLeaveDoesNothing) {
    prof::ChannelProfiler p;
    p.enterStage(prof::Stage::Decode);
    p.leaveStage(prof::Stage::Decode);
    p.enterStage(prof::Stage::Encode);
    p.leaveStage(prof::Stage::Encode);

    const auto snap = p.snapshot();
    for (auto v : snap.stage_us)     EXPECT_EQ(v, 0u);
    for (auto v : snap.stage_count)  EXPECT_EQ(v, 0u);
    for (auto v : snap.sampled_hits) EXPECT_EQ(v, 0u);
    EXPECT_EQ(snap.active_ms, 0);
    EXPECT_EQ(p.currentStage(), prof::Stage::None);
}

TEST(ChannelProfiler, SamplingTracksCurrentStage) {
    prof::ChannelProfiler p;
    p.setMode(prof::Mode::Sampling);

    p.enterStage(prof::Stage::Decode);
    EXPECT_EQ(p.currentStage(), prof::Stage::Decode);

    p.recordSample(p.currentStage());
    p.recordSample(p.currentStage());

    p.leaveStage(prof::Stage::Decode);
    EXPECT_EQ(p.currentStage(), prof::Stage::None);

    p.enterStage(prof::Stage::Encode);
    p.recordSample(p.currentStage());
    p.leaveStage(prof::Stage::Encode);

    const auto snap = p.snapshot();
    EXPECT_EQ(snap.sampled_hits[static_cast<std::size_t>(prof::Stage::Decode)], 2u);
    EXPECT_EQ(snap.sampled_hits[static_cast<std::size_t>(prof::Stage::Encode)], 1u);
    EXPECT_EQ(snap.sampled_hits[static_cast<std::size_t>(prof::Stage::Compose)], 0u);

    // Sampling mode does not populate stage_us / stage_count.
    for (auto v : snap.stage_us)    EXPECT_EQ(v, 0u);
    for (auto v : snap.stage_count) EXPECT_EQ(v, 0u);
}

TEST(ChannelProfiler, InstrumentationAccumulatesUs) {
    prof::ChannelProfiler p;
    p.setMode(prof::Mode::Instrumentation);

    // Sleep windows are coarse on CI, so we just check monotonicity.
    p.enterStage(prof::Stage::Decode);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    p.leaveStage(prof::Stage::Decode);

    p.enterStage(prof::Stage::Encode);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    p.leaveStage(prof::Stage::Encode);

    p.enterStage(prof::Stage::Decode);
    std::this_thread::sleep_for(std::chrono::milliseconds(3));
    p.leaveStage(prof::Stage::Decode);

    const auto snap = p.snapshot();
    const auto decode_us = snap.stage_us[static_cast<std::size_t>(prof::Stage::Decode)];
    const auto encode_us = snap.stage_us[static_cast<std::size_t>(prof::Stage::Encode)];

    EXPECT_GT(decode_us, encode_us);   // 5ms total > 1ms
    EXPECT_GT(decode_us, 0u);
    EXPECT_GT(encode_us, 0u);
    EXPECT_EQ(snap.stage_count[static_cast<std::size_t>(prof::Stage::Decode)], 2u);
    EXPECT_EQ(snap.stage_count[static_cast<std::size_t>(prof::Stage::Encode)], 1u);

    // No samples were recorded externally.
    for (auto v : snap.sampled_hits) EXPECT_EQ(v, 0u);
}

TEST(ChannelProfiler, ResetClearsCounters) {
    prof::ChannelProfiler p;
    p.setMode(prof::Mode::Instrumentation);
    p.enterStage(prof::Stage::Decode);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    p.leaveStage(prof::Stage::Decode);
    p.recordSample(prof::Stage::Decode);

    ASSERT_GT(p.snapshot().stage_us[static_cast<std::size_t>(prof::Stage::Decode)], 0u);

    p.reset();
    const auto snap = p.snapshot();
    for (auto v : snap.stage_us)     EXPECT_EQ(v, 0u);
    for (auto v : snap.stage_count)  EXPECT_EQ(v, 0u);
    for (auto v : snap.sampled_hits) EXPECT_EQ(v, 0u);
    // Mode is preserved across reset.
    EXPECT_EQ(snap.mode, prof::Mode::Instrumentation);
}

TEST(ChannelProfiler, ModeSwitchPreservesAccumulators) {
    prof::ChannelProfiler p;
    p.setMode(prof::Mode::Instrumentation);
    p.enterStage(prof::Stage::Decode);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    p.leaveStage(prof::Stage::Decode);

    const auto before = p.snapshot().stage_us[static_cast<std::size_t>(prof::Stage::Decode)];
    ASSERT_GT(before, 0u);

    // Off -> previous accumulators should still be visible.
    p.setMode(prof::Mode::Off);
    EXPECT_EQ(p.snapshot().stage_us[static_cast<std::size_t>(prof::Stage::Decode)], before);

    // Re-enabling mode keeps the old data.
    p.setMode(prof::Mode::Sampling);
    EXPECT_EQ(p.snapshot().stage_us[static_cast<std::size_t>(prof::Stage::Decode)], before);
}

TEST(ChannelProfiler, ActiveMsTicks) {
    prof::ChannelProfiler p;
    p.setMode(prof::Mode::Sampling);
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    const auto snap = p.snapshot();
    // Coarse: the timer is at least 10ms (10 < x < 1000).
    EXPECT_GE(snap.active_ms, 10);
    EXPECT_LT(snap.active_ms, 1000);
}

TEST(ChannelProfiler, ActiveMsFreezesWhenOff) {
    prof::ChannelProfiler p;
    p.setMode(prof::Mode::Sampling);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    p.setMode(prof::Mode::Off);
    const auto frozen = p.snapshot().active_ms;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_EQ(p.snapshot().active_ms, frozen);
}
