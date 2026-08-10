// fix26 c3 — ProfileSampler unit tests.
//
// Cover registration, unregistration, deterministic single-pass sampling
// via sampleOnce(), and the periodic thread actually firing.

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "metrics/ChannelProfiler.h"
#include "metrics/ProfileSampler.h"

namespace prof = liveqx::profiler;

TEST(ProfileSampler, RegisterUnregisterCount) {
    prof::ProfileSampler s(std::chrono::milliseconds(1000));
    EXPECT_EQ(s.registeredCount(), 0u);

    prof::ChannelProfiler p1, p2;
    s.registerChannel("ch1", &p1);
    s.registerChannel("ch2", &p2);
    EXPECT_EQ(s.registeredCount(), 2u);

    s.unregisterChannel("ch1");
    EXPECT_EQ(s.registeredCount(), 1u);
    s.unregisterChannel("ch2");
    EXPECT_EQ(s.registeredCount(), 0u);
}

TEST(ProfileSampler, NullPointerRegistrationIgnored) {
    prof::ProfileSampler s(std::chrono::milliseconds(1000));
    s.registerChannel("ch", nullptr);
    EXPECT_EQ(s.registeredCount(), 0u);
}

TEST(ProfileSampler, SampleOnceSkipsOffMode) {
    prof::ProfileSampler s(std::chrono::milliseconds(1000));
    prof::ChannelProfiler p;
    s.registerChannel("ch", &p);

    // Off mode: enterStage is a no-op, sampleOnce should not record anything.
    p.enterStage(prof::Stage::Decode);
    s.sampleOnce();

    const auto snap = p.snapshot();
    for (auto v : snap.sampled_hits) EXPECT_EQ(v, 0u);
}

TEST(ProfileSampler, SampleOnceRecordsCurrentStage) {
    prof::ProfileSampler s(std::chrono::milliseconds(1000));
    prof::ChannelProfiler p;
    p.setMode(prof::Mode::Sampling);
    s.registerChannel("ch", &p);

    p.enterStage(prof::Stage::Encode);
    s.sampleOnce();
    s.sampleOnce();
    p.leaveStage(prof::Stage::Encode);

    p.enterStage(prof::Stage::Decode);
    s.sampleOnce();
    p.leaveStage(prof::Stage::Decode);

    const auto snap = p.snapshot();
    EXPECT_EQ(snap.sampled_hits[static_cast<std::size_t>(prof::Stage::Encode)], 2u);
    EXPECT_EQ(snap.sampled_hits[static_cast<std::size_t>(prof::Stage::Decode)], 1u);
}

TEST(ProfileSampler, SampleOnceSkipsInstrumentation) {
    prof::ProfileSampler s(std::chrono::milliseconds(1000));
    prof::ChannelProfiler p;
    p.setMode(prof::Mode::Instrumentation);
    s.registerChannel("ch", &p);

    p.enterStage(prof::Stage::Decode);
    s.sampleOnce();
    p.leaveStage(prof::Stage::Decode);

    const auto snap = p.snapshot();
    // Instrumentation path should NOT bump sampled_hits.
    for (auto v : snap.sampled_hits) EXPECT_EQ(v, 0u);
    // But it should have accumulated decode us via leaveStage.
    EXPECT_GE(snap.stage_count[static_cast<std::size_t>(prof::Stage::Decode)], 1u);
}

TEST(ProfileSampler, PeriodicThreadFires) {
    // 5ms period — sampler should fire at least once in 50ms wall.
    prof::ProfileSampler s(std::chrono::milliseconds(5));
    prof::ChannelProfiler p;
    p.setMode(prof::Mode::Sampling);
    s.registerChannel("ch", &p);

    p.enterStage(prof::Stage::Decode);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    p.leaveStage(prof::Stage::Decode);

    const auto snap = p.snapshot();
    const auto hits = snap.sampled_hits[static_cast<std::size_t>(prof::Stage::Decode)];
    EXPECT_GE(hits, 1u) << "sampler thread did not record any hits in 50ms";
}

TEST(ProfileSampler, UnregisterStopsSampling) {
    prof::ProfileSampler s(std::chrono::milliseconds(1000));
    prof::ChannelProfiler p;
    p.setMode(prof::Mode::Sampling);
    s.registerChannel("ch", &p);

    p.enterStage(prof::Stage::Encode);
    s.sampleOnce();
    s.unregisterChannel("ch");
    s.sampleOnce();   // no longer registered
    p.leaveStage(prof::Stage::Encode);

    const auto snap = p.snapshot();
    EXPECT_EQ(snap.sampled_hits[static_cast<std::size_t>(prof::Stage::Encode)], 1u);
}
