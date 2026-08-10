// Step 8: Broadcast-model end-to-end test.
//
// Walks Timeline::getState() across full playlist cycles for each transition
// mode and asserts that:
//   • slot lengths and total timeline match the broadcast slot arithmetic;
//   • clipA_in_tail/clipB_in_tail flags are set exactly during transitions;
//   • for LiveMix, clipB advances live (getFrame counted on the consumer side)
//     during the prev tail — i.e. content is NOT replayed.
//
// We mock IClip so we can count how many times each accessor is called and
// distinguish content-mode from tail-mode dispatch — this is what RenderLoop
// would do in production via the clipA_in_tail / clipB_in_tail flags.

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <vector>

#include "core/Timeline.h"
#include "transitions/ITransition.h"

namespace {

class MockClip : public IClip {
public:
    explicit MockClip(double dur) : dur_(dur) {}

    Frame getFrame() override {
        ++content_frames;
        Frame f;
        f.width = 1; f.height = 1;
        f.data = std::shared_ptr<uint8_t[]>(new uint8_t[4]{0,0,0,255});
        return f;
    }
    AudioFrame getAudio(int n) override {
        AudioFrame a; a.num_samples = n; a.valid = true;
        a.samples.assign(n * 2, 0.0f);
        return a;
    }
    Frame getTailFrame() override {
        ++tail_frames;
        Frame f; f.width = 1; f.height = 1;
        f.data = std::shared_ptr<uint8_t[]>(new uint8_t[4]{0,0,0,255});
        return f;
    }
    AudioFrame getTailAudio(int n) override {
        AudioFrame a; a.num_samples = n; a.valid = true;
        a.samples.assign(n * 2, 0.0f);
        return a;
    }
    double getDuration() const override { return dur_; }
    bool   hasAudio()    const override { return false; }
    bool   isPrepared()  const override { return prepared_; }
    void   prepare() override { prepared_ = true; }
    void   release() override { prepared_ = false; }
    void   reset()   override { ++resets; }

    int content_frames = 0;
    int tail_frames    = 0;
    int resets         = 0;
private:
    double dur_;
    bool   prepared_ = true;
};

TransitionConfig hardCut()                  { return {TransitionType::HardCut,   TransitionMode::HardCut,    0.0}; }
TransitionConfig freezeFade(double sec=2.0) { return {TransitionType::CrossFade, TransitionMode::FreezeFade, sec}; }
TransitionConfig liveMix  (double sec=2.0)  { return {TransitionType::CrossFade, TransitionMode::LiveMix,    sec}; }

// Drives Timeline at 25 fps for `total_sec` seconds. Mirrors the dispatch
// RenderLoop would perform: each frame either calls getFrame()/getTailFrame()
// on clipA, plus optionally on clipB during in_transition windows.
struct Stats {
    int frames_total          = 0;
    int frames_in_transition  = 0;
    int frames_clipB_live     = 0;  // in_transition && !clipB_in_tail
    int frames_clipA_in_tail  = 0;
};

Stats walk(Timeline& tl, double total_sec, double fps = 25.0) {
    Stats s;
    const int    n_frames = static_cast<int>(total_sec * fps);
    const double dt       = 1.0 / fps;
    for (int i = 0; i < n_frames; ++i) {
        const double t = i * dt;
        auto state = tl.getState(t);
        if (!state.clipA) continue;
        ++s.frames_total;

        if (state.clipA_in_tail) {
            static_cast<MockClip*>(state.clipA)->getTailFrame();
            ++s.frames_clipA_in_tail;
        } else {
            static_cast<MockClip*>(state.clipA)->getFrame();
        }

        if (state.in_transition && state.clipB) {
            ++s.frames_in_transition;
            if (state.clipB_in_tail) {
                static_cast<MockClip*>(state.clipB)->getTailFrame();
            } else {
                static_cast<MockClip*>(state.clipB)->getFrame();
                ++s.frames_clipB_live;
            }
        }
    }
    return s;
}

// Build a 3-clip playlist with raw pointer access for assertions.
struct Built {
    Timeline tl;
    MockClip *a, *b, *c;
};
Built makePlaylist(double dur, std::vector<TransitionConfig> trs) {
    Built built;
    auto a = std::make_unique<MockClip>(dur);
    auto b = std::make_unique<MockClip>(dur);
    auto c = std::make_unique<MockClip>(dur);
    built.a = a.get(); built.b = b.get(); built.c = c.get();
    std::vector<std::unique_ptr<IClip>> v;
    v.push_back(std::move(a));
    v.push_back(std::move(b));
    v.push_back(std::move(c));
    built.tl.setPlaylist(std::move(v), std::move(trs));
    return built;
}

} // namespace

// ─── Scenario A: all FreezeFade ──────────────────────────────────────────────
// 3×5s clips + 3×2s transitions = 21s total. Each clip's content phase = 5s
// (FreezeFade does not consume content from prev). During the 2s tail of every
// slot, BOTH clipA_in_tail and clipB_in_tail must be true.
TEST(BroadcastPlaybackTest, AllFreezeFade) {
    auto p = makePlaylist(5.0, {freezeFade(), freezeFade(), freezeFade()});
    auto& tl = p.tl;

    // Probe slot arithmetic.
    auto s_mid_content_a = tl.getState(2.5);   // inside slot[0] content phase
    EXPECT_FALSE(s_mid_content_a.in_transition);
    EXPECT_EQ(s_mid_content_a.clipA, p.a);
    EXPECT_FALSE(s_mid_content_a.clipA_in_tail);

    auto s_in_tail_a = tl.getState(6.0);       // inside slot[0] tail (5..7)
    EXPECT_TRUE(s_in_tail_a.in_transition);
    EXPECT_TRUE(s_in_tail_a.clipA_in_tail);
    EXPECT_TRUE(s_in_tail_a.clipB_in_tail);   // FreezeFade ⇒ both frozen
    EXPECT_EQ(s_in_tail_a.clipA, p.a);
    EXPECT_EQ(s_in_tail_a.clipB, p.b);

    auto s_mid_content_b = tl.getState(9.0);   // inside slot[1] content (7..12)
    EXPECT_FALSE(s_mid_content_b.in_transition);
    EXPECT_EQ(s_mid_content_b.clipA, p.b);

    auto stats = walk(tl, 21.0);
    // Total = 21s × 25 fps = 525 frames; within rounding.
    EXPECT_NEAR(stats.frames_total, 525, 2);
    // Transition window per slot: 2s × 25 = 50 frames × 3 slots = 150.
    EXPECT_NEAR(stats.frames_in_transition, 150, 3);
    // FreezeFade: clipB never live during transition.
    EXPECT_EQ(stats.frames_clipB_live, 0);
}

// ─── Scenario B: all HardCut ─────────────────────────────────────────────────
// total = 15s, no transition windows.
TEST(BroadcastPlaybackTest, AllHardCut) {
    auto p = makePlaylist(5.0, {hardCut(), hardCut(), hardCut()});
    auto& tl = p.tl;

    EXPECT_FALSE(tl.getState(2.5).in_transition);
    EXPECT_FALSE(tl.getState(4.99).in_transition);
    EXPECT_FALSE(tl.getState(5.01).in_transition);

    auto stats = walk(tl, 15.0);
    EXPECT_EQ(stats.frames_in_transition, 0);
    EXPECT_EQ(stats.frames_clipA_in_tail, 0);
    EXPECT_NEAR(stats.frames_total, 375, 2);
}

// ─── Scenario C: all LiveMix ─────────────────────────────────────────────────
// total = sum(durations) = 15s. During transition, clipB plays live (getFrame
// called, NOT getTailFrame). prev_consumed = 2s, so each slot is
// (5-2)+2 = 5s. clipB content advances linearly across slot boundary.
TEST(BroadcastPlaybackTest, AllLiveMix) {
    auto p = makePlaylist(5.0, {liveMix(), liveMix(), liveMix()});
    auto& tl = p.tl;

    auto s_in_tail = tl.getState(4.0);    // slot[0] is 0..5; tail is 3..5
    EXPECT_TRUE(s_in_tail.in_transition);
    EXPECT_TRUE(s_in_tail.clipA_in_tail);
    EXPECT_FALSE(s_in_tail.clipB_in_tail);   // LiveMix ⇒ clipB live
    EXPECT_EQ(s_in_tail.clipA, p.a);
    EXPECT_EQ(s_in_tail.clipB, p.b);

    auto stats = walk(tl, 15.0);
    EXPECT_NEAR(stats.frames_total, 375, 2);
    // Transition window: 2s × 25 × 3 slots = 150 frames.
    EXPECT_NEAR(stats.frames_in_transition, 150, 3);
    // LiveMix: clipB always live during every transition.
    EXPECT_EQ(stats.frames_clipB_live, stats.frames_in_transition);
    // clipA in tail during every transition window.
    EXPECT_EQ(stats.frames_clipA_in_tail, stats.frames_in_transition);
}

// ─── Scenario D: mixed modes ────────────────────────────────────────────────
// transitions = [HardCut(0), FreezeFade(2), LiveMix(2)].
// slot[0]: prev = LiveMix ⇒ prev_consumed = 2; own = HardCut ⇒ tail = 0.
//          slot.length = (5 - 2) + 0 = 3.
// slot[1]: prev = HardCut ⇒ prev_consumed = 0; own = FreezeFade ⇒ tail = 2.
//          slot.length = 5 + 2 = 7.
// slot[2]: prev = FreezeFade ⇒ prev_consumed = 0; own = LiveMix ⇒ tail = 2.
//          slot.length = 5 + 2 = 7.
// total = 3 + 7 + 7 = 17.
TEST(BroadcastPlaybackTest, MixedModes) {
    auto p = makePlaylist(5.0, {hardCut(), freezeFade(), liveMix()});
    auto& tl = p.tl;

    // slot[0] entirely content-only (no own tail).
    auto s_a = tl.getState(2.0);
    EXPECT_FALSE(s_a.in_transition);
    EXPECT_EQ(s_a.clipA, p.a);

    // slot[1] tail (FreezeFade): both frozen.
    auto s_ab_tail = tl.getState(3 + 5 + 1);  // = 9.0, inside slot[1] tail (8..10)
    EXPECT_TRUE(s_ab_tail.in_transition);
    EXPECT_TRUE(s_ab_tail.clipA_in_tail);
    EXPECT_TRUE(s_ab_tail.clipB_in_tail);
    EXPECT_EQ(s_ab_tail.clipA, p.b);
    EXPECT_EQ(s_ab_tail.clipB, p.c);

    // slot[2] tail (LiveMix): clipA frozen, clipB live (wraps to clip[0]).
    auto s_bc_tail = tl.getState(3 + 7 + 6);  // = 16.0, inside slot[2] tail (15..17)
    EXPECT_TRUE(s_bc_tail.in_transition);
    EXPECT_TRUE(s_bc_tail.clipA_in_tail);
    EXPECT_FALSE(s_bc_tail.clipB_in_tail);
    EXPECT_EQ(s_bc_tail.clipA, p.c);
    EXPECT_EQ(s_bc_tail.clipB, p.a);

    // Walk full cycle.
    auto stats = walk(tl, 17.0);
    EXPECT_NEAR(stats.frames_total, 425, 2);
    // Transition windows: slot[1]=50, slot[2]=50, slot[0]=0 → 100 total.
    EXPECT_NEAR(stats.frames_in_transition, 100, 3);
    // Only the LiveMix transition (slot[2]) drives clipB live → ~50 frames.
    EXPECT_NEAR(stats.frames_clipB_live, 50, 3);
}
