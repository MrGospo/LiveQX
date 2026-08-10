#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <utility>

#include "clips/LiveClip.h"
#include "clips/LiveInputClip.h"

using liveqx::LiveClip;

namespace {

// fix13 c3 — LiveClip skeleton tests.
//
// The state machine is the load-bearing piece in c3 (fallback hookup
// lands in c6, Timeline warm-up callback in c5). Here we pin every
// transition Idle → WarmingUp → Live ↔ Lost → Finished with a fake
// LiveInputClip that lets the test pretend the upstream is/isn't
// healthy and report a stamped lastPacketNs.

class FakeLiveInput : public LiveInputClip {
public:
    int prepare_calls   = 0;
    int release_calls   = 0;
    int frame_calls     = 0;
    bool prepared_      = false;
    bool has_audio_     = true;
    std::atomic<bool>          healthy_{false};
    std::atomic<std::int64_t>  last_pkt_ns_{0};
    int numa_node_      = -2;  // -1 means "unset", -2 sentinel for "never called"

    Frame      getFrame() override                         { ++frame_calls; return Frame{}; }
    AudioFrame getAudio(int n) override                    { return getTailAudio(n); }
    bool       hasAudio() const override                   { return has_audio_; }
    bool       isPrepared() const override                 { return prepared_; }
    void       prepare() override                          { ++prepare_calls; prepared_ = true; }
    void       release() override                          { ++release_calls; prepared_ = false; }

    bool          isHealthy() const override               { return healthy_.load(); }
    std::int64_t  lastPacketNs() const noexcept override   { return last_pkt_ns_.load(); }
    void          setNumaNode(int node) noexcept override  { numa_node_ = node; }
    nlohmann::json statusJson() const override             { return {{"fake", true}}; }
};

// Returns a non-owning observer to the fake so tests can poke
// healthy_ / last_pkt_ns_; ownership stays with the LiveClip.
LiveClip makeClip(FakeLiveInput*& out_fake,
                  std::uint64_t duration_ns       = 60'000'000'000ULL,
                  std::uint64_t warm_up_ns        =  5'000'000'000ULL,
                  std::uint64_t loss_threshold_ns =  2'000'000'000ULL) {
    auto fake = std::make_unique<FakeLiveInput>();
    out_fake = fake.get();
    LiveClip::Cfg cfg;
    cfg.id                = "studio";
    cfg.duration_ns       = duration_ns;
    cfg.warm_up_ns        = warm_up_ns;
    cfg.loss_threshold_ns = loss_threshold_ns;
    return LiveClip(std::move(cfg), std::move(fake));
}

// ── Construction ────────────────────────────────────────────────────────

TEST(LiveClipCtor, RejectsNullInput) {
    LiveClip::Cfg cfg;
    cfg.duration_ns = 1000;
    EXPECT_THROW(LiveClip(cfg, nullptr), std::invalid_argument);
}

TEST(LiveClipCtor, RejectsZeroDuration) {
    auto fake = std::make_unique<FakeLiveInput>();
    LiveClip::Cfg cfg;
    cfg.duration_ns = 0;
    EXPECT_THROW(LiveClip(std::move(cfg), std::move(fake)), std::invalid_argument);
}

TEST(LiveClipCtor, InitialStateIsIdle) {
    FakeLiveInput* fake = nullptr;
    auto clip = makeClip(fake);
    EXPECT_EQ(clip.state(), LiveClip::State::Idle);
    EXPECT_FALSE(clip.isPrepared());
    EXPECT_FALSE(clip.isFinished(0));
    // Without scheduled_start, isFinished is always false — guard against
    // accidental promotion to Finished on the first onTick.
    EXPECT_FALSE(clip.isFinished(1'000'000'000'000ULL));
}

// ── prepare / release idempotency ──────────────────────────────────────

TEST(LiveClipLifecycle, PrepareReleaseAreIdempotent) {
    FakeLiveInput* fake = nullptr;
    auto clip = makeClip(fake);

    clip.prepare();
    clip.prepare();
    EXPECT_EQ(fake->prepare_calls, 1);
    EXPECT_TRUE(clip.isPrepared());

    clip.release();
    clip.release();
    EXPECT_EQ(fake->release_calls, 1);
    EXPECT_FALSE(clip.isPrepared());
}

// ── Idle → WarmingUp ────────────────────────────────────────────────────

TEST(LiveClipStateMachine, IdleStaysIdleWithoutSchedule) {
    FakeLiveInput* fake = nullptr;
    auto clip = makeClip(fake);
    clip.onTick(1'000'000'000ULL);
    EXPECT_EQ(clip.state(), LiveClip::State::Idle);
    EXPECT_EQ(fake->prepare_calls, 0);
}

TEST(LiveClipStateMachine, IdleStaysIdleBeforeWarmupBoundary) {
    FakeLiveInput* fake = nullptr;
    auto clip = makeClip(fake);
    const std::uint64_t start = 100'000'000'000ULL;  // 100s
    clip.setScheduledStart(start);
    // 6s before start → outside 5s warm-up window
    clip.onTick(start - 6'000'000'000ULL);
    EXPECT_EQ(clip.state(), LiveClip::State::Idle);
    EXPECT_EQ(fake->prepare_calls, 0);
}

TEST(LiveClipStateMachine, IdleEntersWarmingUpAtBoundary) {
    FakeLiveInput* fake = nullptr;
    auto clip = makeClip(fake);
    const std::uint64_t start = 100'000'000'000ULL;
    clip.setScheduledStart(start);
    // Exactly at warm_up_ns boundary
    clip.onTick(start - 5'000'000'000ULL);
    EXPECT_EQ(clip.state(), LiveClip::State::WarmingUp);
    EXPECT_EQ(fake->prepare_calls, 1);
    EXPECT_TRUE(clip.isPrepared());
}

TEST(LiveClipStateMachine, IdleEntersWarmingUpEvenIfWarmupExceedsStart) {
    // Risk #6 in fix13 plan: hot-add live-clip with warm_up bigger than
    // the distance to start. State machine must still call prepare()
    // immediately rather than getting stuck in Idle.
    FakeLiveInput* fake = nullptr;
    auto clip = makeClip(fake, 60'000'000'000ULL, /*warm=*/10'000'000'000ULL);
    const std::uint64_t start = 3'000'000'000ULL;  // start in 3s, warm_up=10s
    clip.setScheduledStart(start);
    clip.onTick(0);
    EXPECT_EQ(clip.state(), LiveClip::State::WarmingUp);
    EXPECT_EQ(fake->prepare_calls, 1);
}

// ── WarmingUp → Live / Lost ────────────────────────────────────────────

TEST(LiveClipStateMachine, WarmingUpEntersLiveIfHealthyAtStart) {
    FakeLiveInput* fake = nullptr;
    auto clip = makeClip(fake);
    const std::uint64_t start = 100'000'000'000ULL;
    clip.setScheduledStart(start);
    clip.onTick(start - 4'000'000'000ULL);    // → WarmingUp
    fake->healthy_.store(true);
    fake->last_pkt_ns_.store(static_cast<std::int64_t>(start - 100'000'000));
    clip.onTick(start);                       // → Live
    EXPECT_EQ(clip.state(), LiveClip::State::Live);
}

TEST(LiveClipStateMachine, WarmingUpEntersLostIfUnhealthyAtStart) {
    FakeLiveInput* fake = nullptr;
    auto clip = makeClip(fake);
    const std::uint64_t start = 100'000'000'000ULL;
    clip.setScheduledStart(start);
    clip.onTick(start - 4'000'000'000ULL);
    // healthy_ stays false — RTMP handshake didn't finish in time
    clip.onTick(start);
    EXPECT_EQ(clip.state(), LiveClip::State::Lost);
}

// ── Live → Lost ────────────────────────────────────────────────────────

TEST(LiveClipStateMachine, LiveFlipsToLostOnPacketSilence) {
    FakeLiveInput* fake = nullptr;
    auto clip = makeClip(fake);
    const std::uint64_t start = 100'000'000'000ULL;
    clip.setScheduledStart(start);
    clip.onTick(start - 4'000'000'000ULL);
    fake->healthy_.store(true);
    fake->last_pkt_ns_.store(static_cast<std::int64_t>(start));
    clip.onTick(start);
    ASSERT_EQ(clip.state(), LiveClip::State::Live);

    // Pump stops; >2s without packets → Lost
    clip.onTick(start + 2'500'000'000ULL);
    EXPECT_EQ(clip.state(), LiveClip::State::Lost);
}

TEST(LiveClipStateMachine, LiveFlipsToLostOnUnhealthy) {
    FakeLiveInput* fake = nullptr;
    auto clip = makeClip(fake);
    const std::uint64_t start = 100'000'000'000ULL;
    clip.setScheduledStart(start);
    clip.onTick(start - 4'000'000'000ULL);
    fake->healthy_.store(true);
    fake->last_pkt_ns_.store(static_cast<std::int64_t>(start));
    clip.onTick(start);
    ASSERT_EQ(clip.state(), LiveClip::State::Live);

    // Packets are still recent, but the input flipped its own
    // healthy gauge (e.g. RTMP server closed the connection).
    fake->healthy_.store(false);
    clip.onTick(start + 100'000'000ULL);
    EXPECT_EQ(clip.state(), LiveClip::State::Lost);
}

TEST(LiveClipStateMachine, LossThresholdConfigurable) {
    FakeLiveInput* fake = nullptr;
    auto clip = makeClip(fake, 60'000'000'000ULL, 5'000'000'000ULL,
                         /*loss=*/500'000'000ULL);   // 500ms tighter
    const std::uint64_t start = 100'000'000'000ULL;
    clip.setScheduledStart(start);
    clip.onTick(start - 4'000'000'000ULL);
    fake->healthy_.store(true);
    fake->last_pkt_ns_.store(static_cast<std::int64_t>(start));
    clip.onTick(start);
    ASSERT_EQ(clip.state(), LiveClip::State::Live);

    // 600ms silence is past the tightened threshold
    clip.onTick(start + 600'000'000ULL);
    EXPECT_EQ(clip.state(), LiveClip::State::Lost);
}

// ── Lost → Live (recovery) ─────────────────────────────────────────────

TEST(LiveClipStateMachine, LostRecoversWhenHealthy) {
    FakeLiveInput* fake = nullptr;
    auto clip = makeClip(fake);
    const std::uint64_t start = 100'000'000'000ULL;
    clip.setScheduledStart(start);
    clip.onTick(start - 4'000'000'000ULL);

    // Drive WarmingUp → Lost (signal never arrived)
    clip.onTick(start);
    ASSERT_EQ(clip.state(), LiveClip::State::Lost);

    // Pump finally connected — fresh packets, healthy flag flips
    fake->healthy_.store(true);
    fake->last_pkt_ns_.store(static_cast<std::int64_t>(start + 1'000'000'000ULL));
    clip.onTick(start + 1'200'000'000ULL);
    EXPECT_EQ(clip.state(), LiveClip::State::Live);

    auto j = clip.statusJson();
    EXPECT_EQ(j["recovered_count"], 1u);
    EXPECT_GE(j["loss_count"].get<std::uint32_t>(), 1u);
}

// ── Finished ─────────────────────────────────────────────────────────────

TEST(LiveClipStateMachine, FinishesAtDurationBoundaryEvenWhenLive) {
    FakeLiveInput* fake = nullptr;
    auto clip = makeClip(fake, /*dur=*/10'000'000'000ULL);   // 10s
    const std::uint64_t start = 100'000'000'000ULL;
    clip.setScheduledStart(start);
    clip.onTick(start - 4'000'000'000ULL);
    fake->healthy_.store(true);
    fake->last_pkt_ns_.store(static_cast<std::int64_t>(start));
    clip.onTick(start);
    ASSERT_EQ(clip.state(), LiveClip::State::Live);

    fake->last_pkt_ns_.store(static_cast<std::int64_t>(start + 10'000'000'000ULL));
    clip.onTick(start + 10'000'000'000ULL);   // boundary
    EXPECT_EQ(clip.state(), LiveClip::State::Finished);
    EXPECT_EQ(fake->release_calls, 1);
    EXPECT_FALSE(clip.isPrepared());
    EXPECT_TRUE(clip.isFinished(start + 10'000'000'000ULL));
}

TEST(LiveClipStateMachine, FinishedIsTerminal) {
    FakeLiveInput* fake = nullptr;
    auto clip = makeClip(fake, 1'000'000'000ULL);
    const std::uint64_t start = 100'000'000'000ULL;
    clip.setScheduledStart(start);
    clip.onTick(start - 4'000'000'000ULL);
    fake->healthy_.store(true);
    fake->last_pkt_ns_.store(static_cast<std::int64_t>(start));
    clip.onTick(start);
    clip.onTick(start + 1'000'000'000ULL);   // → Finished
    ASSERT_EQ(clip.state(), LiveClip::State::Finished);

    // No matter how many ticks we throw at it, no more side effects
    fake->healthy_.store(true);
    clip.onTick(start + 5'000'000'000ULL);
    clip.onTick(start + 50'000'000'000ULL);
    EXPECT_EQ(clip.state(), LiveClip::State::Finished);
    EXPECT_EQ(fake->release_calls, 1);
}

// ── Frame routing ───────────────────────────────────────────────────────

TEST(LiveClipFrameRouting, GetFrameOnlyForwardsInLive) {
    FakeLiveInput* fake = nullptr;
    auto clip = makeClip(fake);
    const std::uint64_t start = 100'000'000'000ULL;
    clip.setScheduledStart(start);

    // Idle: no forwarding
    auto f = clip.getFrame();
    EXPECT_FALSE(f.valid());
    EXPECT_EQ(fake->frame_calls, 0);

    // WarmingUp: still no forwarding
    clip.onTick(start - 4'000'000'000ULL);
    f = clip.getFrame();
    EXPECT_EQ(fake->frame_calls, 0);

    // Live: forwarded
    fake->healthy_.store(true);
    fake->last_pkt_ns_.store(static_cast<std::int64_t>(start));
    clip.onTick(start);
    ASSERT_EQ(clip.state(), LiveClip::State::Live);
    (void)clip.getFrame();
    EXPECT_EQ(fake->frame_calls, 1);

    // Lost: stops forwarding
    fake->healthy_.store(false);
    clip.onTick(start + 100'000'000ULL);
    ASSERT_EQ(clip.state(), LiveClip::State::Lost);
    (void)clip.getFrame();
    EXPECT_EQ(fake->frame_calls, 1);
}

// ── statusJson shape ────────────────────────────────────────────────────

TEST(LiveClipStatus, JsonHasExpectedFields) {
    FakeLiveInput* fake = nullptr;
    auto clip = makeClip(fake);
    auto j = clip.statusJson();
    EXPECT_EQ(j["id"], "studio");
    EXPECT_EQ(j["state"], "Idle");
    EXPECT_EQ(j["loss_count"], 0u);
    EXPECT_EQ(j["recovered_count"], 0u);
    EXPECT_TRUE(j.contains("input"));
    EXPECT_EQ(j["input"]["fake"], true);
    EXPECT_EQ(j["duration_ns"], 60'000'000'000ULL);
    EXPECT_EQ(j["warm_up_ns"],   5'000'000'000ULL);
}

// ── OnLossProvider plumbing (c6) ────────────────────────────────────────

TEST(LiveClipOnLoss, ProviderUsedOutsideLive) {
    FakeLiveInput* fake = nullptr;
    auto clip = makeClip(fake);

    int video_calls = 0;
    int audio_calls = 0;
    LiveClip::OnLossProvider p;
    p.video = [&]() {
        ++video_calls;
        Frame f;
        f.width = 4; f.height = 4;
        f.data = std::shared_ptr<std::uint8_t[]>(new std::uint8_t[64]());
        return f;
    };
    p.audio = [&](int n) {
        ++audio_calls;
        AudioFrame af;
        af.num_samples = n;
        af.samples.assign(n * 2, 0.0f);
        af.valid = true;
        return af;
    };
    clip.setOnLossProvider(std::move(p));

    // Idle → provider invoked
    auto f = clip.getFrame();
    EXPECT_TRUE(f.valid());
    EXPECT_EQ(video_calls, 1);
    EXPECT_EQ(fake->frame_calls, 0);

    auto af = clip.getAudio(512);
    EXPECT_TRUE(af.valid);
    EXPECT_EQ(audio_calls, 1);
}

TEST(LiveClipOnLoss, ProviderBypassedInLive) {
    FakeLiveInput* fake = nullptr;
    auto clip = makeClip(fake);

    int video_calls = 0;
    LiveClip::OnLossProvider p;
    p.video = [&]() { ++video_calls; return Frame{}; };
    clip.setOnLossProvider(std::move(p));

    const std::uint64_t start = 100'000'000'000ULL;
    clip.setScheduledStart(start);
    clip.onTick(start - 4'000'000'000ULL);
    fake->healthy_.store(true);
    fake->last_pkt_ns_.store(static_cast<std::int64_t>(start));
    clip.onTick(start);
    ASSERT_EQ(clip.state(), LiveClip::State::Live);

    (void)clip.getFrame();
    EXPECT_EQ(video_calls, 0);     // provider not consulted in Live
    EXPECT_EQ(fake->frame_calls, 1);
}

TEST(LiveClipOnLoss, NullProviderFallsBackToInvalidFrame) {
    FakeLiveInput* fake = nullptr;
    auto clip = makeClip(fake);
    // No provider installed — non-Live getFrame() must return invalid Frame
    auto f = clip.getFrame();
    EXPECT_FALSE(f.valid());
    EXPECT_EQ(fake->frame_calls, 0);
}

TEST(LiveClipOnLoss, VideoAndAudioCanBeWiredIndependently) {
    FakeLiveInput* fake = nullptr;
    auto clip = makeClip(fake);

    int video_calls = 0;
    LiveClip::OnLossProvider p;
    p.video = [&]() { ++video_calls; return Frame{}; };
    // p.audio left null on purpose
    clip.setOnLossProvider(std::move(p));

    (void)clip.getFrame();
    EXPECT_EQ(video_calls, 1);

    // Audio falls back to input_->getTailAudio() — silence by default.
    auto af = clip.getAudio(256);
    // FakeLiveInput::getTailAudio returns whatever AudioFrame{} default is;
    // the contract is "no crash, no provider invocation".
    SUCCEED();
}

TEST(LiveClipOnLoss, ProviderReplacedAtRuntime) {
    FakeLiveInput* fake = nullptr;
    auto clip = makeClip(fake);

    int a_calls = 0, b_calls = 0;
    LiveClip::OnLossProvider pa;
    pa.video = [&]() { ++a_calls; return Frame{}; };
    clip.setOnLossProvider(std::move(pa));
    (void)clip.getFrame();
    EXPECT_EQ(a_calls, 1);

    LiveClip::OnLossProvider pb;
    pb.video = [&]() { ++b_calls; return Frame{}; };
    clip.setOnLossProvider(std::move(pb));
    (void)clip.getFrame();
    EXPECT_EQ(a_calls, 1);
    EXPECT_EQ(b_calls, 1);
}

// ── Tail dispatch through provider (c10) ────────────────────────────────
// During a transition Timeline calls getTailFrame on whichever side is
// flagged in_tail (FreezeFade ⇒ both, LiveMix ⇒ only the outgoing). For
// a live clip not yet in Live (entering on a File→Live transition) or
// already gone Lost we want the OnLossProvider to back the tail too —
// otherwise the crossfade would blend a stale/empty input frame.

TEST(LiveClipTail, GetTailFrameUsesProviderOutsideLive) {
    FakeLiveInput* fake = nullptr;
    auto clip = makeClip(fake);

    int provider_calls = 0;
    LiveClip::OnLossProvider p;
    p.video = [&]() { ++provider_calls; return Frame{}; };
    clip.setOnLossProvider(std::move(p));

    // Idle (no scheduled_start): tail should still consult provider.
    (void)clip.getTailFrame();
    EXPECT_EQ(provider_calls, 1);

    // WarmingUp: still provider, not input.
    const std::uint64_t start = 100'000'000'000ULL;
    clip.setScheduledStart(start);
    clip.onTick(start - 4'000'000'000ULL);
    ASSERT_EQ(clip.state(), LiveClip::State::WarmingUp);
    (void)clip.getTailFrame();
    EXPECT_EQ(provider_calls, 2);
}

TEST(LiveClipTail, GetTailFrameForwardsToInputInLive) {
    FakeLiveInput* fake = nullptr;
    auto clip = makeClip(fake);

    int provider_calls = 0;
    LiveClip::OnLossProvider p;
    p.video = [&]() { ++provider_calls; return Frame{}; };
    clip.setOnLossProvider(std::move(p));

    // Drive into Live state.
    const std::uint64_t start = 100'000'000'000ULL;
    clip.setScheduledStart(start);
    fake->healthy_.store(true);
    fake->last_pkt_ns_.store(static_cast<std::int64_t>(start));
    clip.onTick(start - 4'000'000'000ULL);
    clip.onTick(start);
    ASSERT_EQ(clip.state(), LiveClip::State::Live);

    (void)clip.getTailFrame();
    EXPECT_EQ(provider_calls, 0);  // input took over
}

TEST(LiveClipTail, GetTailFrameUsesProviderInLost) {
    FakeLiveInput* fake = nullptr;
    auto clip = makeClip(fake);

    int provider_calls = 0;
    LiveClip::OnLossProvider p;
    p.video = [&]() { ++provider_calls; return Frame{}; };
    clip.setOnLossProvider(std::move(p));

    const std::uint64_t start = 100'000'000'000ULL;
    clip.setScheduledStart(start);
    fake->healthy_.store(true);
    fake->last_pkt_ns_.store(static_cast<std::int64_t>(start));
    clip.onTick(start - 4'000'000'000ULL);
    clip.onTick(start);
    ASSERT_EQ(clip.state(), LiveClip::State::Live);

    // Drop the source.
    fake->healthy_.store(false);
    clip.onTick(start + 100'000'000ULL);
    ASSERT_EQ(clip.state(), LiveClip::State::Lost);

    (void)clip.getTailFrame();
    EXPECT_EQ(provider_calls, 1);
}

TEST(LiveClipTail, GetTailAudioUsesProviderOutsideLive) {
    FakeLiveInput* fake = nullptr;
    auto clip = makeClip(fake);

    int audio_calls = 0;
    LiveClip::OnLossProvider p;
    p.audio = [&](int n) {
        ++audio_calls;
        AudioFrame af;
        af.num_samples = n;
        af.valid       = true;
        return af;
    };
    clip.setOnLossProvider(std::move(p));

    const auto af = clip.getTailAudio(960);
    EXPECT_EQ(audio_calls, 1);
    EXPECT_EQ(af.num_samples, 960);
}

} // namespace
