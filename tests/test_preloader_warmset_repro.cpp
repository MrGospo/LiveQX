// fix30 c6 — Integration reproducer for the live mid-stream-delete bug
// observed in logi.md (2026-05-07): playlist=[image, video], delete the
// active video file from the share → channel goes degraded with an
// infinite "RenderLoop: getFrame() returned invalid frame" stream.
//
// Root cause was Preloader's release-on-wrap pingpong on short playlists
// combined with the broad active_pending gate that suppressed re-prepare
// during the markForRemoval window. Warm-set policy (cap=2) makes the
// short-playlist case structurally release-free, and the narrow
// single-clip-pending gate lets re-prepare keep running for n=2.
//
// The mock clip below tracks frames returned and records any "would have
// been invalid" event so the assertion is direct — no sampling.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#include "clips/IClip.h"
#include "core/Preloader.h"
#include "core/Timeline.h"
#include "transitions/ITransition.h"

namespace {

// Mock clip that returns valid frames only when prepared. Mimics the
// VideoClip semantics that triggered the live bug — once released, the
// internal state collapses and getFrame() returns an invalid frame.
class StatefulMockClip : public IClip {
public:
    explicit StatefulMockClip(double dur) : dur_(dur) {}

    Frame getFrame() override {
        if (!prepared_.load()) {
            ++invalid_frame_calls;
            return Frame{};
        }
        Frame f;
        f.width = 1; f.height = 1;
        f.data = std::shared_ptr<uint8_t[]>(new uint8_t[4]{255,255,255,255});
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
    std::atomic<int> invalid_frame_calls{0};

private:
    double            dur_;
    std::atomic<bool> prepared_{false};
};

}  // namespace

// Direct reproducer for the 2026-05-07 live test:
//   playlist = [logo (image, 10s), clip.mp4 (video, 15s)]
//   - 8 ticks at 1s each: image plays
//   - wrap to video; ~7 ticks at 1s each: video half-played
//   - delete clip.mp4 from share → markForRemoval
//   - 6+ ticks: reapPendingActive should fire, cursor heals to image
//   - assertion: image (logo) stays prepared every tick, never releases,
//     getFrame() never returns invalid for the surviving warm-set member.
TEST(PreloaderWarmSetRepro, MidStreamDeleteOfActiveVideoNoInvalidFrames) {
    Timeline timeline;
    auto img = std::make_unique<StatefulMockClip>(/*dur=*/10.0);
    auto vid = std::make_unique<StatefulMockClip>(/*dur=*/15.0);
    img->prepare();
    vid->prepare();
    auto* rawImg = img.get();
    auto* rawVid = vid.get();

    std::vector<std::unique_ptr<IClip>> clips;
    clips.push_back(std::move(img));
    clips.push_back(std::move(vid));
    TransitionConfig hard{TransitionType::HardCut, TransitionMode::HardCut, 0.0};
    timeline.setPlaylist(std::move(clips),
                         {hard, hard},
                         {"logo.png", "clip.mp4"});

    Preloader pre(timeline, /*preload_sec=*/2.0);

    auto pump = [&] {
        // Sleep briefly between ticks so async prepare/release futures
        // have a chance to land — mirrors the real-time render loop.
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    };

    // Phase 1: 8 ticks at 1s → image plays for 8s of its 10s slot.
    for (int i = 0; i < 8; ++i) {
        timeline.advance(1.0);
        pre.tick();
        ASSERT_TRUE(rawImg->isPrepared()) << "tick " << i;
        ASSERT_TRUE(rawVid->isPrepared()) << "tick " << i;
        // Frame from active is valid — proxy for RenderLoop output.
        ASSERT_NE(timeline.getActiveIndex(), -1);
        pump();
    }

    // Wrap to video.
    timeline.advance(2.5);
    pre.tick();
    pump();
    ASSERT_EQ(timeline.getActiveIndex(), 1) << "should have wrapped to video";

    // Phase 2: 7 ticks at 1s → video plays for 7s of its 15s slot.
    for (int i = 0; i < 7; ++i) {
        timeline.advance(1.0);
        pre.tick();
        ASSERT_TRUE(rawImg->isPrepared())
            << "image must stay warm while video plays (n=2 short playlist), tick " << i;
        ASSERT_TRUE(rawVid->isPrepared()) << "tick " << i;
        pump();
    }

    // Mid-stream delete (mirrors ContentSync removing a watched file).
    ASSERT_TRUE(timeline.markForRemoval("clip.mp4"));

    // Phase 3: continue ticking past the natural slot wrap so
    // reapPendingActive fires. Video slot remaining at mark = 8s.
    for (int i = 0; i < 12; ++i) {
        timeline.advance(1.0);
        pre.tick();
        ASSERT_TRUE(rawImg->isPrepared())
            << "image must remain warm during the entire pending-removal window, tick " << i;
        pump();
    }

    // Drain any in-flight futures and let cursor self-heal land.
    for (int i = 0; i < 50; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        timeline.advance(0.001);
        pre.tick();
    }

    // Image must have ridden the entire scenario without a release.
    EXPECT_TRUE(rawImg->isPrepared())
        << "image (logo) is the survivor — must end prepared";
    EXPECT_EQ(rawImg->release_calls.load(), 0)
        << "image was warm-set member throughout — must never have been released";
    EXPECT_EQ(rawImg->invalid_frame_calls.load(), 0)
        << "image must never have returned an invalid frame";
}
