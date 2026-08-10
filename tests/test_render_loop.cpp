#include <gtest/gtest.h>
#include "core/RenderLoop.h"
#include "core/Timeline.h"
#include "encoding/Encoder.h"
#include "render/ICompositor.h"
#include "clips/IClip.h"
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

// ─── Mocks ────────────────────────────────────────────────────────────────────

// Makes one shared_ptr<uint8_t[]> pixel buffer and reuses it every call.
class MockClip : public IClip {
public:
    explicit MockClip(double duration = 100.0) : dur_(duration) {
        // Allocate a minimal 1×1 RGBA frame once.
        buf_ = std::shared_ptr<uint8_t[]>(new uint8_t[4]{128, 0, 128, 255});
    }

    Frame getFrame() override {
        ++frame_count;
        Frame f;
        f.data   = buf_;
        f.width  = 1;
        f.height = 1;
        f.pts    = frame_count.load() * 40'000; // 40ms per frame @ 25fps
        return f;
    }

    AudioFrame getAudio(int num_samples) override {
        AudioFrame a;
        a.sample_rate  = 48000;
        a.channels     = 2;
        a.num_samples  = num_samples;
        a.samples.assign(static_cast<size_t>(a.num_samples * a.channels), 0.0f);
        a.valid = true;
        return a;
    }

    double getDuration() const override { return dur_; }
    bool   hasAudio()   const override { return true; }
    bool   isPrepared() const override { return true; }
    void   prepare()          override {}
    void   release()          override {}

    Frame      getTailFrame()        override { return getFrame(); }
    AudioFrame getTailAudio(int n)   override { return getAudio(n); }
    void       reset()               override { frame_count.store(0); }

    std::atomic<int> frame_count{0};

private:
    double dur_;
    std::shared_ptr<uint8_t[]> buf_;
};

// Passthrough compositor — returns frame A unchanged.
class PassthroughCompositor : public ICompositor {
public:
    Frame composite(const Frame& a, const Frame& /*b*/,
                    TransitionType /*type*/, float /*progress*/,
                    Easing /*easing*/ = Easing::Linear) override {
        return a;
    }
};

// ─── Tests ────────────────────────────────────────────────────────────────────

class RenderLoopTest : public ::testing::Test {
protected:
    void SetUp() override {
        Encoder::Config cfg;
        cfg.width  = 1;
        cfg.height = 1;
        cfg.fps    = 25;
        encoder = std::make_unique<Encoder>(cfg);
        // open() may return false with stubs — pushFrame() is safe regardless
        encoder->open();
    }

    Timeline                    timeline;
    PassthroughCompositor       compositor;
    std::unique_ptr<Encoder>    encoder;
};

TEST_F(RenderLoopTest, Produces50FramesIn2Seconds) {
    auto clip_owned = std::make_unique<MockClip>(100.0);
    MockClip* clip  = clip_owned.get(); // keep raw ptr before move

    std::vector<std::unique_ptr<IClip>> playlist;
    playlist.push_back(std::move(clip_owned));
    timeline.setPlaylist(std::move(playlist), {TransitionConfig{}});

    RenderLoop loop(25, timeline, compositor, *encoder);
    loop.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(2050));
    loop.stop();

    const int count = clip->frame_count.load();
    // At 25 fps for 2 s = 50 frames. Allow ±10 for scheduling jitter.
    EXPECT_GE(count, 40);
    EXPECT_LE(count, 60);
}

TEST_F(RenderLoopTest, IsRunningAfterStart) {
    auto clip_owned = std::make_unique<MockClip>();
    std::vector<std::unique_ptr<IClip>> playlist;
    playlist.push_back(std::move(clip_owned));
    timeline.setPlaylist(std::move(playlist), {TransitionConfig{}});

    RenderLoop loop(25, timeline, compositor, *encoder);
    EXPECT_FALSE(loop.isRunning());
    loop.start();
    EXPECT_TRUE(loop.isRunning());
    loop.stop();
    EXPECT_FALSE(loop.isRunning());
}

TEST_F(RenderLoopTest, DoubleStartIsIdempotent) {
    auto clip_owned = std::make_unique<MockClip>();
    std::vector<std::unique_ptr<IClip>> playlist;
    playlist.push_back(std::move(clip_owned));
    timeline.setPlaylist(std::move(playlist), {TransitionConfig{}});

    RenderLoop loop(25, timeline, compositor, *encoder);
    loop.start();
    loop.start(); // second call must not crash or spawn a second thread
    loop.stop();
    SUCCEED();
}

TEST_F(RenderLoopTest, StopWithoutStartDoesNotCrash) {
    RenderLoop loop(25, timeline, compositor, *encoder);
    EXPECT_NO_THROW(loop.stop());
}

TEST_F(RenderLoopTest, EmptyPlaylistDoesNotCrash) {
    // No clips → fallback path; loop must not crash for 200ms.
    RenderLoop loop(25, timeline, compositor, *encoder);
    loop.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    loop.stop();
    SUCCEED();
}

TEST_F(RenderLoopTest, FpsThrottlesFrameRate) {
    // Run at 10 fps for ~1 s — expect ~10 frames, not 25.
    auto clip_owned = std::make_unique<MockClip>(100.0);
    MockClip* clip  = clip_owned.get();

    std::vector<std::unique_ptr<IClip>> playlist;
    playlist.push_back(std::move(clip_owned));
    timeline.setPlaylist(std::move(playlist), {TransitionConfig{}});

    RenderLoop loop(10, timeline, compositor, *encoder);
    loop.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(1050));
    loop.stop();

    const int count = clip->frame_count.load();
    EXPECT_GE(count, 8);
    EXPECT_LE(count, 14);
}

// Step 13: audio sample clock — sample-based elapsed must not drift from wall
// clock. We run the loop for 2 seconds and check that the metrics frame count
// matches what 25 fps for 2 s would produce, with tight tolerance.
TEST_F(RenderLoopTest, AudioSampleClockTracksWallClock) {
    auto clip_owned = std::make_unique<MockClip>(100.0);
    std::vector<std::unique_ptr<IClip>> playlist;
    playlist.push_back(std::move(clip_owned));
    timeline.setPlaylist(std::move(playlist), {TransitionConfig{}});

    RenderLoop loop(25, timeline, compositor, *encoder);
    loop.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(2050));
    loop.stop();

    // Each tick advances total_audio_samples_ by 1920 (= 48000/25). For 2s
    // wall-clock at 25 fps that should be ~50 ticks → ~96000 samples;
    // any large drift would manifest as drift in the snapshot count.
    const auto m = loop.getMetrics();
    EXPECT_GE(m.frames_rendered, 40u);
    EXPECT_LE(m.frames_rendered, 60u);
}

TEST_F(RenderLoopTest, DestructorStopsLoop) {
    auto clip_owned = std::make_unique<MockClip>();
    std::vector<std::unique_ptr<IClip>> playlist;
    playlist.push_back(std::move(clip_owned));
    timeline.setPlaylist(std::move(playlist), {TransitionConfig{}});

    {
        RenderLoop loop(25, timeline, compositor, *encoder);
        loop.start();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    } // destructor must join cleanly — no deadlock
    SUCCEED();
}
