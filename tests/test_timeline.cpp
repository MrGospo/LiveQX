#include <gtest/gtest.h>
#include "core/Timeline.h"

// ─── Mock clip ────────────────────────────────────────────────────────────────

class MockClip : public IClip {
public:
    explicit MockClip(double dur) : dur_(dur) {}
    Frame      getFrame()            override { return {}; }
    AudioFrame getAudio(int)         override { return {}; }
    double     getDuration()   const override { return dur_; }
    bool       hasAudio()      const override { return false; }
    bool       isPrepared()    const override { return true; }
    void       prepare()             override {}
    void       release()             override {}
    Frame      getTailFrame()        override { return {}; }
    AudioFrame getTailAudio(int)     override { return {}; }
    void       reset()               override {}
private:
    double dur_;
};

// ─── helpers ─────────────────────────────────────────────────────────────────

static TransitionConfig freezeFade(double dur = 2.0) {
    return { TransitionType::CrossFade, TransitionMode::FreezeFade, dur };
}
static TransitionConfig liveMix(double dur = 2.0) {
    return { TransitionType::CrossFade, TransitionMode::LiveMix, dur };
}
static TransitionConfig hardCut() {
    return { TransitionType::HardCut, TransitionMode::HardCut, 0.0 };
}

// ─── basic tests ─────────────────────────────────────────────────────────────

TEST(TimelineTest, EmptyPlaylistReturnsFallback) {
    Timeline t;
    auto s = t.getState(0.0);
    EXPECT_EQ(s.clipA, nullptr);
    EXPECT_FALSE(s.in_transition);
}

TEST(TimelineTest, EmptyPlaylistWithFallback) {
    auto fb = std::make_unique<MockClip>(0.0);
    const IClip* raw = fb.get();

    Timeline t;
    t.setFallback(std::move(fb));

    auto s = t.getState(0.0);
    EXPECT_EQ(s.clipA, raw);
    EXPECT_FALSE(s.in_transition);
}

// ─── FreezeFade fixture ──────────────────────────────────────────────────────
// Clips: [10s, 30s, 5s], all transitions FreezeFade 2s.
// FreezeFade: prev_consumed=0, content_phase=duration, tail=2s.
//   slot[0] = 12s : content [0, 10),   tail [10, 12)
//   slot[1] = 32s : content [12, 42),  tail [42, 44)
//   slot[2] =  7s : content [44, 49),  tail [49, 51)
//   total = 51s

class TimelineFreezeFadeTest : public ::testing::Test {
protected:
    static constexpr double kD0 = 10.0;
    static constexpr double kD1 = 30.0;
    static constexpr double kD2 =  5.0;

    const IClip* clip0_ = nullptr;
    const IClip* clip1_ = nullptr;
    const IClip* clip2_ = nullptr;
    Timeline tl_;

    void SetUp() override {
        auto c0 = std::make_unique<MockClip>(kD0);
        auto c1 = std::make_unique<MockClip>(kD1);
        auto c2 = std::make_unique<MockClip>(kD2);
        clip0_ = c0.get();
        clip1_ = c1.get();
        clip2_ = c2.get();

        std::vector<std::unique_ptr<IClip>> clips;
        clips.push_back(std::move(c0));
        clips.push_back(std::move(c1));
        clips.push_back(std::move(c2));

        tl_.setPlaylist(std::move(clips),
                        { freezeFade(), freezeFade(), freezeFade() });
    }
};

TEST_F(TimelineFreezeFadeTest, ContentPhaseClip0) {
    auto s = tl_.getState(0.0);
    EXPECT_EQ(s.clipA, clip0_);
    EXPECT_EQ(s.clipB, nullptr);
    EXPECT_FALSE(s.in_transition);
    EXPECT_FALSE(s.clipA_in_tail);
    EXPECT_FALSE(s.clipB_in_tail);
}

TEST_F(TimelineFreezeFadeTest, TailMidpointClip0) {
    auto s = tl_.getState(11.0);
    EXPECT_EQ(s.clipA, clip0_);
    EXPECT_EQ(s.clipB, clip1_);
    EXPECT_TRUE(s.in_transition);
    EXPECT_TRUE(s.clipA_in_tail);
    EXPECT_TRUE(s.clipB_in_tail);   // FreezeFade ⇒ clipB frozen too
    EXPECT_FLOAT_EQ(s.transition_progress, 0.5f);
}

TEST_F(TimelineFreezeFadeTest, ContentPhaseClip1Boundary) {
    // t=12 — клип 1 content начинается, нет transition.
    auto s = tl_.getState(12.0);
    EXPECT_EQ(s.clipA, clip1_);
    EXPECT_FALSE(s.in_transition);
}

TEST_F(TimelineFreezeFadeTest, TailMidpointClip1) {
    auto s = tl_.getState(43.0);
    EXPECT_EQ(s.clipA, clip1_);
    EXPECT_EQ(s.clipB, clip2_);
    EXPECT_TRUE(s.clipA_in_tail);
    EXPECT_TRUE(s.clipB_in_tail);
    EXPECT_FLOAT_EQ(s.transition_progress, 0.5f);
}

TEST_F(TimelineFreezeFadeTest, LoopWrapsAtTotal51) {
    auto s = tl_.getState(51.0);
    EXPECT_EQ(s.clipA, clip0_);
    EXPECT_FALSE(s.in_transition);
}

TEST_F(TimelineFreezeFadeTest, LastClipTailLoopsToFirst) {
    auto s = tl_.getState(50.0);
    EXPECT_EQ(s.clipA, clip2_);
    EXPECT_EQ(s.clipB, clip0_);
    EXPECT_TRUE(s.clipA_in_tail);
    EXPECT_TRUE(s.clipB_in_tail);
    EXPECT_FLOAT_EQ(s.transition_progress, 0.5f);
}

// ─── LiveMix fixture ─────────────────────────────────────────────────────────
// LiveMix: prev_consumed = prev.duration_sec, content_phase = duration - prev,
//          tail = own.duration_sec. Total = sum(durations) (transitions cancel).
//   transitions all LiveMix 2s.
//   slot[0]: prev = transitions[2] = LiveMix 2s ⇒ content_phase = 10 - 2 = 8.
//            tail = 2s. slot.length = 10.
//            t ∈ [0, 8): content; [8, 10): tail (clipB = clip[1] LIVE, not frozen)
//   slot[1]: prev = transitions[0] = LiveMix 2s ⇒ content_phase = 30 - 2 = 28.
//            tail = 2s. slot.length = 30.
//            t ∈ [10, 38): content; [38, 40): tail
//   slot[2]: prev = transitions[1] = LiveMix 2s ⇒ content_phase = 5 - 2 = 3.
//            tail = 2s. slot.length = 5.
//            t ∈ [40, 43): content; [43, 45): tail (clipB = clip[0] LIVE)
//   total = 45 = sum(durations).

class TimelineLiveMixTest : public ::testing::Test {
protected:
    const IClip* clip0_ = nullptr;
    const IClip* clip1_ = nullptr;
    const IClip* clip2_ = nullptr;
    Timeline tl_;

    void SetUp() override {
        auto c0 = std::make_unique<MockClip>(10.0);
        auto c1 = std::make_unique<MockClip>(30.0);
        auto c2 = std::make_unique<MockClip>( 5.0);
        clip0_ = c0.get();
        clip1_ = c1.get();
        clip2_ = c2.get();

        std::vector<std::unique_ptr<IClip>> clips;
        clips.push_back(std::move(c0));
        clips.push_back(std::move(c1));
        clips.push_back(std::move(c2));

        tl_.setPlaylist(std::move(clips),
                        { liveMix(), liveMix(), liveMix() });
    }
};

TEST_F(TimelineLiveMixTest, TotalEqualsSumDurations) {
    // Probing at t=45 — должен начать новый цикл с clip[0] content.
    auto s = tl_.getState(45.0);
    EXPECT_EQ(s.clipA, clip0_);
    EXPECT_FALSE(s.in_transition);
}

TEST_F(TimelineLiveMixTest, ContentPhaseClip0) {
    auto s = tl_.getState(4.0);
    EXPECT_EQ(s.clipA, clip0_);
    EXPECT_FALSE(s.in_transition);
}

TEST_F(TimelineLiveMixTest, TailWithLiveB) {
    // t=9 in slot[0].tail [8,10), middle.
    auto s = tl_.getState(9.0);
    EXPECT_EQ(s.clipA, clip0_);
    EXPECT_EQ(s.clipB, clip1_);
    EXPECT_TRUE(s.in_transition);
    EXPECT_TRUE(s.clipA_in_tail);
    EXPECT_FALSE(s.clipB_in_tail);   // LiveMix ⇒ B живой
    EXPECT_FLOAT_EQ(s.transition_progress, 0.5f);
}

TEST_F(TimelineLiveMixTest, NextSlotStartsRightAfterTail) {
    // t=10 — start of slot[1], no transition.
    auto s = tl_.getState(10.0);
    EXPECT_EQ(s.clipA, clip1_);
    EXPECT_FALSE(s.in_transition);
}

// ─── HardCut fixture ─────────────────────────────────────────────────────────
// transitions all HardCut. tail = 0 ⇒ нет окна перехода. slot = duration.
// total = sum(durations).

TEST(TimelineHardCutTest, NoTransitionWindow) {
    auto c0 = std::make_unique<MockClip>(5.0);
    auto c1 = std::make_unique<MockClip>(7.0);
    const IClip* raw0 = c0.get();
    const IClip* raw1 = c1.get();

    std::vector<std::unique_ptr<IClip>> clips;
    clips.push_back(std::move(c0));
    clips.push_back(std::move(c1));

    Timeline tl;
    tl.setPlaylist(std::move(clips), { hardCut(), hardCut() });

    EXPECT_EQ(tl.getState(4.99).clipA, raw0);
    EXPECT_FALSE(tl.getState(4.99).in_transition);

    EXPECT_EQ(tl.getState(5.00).clipA, raw1);
    EXPECT_FALSE(tl.getState(5.00).in_transition);

    // wrap at 5+7 = 12
    EXPECT_EQ(tl.getState(12.0).clipA, raw0);
}

TEST(TimelineHardCutTest, EmptyTransitionsVectorTreatedAsHardCut) {
    auto c0 = std::make_unique<MockClip>(5.0);
    auto c1 = std::make_unique<MockClip>(5.0);
    const IClip* raw1 = c1.get();

    std::vector<std::unique_ptr<IClip>> clips;
    clips.push_back(std::move(c0));
    clips.push_back(std::move(c1));

    Timeline tl;
    tl.setPlaylist(std::move(clips), {});

    auto s = tl.getState(4.9);
    EXPECT_FALSE(s.in_transition);

    auto s2 = tl.getState(5.0);
    EXPECT_EQ(s2.clipA, raw1);
    EXPECT_FALSE(s2.in_transition);
}

// ─── Mixed mode: HardCut + FreezeFade + LiveMix ──────────────────────────────
// Clips [5, 10, 8], transitions[0]=HardCut, transitions[1]=FreezeFade(2),
// transitions[2]=LiveMix(2).
// slot[0]: prev = transitions[2] LiveMix 2s ⇒ content = 5-2 = 3.
//          own = HardCut ⇒ tail = 0. slot.length = 3.
//          t ∈ [0, 3) content
// slot[1]: prev = transitions[0] HardCut ⇒ content = 10. tail = 2. slot = 12.
//          t ∈ [3, 13) content; [13, 15) tail (clipB frozen, FreezeFade)
// slot[2]: prev = transitions[1] FreezeFade ⇒ content = 8. tail = 2. slot = 10.
//          t ∈ [15, 23) content; [23, 25) tail (clipB live, LiveMix to clip 0)
// total = 25
TEST(TimelineMixedTest, AllThreeModesInOnePlaylist) {
    auto c0 = std::make_unique<MockClip>(5.0);
    auto c1 = std::make_unique<MockClip>(10.0);
    auto c2 = std::make_unique<MockClip>(8.0);
    const IClip* r0 = c0.get();
    const IClip* r1 = c1.get();
    const IClip* r2 = c2.get();

    std::vector<std::unique_ptr<IClip>> clips;
    clips.push_back(std::move(c0));
    clips.push_back(std::move(c1));
    clips.push_back(std::move(c2));

    Timeline tl;
    tl.setPlaylist(std::move(clips), { hardCut(), freezeFade(2.0), liveMix(2.0) });

    // slot[0] content: clipA = clip0
    EXPECT_EQ(tl.getState(1.0).clipA, r0);
    EXPECT_FALSE(tl.getState(1.0).in_transition);

    // boundary: t=3 ⇒ slot[1] content starts (HardCut, no tail)
    EXPECT_EQ(tl.getState(3.0).clipA, r1);
    EXPECT_FALSE(tl.getState(3.0).in_transition);

    // slot[1] tail mid: t=14 ⇒ FreezeFade, clipB frozen
    {
        auto s = tl.getState(14.0);
        EXPECT_EQ(s.clipA, r1);
        EXPECT_EQ(s.clipB, r2);
        EXPECT_TRUE(s.in_transition);
        EXPECT_TRUE(s.clipA_in_tail);
        EXPECT_TRUE(s.clipB_in_tail);
    }

    // slot[2] tail mid: t=24 ⇒ LiveMix, clipB live
    {
        auto s = tl.getState(24.0);
        EXPECT_EQ(s.clipA, r2);
        EXPECT_EQ(s.clipB, r0);
        EXPECT_TRUE(s.in_transition);
        EXPECT_TRUE(s.clipA_in_tail);
        EXPECT_FALSE(s.clipB_in_tail);
    }

    // wrap at 25
    EXPECT_EQ(tl.getState(25.0).clipA, r0);
}

// ─── Boundary status (fix8 step 8) ───────────────────────────────────────────

TEST(TimelineBoundaryStatus, DefaultIsCompleted) {
    Timeline tl;
    EXPECT_EQ(tl.consumeBoundaryStatus(), Timeline::BoundaryStatus::Completed);
}

TEST(TimelineBoundaryStatus, ConsumeIsExchange) {
    Timeline tl;
    std::vector<std::unique_ptr<IClip>> clips;
    clips.emplace_back(std::make_unique<MockClip>(10.0));
    clips.emplace_back(std::make_unique<MockClip>(10.0));
    std::vector<TransitionConfig> trs(2, hardCut());
    tl.setPlaylist(std::move(clips), std::move(trs));

    tl.skipToNext();
    EXPECT_EQ(tl.consumeBoundaryStatus(), Timeline::BoundaryStatus::SkippedUser);
    // Second consume returns to default — pending is reset on every read.
    EXPECT_EQ(tl.consumeBoundaryStatus(), Timeline::BoundaryStatus::Completed);
}

TEST(TimelineBoundaryStatus, SkipToNextOnEmptyDoesNotMark) {
    Timeline tl;
    tl.skipToNext();
    EXPECT_EQ(tl.consumeBoundaryStatus(), Timeline::BoundaryStatus::Completed);
}

TEST(TimelineBoundaryStatus, ReapPendingActiveSetsRemoved) {
    Timeline tl;
    std::vector<std::unique_ptr<IClip>> clips;
    clips.emplace_back(std::make_unique<MockClip>(10.0));
    clips.emplace_back(std::make_unique<MockClip>(10.0));
    std::vector<TransitionConfig> trs(2, hardCut());
    std::vector<std::string> paths{"a", "b"};
    tl.setPlaylist(std::move(clips), std::move(trs), std::move(paths));

    EXPECT_TRUE(tl.markForRemoval("a"));
    EXPECT_TRUE(tl.reapPendingActive());
    EXPECT_EQ(tl.consumeBoundaryStatus(), Timeline::BoundaryStatus::Removed);
}

TEST(TimelineBoundaryStatus, NameMapping) {
    EXPECT_STREQ(Timeline::boundaryStatusName(Timeline::BoundaryStatus::Completed),   "completed");
    EXPECT_STREQ(Timeline::boundaryStatusName(Timeline::BoundaryStatus::SkippedUser), "skipped_user");
    EXPECT_STREQ(Timeline::boundaryStatusName(Timeline::BoundaryStatus::Removed),     "removed");
}

// ─── restoreCursor (fix17) ───────────────────────────────────────────────────

TEST(TimelineRestoreCursor, EmptyPlaylistIsNoop) {
    Timeline tl;
    EXPECT_FALSE(tl.restoreCursor(0, 1.0));
}

TEST(TimelineRestoreCursor, OutOfRangeIdxIsNoop) {
    Timeline tl;
    std::vector<std::unique_ptr<IClip>> clips;
    clips.emplace_back(std::make_unique<MockClip>(10.0));
    clips.emplace_back(std::make_unique<MockClip>(10.0));
    tl.setPlaylist(std::move(clips), std::vector<TransitionConfig>(2, hardCut()));

    EXPECT_FALSE(tl.restoreCursor(-1, 0.0));
    EXPECT_FALSE(tl.restoreCursor(2, 0.0));
}

TEST(TimelineRestoreCursor, AnchorsToIdxAndSlotPos) {
    Timeline tl;
    std::vector<std::unique_ptr<IClip>> clips;
    clips.emplace_back(std::make_unique<MockClip>(10.0));
    clips.emplace_back(std::make_unique<MockClip>(20.0));
    clips.emplace_back(std::make_unique<MockClip>(15.0));
    tl.setPlaylist(std::move(clips), std::vector<TransitionConfig>(3, hardCut()));

    EXPECT_TRUE(tl.restoreCursor(2, 4.5));
    const auto cs = tl.getCursorSnapshot();
    EXPECT_EQ(cs.active_idx, 2);
    EXPECT_DOUBLE_EQ(cs.slot_pos_sec, 4.5);
}

TEST(TimelineRestoreCursor, ClampsOvershootToZero) {
    Timeline tl;
    std::vector<std::unique_ptr<IClip>> clips;
    clips.emplace_back(std::make_unique<MockClip>(5.0));
    tl.setPlaylist(std::move(clips), std::vector<TransitionConfig>(1, hardCut()));

    // slot_pos >= duration → start from 0 (stale snapshot defence).
    EXPECT_TRUE(tl.restoreCursor(0, 99.0));
    EXPECT_DOUBLE_EQ(tl.getCursorSnapshot().slot_pos_sec, 0.0);
}

TEST(TimelineRestoreCursor, ClampsNegativeToZero) {
    Timeline tl;
    std::vector<std::unique_ptr<IClip>> clips;
    clips.emplace_back(std::make_unique<MockClip>(5.0));
    tl.setPlaylist(std::move(clips), std::vector<TransitionConfig>(1, hardCut()));

    EXPECT_TRUE(tl.restoreCursor(0, -3.0));
    EXPECT_DOUBLE_EQ(tl.getCursorSnapshot().slot_pos_sec, 0.0);
}

// ─── pending_remove → fallback подмешивание (fix30 follow-up) ────────────────
// Регрессия: при удалении активного клипа выходной transition вместо
// плавного crossfade в next_clip делал crossfade в fallback и затем
// хард-кат в next. Visible как «вспышка fallback» на каждом переходе с
// помеченным клипом. Корректное поведение — fallback подставляется только
// когда живого next нет (single-clip / next тоже pending / next не
// подготовлен).

TEST(TimelinePendingRemoveCrossfade, ToLiveNextNotFallback) {
    Timeline tl;
    auto c0 = std::make_unique<MockClip>(10.0);
    auto c1 = std::make_unique<MockClip>(10.0);
    auto fb = std::make_unique<MockClip>(0.0);
    auto* raw_c1 = c0.get();  // before move
    auto* raw_next = c1.get();
    auto* raw_fb = fb.get();
    (void)raw_c1;

    std::vector<std::unique_ptr<IClip>> v;
    v.push_back(std::move(c0));
    v.push_back(std::move(c1));
    tl.setFallback(std::shared_ptr<IClip>(std::move(fb)));
    tl.setPlaylist(std::move(v), { liveMix(2.0), liveMix(2.0) },
                   std::vector<std::string>{"a", "b"});

    // active = idx 0, помечаем его на удаление.
    ASSERT_TRUE(tl.markForRemoval("a"));

    // Slot[0] (LiveMix 2s): content_len = 10 - 2 = 8, slot = 10. В
    // transition window [8, 10) clipB должен быть c1 (живой next), не fb.
    tl.advance(8.5);
    auto s = tl.peek();
    EXPECT_TRUE(s.in_transition);
    EXPECT_EQ(s.clipB, raw_next)
        << "Pending active с живым next → crossfade в next, не в fallback";
    EXPECT_NE(s.clipB, raw_fb)
        << "Fallback не должен подмешиваться при наличии живого next";
}

TEST(TimelinePendingRemoveCrossfade, ToFallbackWhenNextAlsoPending) {
    Timeline tl;
    auto c0 = std::make_unique<MockClip>(10.0);
    auto c1 = std::make_unique<MockClip>(10.0);
    auto fb = std::make_unique<MockClip>(0.0);
    auto* raw_fb = fb.get();

    std::vector<std::unique_ptr<IClip>> v;
    v.push_back(std::move(c0));
    v.push_back(std::move(c1));
    tl.setFallback(std::shared_ptr<IClip>(std::move(fb)));
    tl.setPlaylist(std::move(v), { liveMix(2.0), liveMix(2.0) },
                   std::vector<std::string>{"a", "b"});

    // Оба слота помечены на удаление — нет живого next, fallback оправдан.
    ASSERT_TRUE(tl.markForRemoval("a"));
    ASSERT_TRUE(tl.markForRemoval("b"));

    tl.advance(8.5);
    auto s = tl.peek();
    EXPECT_TRUE(s.in_transition);
    EXPECT_EQ(s.clipB, raw_fb)
        << "Когда next тоже помечен — обоих обречены, fallback оправдан";
}

TEST(TimelinePendingRemoveCrossfade, ToFallbackWhenSingleClip) {
    Timeline tl;
    auto c0 = std::make_unique<MockClip>(10.0);
    auto fb = std::make_unique<MockClip>(0.0);
    auto* raw_fb = fb.get();

    std::vector<std::unique_ptr<IClip>> v;
    v.push_back(std::move(c0));
    tl.setFallback(std::shared_ptr<IClip>(std::move(fb)));
    tl.setPlaylist(std::move(v), { liveMix(2.0) },
                   std::vector<std::string>{"a"});

    ASSERT_TRUE(tl.markForRemoval("a"));

    // Single-clip: next == cur (self-loop), играть нечего → fallback оправдан.
    tl.advance(8.5);
    auto s = tl.peek();
    EXPECT_TRUE(s.in_transition);
    EXPECT_EQ(s.clipB, raw_fb)
        << "Single-clip с pending — единственный обречённый клип → fallback";
}
