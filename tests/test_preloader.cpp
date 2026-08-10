// Step 13: Preloader — verifies async prepare()/release() lifecycle.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#include "core/Preloader.h"
#include "core/Timeline.h"
#include "transitions/ITransition.h"

namespace {

class CountingClip : public IClip {
public:
    explicit CountingClip(double dur) : dur_(dur) {}

    Frame getFrame() override {
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
    double getDuration() const override { return dur_; }
    bool   hasAudio()   const override { return false; }
    bool   isPrepared() const override { return prepared_.load(); }

    void prepare() override {
        ++prepare_calls;
        prepared_.store(true);
    }
    void release() override {
        ++release_calls;
        prepared_.store(false);
    }

    Frame getTailFrame() override { return getFrame(); }
    AudioFrame getTailAudio(int n) override { return getAudio(n); }
    void reset() override { ++reset_calls; }

    std::atomic<int> prepare_calls{0};
    std::atomic<int> release_calls{0};
    std::atomic<int> reset_calls{0};

private:
    double            dur_;
    std::atomic<bool> prepared_{false};
};

} // namespace

class PreloaderTest : public ::testing::Test {
protected:
    Timeline timeline_;
};

// Single-clip playlist: nothing to preload, no crashes.
TEST_F(PreloaderTest, SingleClipNoPreload) {
    auto clip = std::make_unique<CountingClip>(5.0);
    auto* raw = clip.get();
    std::vector<std::unique_ptr<IClip>> v;
    v.push_back(std::move(clip));
    timeline_.setPlaylist(std::move(v), {TransitionConfig{}});

    Preloader pre(timeline_, /*preload_sec=*/2.0);
    timeline_.advance(0.0); pre.tick();
    timeline_.advance(2.0); pre.tick();
    timeline_.advance(2.0); pre.tick();

    EXPECT_EQ(raw->prepare_calls.load(), 0)
        << "single-clip playlist must not trigger preload of itself";
}

// fix30: warm-set policy. Next clip (warm-set member) is prepared
// eagerly — first tick suffices, no need to wait for the preload_sec
// window. Cap=2 by default so for a 3-clip playlist with active=A the
// warm-set is {A, B}: B fires prepare immediately, C stays cold until
// it enters the warm-set on the A→B wrap.
TEST_F(PreloaderTest, WarmSetMemberPreparedOnFirstTick) {
    auto a = std::make_unique<CountingClip>(5.0);
    auto b = std::make_unique<CountingClip>(5.0);
    auto c = std::make_unique<CountingClip>(5.0);
    auto* rawB = b.get();
    auto* rawC = c.get();

    std::vector<std::unique_ptr<IClip>> v;
    v.push_back(std::move(a));
    v.push_back(std::move(b));
    v.push_back(std::move(c));
    timeline_.setPlaylist(std::move(v),
                          {TransitionConfig{}, TransitionConfig{}, TransitionConfig{}});

    Preloader pre(timeline_, /*preload_sec=*/2.0);

    // First tick — active=A (idx 0), warm-set={A, B}. B should fire
    // prepare async; allow the future to resolve.
    timeline_.advance(0.0); pre.tick();
    for (int i = 0; i < 100 && rawB->prepare_calls.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        pre.tick();
    }
    EXPECT_GE(rawB->prepare_calls.load(), 1);
    EXPECT_TRUE(rawB->isPrepared());

    // C is outside the warm-set (cap=2, active=A) — must remain cold.
    EXPECT_EQ(rawC->prepare_calls.load(), 0)
        << "C is not in warm-set while active=A; must not be prepared";
}

// After moving past clip A — with hard cuts so we have a non-transition
// window where the release branch can fire — A should be released async.
TEST_F(PreloaderTest, PreviousClipReleasedAfterTransition) {
    auto a = std::make_unique<CountingClip>(5.0);
    auto b = std::make_unique<CountingClip>(5.0);
    auto c = std::make_unique<CountingClip>(5.0);
    auto* rawA = a.get();

    // Both A and B already prepared so getState() doesn't skip transitions
    // and so timeline reports clipA as the "previous" clip eligible for release.
    a->prepare();
    b->prepare();
    c->prepare();

    std::vector<std::unique_ptr<IClip>> v;
    v.push_back(std::move(a));
    v.push_back(std::move(b));
    v.push_back(std::move(c));
    // Hard cuts (duration 0) → non-transition window exists between clips,
    // which is when Preloader's release branch fires.
    TransitionConfig hard_cut{TransitionType::HardCut, TransitionMode::HardCut, 0.0};
    timeline_.setPlaylist(std::move(v), {hard_cut, hard_cut, hard_cut});

    Preloader pre(timeline_, /*preload_sec=*/0.5);

    // Establish "current = clip 0".
    timeline_.advance(0.5); pre.tick();

    // Cross clip boundary (clip 0 ends at t=5.0). At t=5.5 cur=1, no transition.
    timeline_.advance(5.0); pre.tick();

    // Wait for async release.
    for (int i = 0; i < 200 && rawA->release_calls.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_GE(rawA->release_calls.load(), 1);
}

// Empty playlist: tick must be a no-op.
TEST_F(PreloaderTest, EmptyPlaylistTickNoOp) {
    Preloader pre(timeline_, 2.0);
    EXPECT_NO_THROW(pre.tick());
    EXPECT_NO_THROW(pre.tick());
}

// fix30: short playlist (n ≤ kWarmCap) — every clip is a warm-set member
// at every cursor position. Slot-wrap A→B and B→A must NOT trigger a
// release: the original release-on-wrap pingpong was the live-test trigger
// for the mid-stream-delete invalid-frame storm.
TEST_F(PreloaderTest, ShortPlaylistNoReleaseOnWrap) {
    auto a = std::make_unique<CountingClip>(5.0);
    auto b = std::make_unique<CountingClip>(5.0);
    auto* rawA = a.get();
    auto* rawB = b.get();

    a->prepare();
    b->prepare();

    std::vector<std::unique_ptr<IClip>> v;
    v.push_back(std::move(a));
    v.push_back(std::move(b));
    TransitionConfig hard_cut{TransitionType::HardCut, TransitionMode::HardCut, 0.0};
    timeline_.setPlaylist(std::move(v), {hard_cut, hard_cut});

    Preloader pre(timeline_, /*preload_sec=*/0.5);

    // Tick across two full A→B→A cycles, sleeping briefly so any rogue
    // async release would have a chance to land.
    for (int i = 0; i < 25; ++i) {
        timeline_.advance(0.5);
        pre.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    EXPECT_EQ(rawA->release_calls.load(), 0)
        << "n=2 playlist: A is always in warm-set, must never be released";
    EXPECT_EQ(rawB->release_calls.load(), 0)
        << "n=2 playlist: B is always in warm-set, must never be released";
    EXPECT_TRUE(rawA->isPrepared());
    EXPECT_TRUE(rawB->isPrepared());
}

// fix30: long playlist (n > kWarmCap) — clips outside the warm-set get
// released one at a time (staggered) by the cold-sweep, never in batch.
TEST_F(PreloaderTest, LongPlaylistColdReleaseStaggered) {
    auto a = std::make_unique<CountingClip>(5.0);
    auto b = std::make_unique<CountingClip>(5.0);
    auto c = std::make_unique<CountingClip>(5.0);
    auto d = std::make_unique<CountingClip>(5.0);
    auto e = std::make_unique<CountingClip>(5.0);
    auto* rawA = a.get();
    auto* rawB = b.get();
    auto* rawC = c.get();
    auto* rawD = d.get();
    auto* rawE = e.get();

    // All five start prepared — simulates a steady-state where prior cycles
    // had warmed everything.
    a->prepare(); b->prepare(); c->prepare(); d->prepare(); e->prepare();

    std::vector<std::unique_ptr<IClip>> v;
    v.push_back(std::move(a));
    v.push_back(std::move(b));
    v.push_back(std::move(c));
    v.push_back(std::move(d));
    v.push_back(std::move(e));
    TransitionConfig hard_cut{TransitionType::HardCut, TransitionMode::HardCut, 0.0};
    timeline_.setPlaylist(std::move(v),
                          {hard_cut, hard_cut, hard_cut, hard_cut, hard_cut});

    Preloader pre(timeline_, /*preload_sec=*/0.5);

    // Active=A, warm-set={A, B}. C, D, E are cold candidates.
    timeline_.advance(0.5); pre.tick();

    // Tick repeatedly with sleeps so async releases finalize between ticks.
    auto total_cold_releases = [&] {
        return rawC->release_calls.load() + rawD->release_calls.load()
             + rawE->release_calls.load();
    };
    for (int i = 0; i < 50 && total_cold_releases() < 3; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        timeline_.advance(0.01);
        pre.tick();
    }

    EXPECT_EQ(rawC->release_calls.load(), 1);
    EXPECT_EQ(rawD->release_calls.load(), 1);
    EXPECT_EQ(rawE->release_calls.load(), 1);
    EXPECT_EQ(rawA->release_calls.load(), 0) << "A is active — never released";
    EXPECT_EQ(rawB->release_calls.load(), 0) << "B is in warm-set — never released";
}

// fix30: markForRemoval on the active clip in a 2-clip playlist must NOT
// block warm-set maintenance for the other clip. Pre-fix30, the broad
// active_pending gate suppressed all section-3 work, allowing the other
// clip to drift into a released-but-not-re-prepared state by the time
// reapPendingActive landed the cursor on it.
TEST_F(PreloaderTest, MarkForRemovalActiveDoesNotBlockOther) {
    auto a = std::make_unique<CountingClip>(5.0);
    auto b = std::make_unique<CountingClip>(5.0);
    auto* rawA = a.get();
    auto* rawB = b.get();

    a->prepare();
    b->prepare();

    std::vector<std::unique_ptr<IClip>> v;
    v.push_back(std::move(a));
    v.push_back(std::move(b));
    TransitionConfig hard_cut{TransitionType::HardCut, TransitionMode::HardCut, 0.0};
    std::vector<std::string> paths{"a.mp4", "b.mp4"};
    timeline_.setPlaylist(std::move(v), {hard_cut, hard_cut}, std::move(paths));

    Preloader pre(timeline_, /*preload_sec=*/0.5);

    // Advance to active=B. With hard cuts, slot[0] = 5s.
    timeline_.advance(0.5); pre.tick();
    timeline_.advance(5.0); pre.tick();
    ASSERT_EQ(timeline_.getActiveIndex(), 1);

    ASSERT_TRUE(timeline_.markForRemoval("b.mp4"));

    // Continue ticking — A must remain prepared throughout.
    for (int i = 0; i < 30; ++i) {
        timeline_.advance(0.1);
        pre.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        EXPECT_TRUE(rawA->isPrepared())
            << "A (warm-set, idx 0) must stay prepared while active B is pending";
    }
    EXPECT_EQ(rawA->release_calls.load(), 0);
    // B is the active pending — its release is owned by the slot-wrap
    // branch firing once active changes (covered elsewhere).
    (void)rawB;
}
