#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "clips/VideoClip.h"

namespace fs = std::filesystem;

// ─── helpers ─────────────────────────────────────────────────────────────────

// Writes a YUV4MPEG2 (.y4m) file — trivially parseable by FFmpeg without any
// container library.  Each frame is a flat mid-gray YUV 4:2:0 image; the first
// pixel of the Y plane is set to frame_idx*10 so frames are distinguishable.
static fs::path createY4mVideo(int w, int h, int num_frames) {
    auto path = fs::temp_directory_path() / "liveqx_test_video.y4m";
    std::ofstream f(path, std::ios::binary);

    f << "YUV4MPEG2 W" << w << " H" << h << " F25:1 Ip A0:0 C420\n";

    const int y_sz  = w * h;
    const int uv_sz = (w / 2) * (h / 2);
    std::vector<uint8_t> y(y_sz, 128u);
    std::vector<uint8_t> u(uv_sz, 128u);
    std::vector<uint8_t> v(uv_sz, 128u);

    for (int fi = 0; fi < num_frames; ++fi) {
        y[0] = static_cast<uint8_t>(fi * 10 % 256);
        f << "FRAME\n";
        f.write(reinterpret_cast<const char*>(y.data()), y_sz);
        f.write(reinterpret_cast<const char*>(u.data()), uv_sz);
        f.write(reinterpret_cast<const char*>(v.data()), uv_sz);
    }
    return path;
}

// ─── fixture ─────────────────────────────────────────────────────────────────

class VideoClipTest : public ::testing::Test {
protected:
    static constexpr int kSrcW     = 16;
    static constexpr int kSrcH     = 16;
    static constexpr int kOutW     = 64;
    static constexpr int kOutH     = 48;
    static constexpr int kNumFrames = 25; // 1 second at 25 fps

    fs::path vid_path_;

    void SetUp()    override { vid_path_ = createY4mVideo(kSrcW, kSrcH, kNumFrames); }
    void TearDown() override { fs::remove(vid_path_); }
};

// ─── tests ───────────────────────────────────────────────────────────────────

// Before prepare(), getFrame() must return an invalid frame.
TEST_F(VideoClipTest, GetFrameBeforePrepareIsInvalid) {
    VideoClip clip(vid_path_.string(), kOutW, kOutH);
    EXPECT_FALSE(clip.getFrame().valid());
}

// After prepare(), at least one frame must be valid with correct geometry.
TEST_F(VideoClipTest, PrepareProducesValidFrame) {
    VideoClip clip(vid_path_.string(), kOutW, kOutH);
    clip.prepare();

    // Give the decode thread time to fill the ring buffer.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    Frame f = clip.getFrame();
    ASSERT_TRUE(f.valid());
    EXPECT_EQ(f.width,  kOutW);
    EXPECT_EQ(f.height, kOutH);
    EXPECT_EQ(f.sizeBytes(), static_cast<size_t>(kOutW) * kOutH * 4u);
}

// getDuration() must be close to 1 second (25 frames / 25 fps).
TEST_F(VideoClipTest, GetDurationApprox1Second) {
    VideoClip clip(vid_path_.string(), kOutW, kOutH);
    clip.prepare();

    double dur = clip.getDuration();
    EXPECT_GT(dur, 0.9);
    EXPECT_LT(dur, 1.5);
}

// Y4M carries no audio stream.
TEST_F(VideoClipTest, HasNoAudio) {
    VideoClip clip(vid_path_.string(), kOutW, kOutH);
    clip.prepare();
    EXPECT_FALSE(clip.hasAudio());
}

// getAudio() on a video-only clip returns an invalid frame (not a crash).
TEST_F(VideoClipTest, GetAudioOnVideoOnlyReturnsInvalid) {
    VideoClip clip(vid_path_.string(), kOutW, kOutH);
    clip.prepare();
    AudioFrame af = clip.getAudio(1920);
    EXPECT_FALSE(af.valid);
}

// Drain all 25 frames; each must be valid with correct dimensions.
TEST_F(VideoClipTest, GetFrameLoopReceivesAllFrames) {
    VideoClip clip(vid_path_.string(), kOutW, kOutH);
    clip.prepare();

    int received = 0;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);

    while (received < kNumFrames && std::chrono::steady_clock::now() < deadline) {
        Frame f = clip.getFrame();
        if (f.valid()) {
            EXPECT_EQ(f.width,  kOutW);
            EXPECT_EQ(f.height, kOutH);
            ++received;
        }
    }

    EXPECT_EQ(received, kNumFrames);
}

// PTS must be non-decreasing across successive frames.
TEST_F(VideoClipTest, FramePtsNonDecreasing) {
    VideoClip clip(vid_path_.string(), kOutW, kOutH);
    clip.prepare();

    int64_t prev_pts = -1;
    int     received = 0;
    auto    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);

    while (received < kNumFrames && std::chrono::steady_clock::now() < deadline) {
        Frame f = clip.getFrame();
        if (!f.valid()) continue;
        EXPECT_GE(f.pts, prev_pts);
        prev_pts = f.pts;
        ++received;
    }
    EXPECT_EQ(received, kNumFrames);
}

// After release(), getFrame() returns an invalid frame.
TEST_F(VideoClipTest, ReleaseInvalidatesQueue) {
    VideoClip clip(vid_path_.string(), kOutW, kOutH);
    clip.prepare();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    clip.release();

    EXPECT_FALSE(clip.getFrame().valid());
    // Duration is stable container metadata — retained across release()
    // so Timeline's slot arithmetic stays consistent.
    EXPECT_GT(clip.getDuration(), 0.0);
}

// prepare() → release() → prepare() must work correctly.
TEST_F(VideoClipTest, PrepareAfterRelease) {
    VideoClip clip(vid_path_.string(), kOutW, kOutH);

    clip.prepare();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    clip.release();

    clip.prepare();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    Frame f = clip.getFrame();
    EXPECT_TRUE(f.valid());
}

// fix30 follow-up: head_buffer must NOT cost frames from the live queue on
// first playback. captureHeadBuffer used to set skip_video_frames_ for every
// prepare(), but head_video_ is only consumed by self-loop transitions —
// non-self-loop clips lost their first head_buffer_sec_ of content. The
// field-test repro: 15s video with a 2s default LiveMix transition started
// at file t=2s (skipped 50 frames) and ran out of queue ~4s before slot end
// (perceived as a long stuck last frame). PTS-based distinct count is used
// because past the live queue VideoClip::getFrame() falls back to the
// tail-frame snapshot — counting raw `valid()` calls would always read
// kNumFrames regardless of skip.
namespace {
int countDistinctFrames(VideoClip& clip, int budget_frames) {
    std::set<int64_t> pts_seen;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    int  popped   = 0;
    while (popped < budget_frames * 2
           && std::chrono::steady_clock::now() < deadline) {
        Frame f = clip.getFrame();
        if (f.valid()) {
            pts_seen.insert(f.pts);
            ++popped;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return static_cast<int>(pts_seen.size());
}
}  // namespace

TEST_F(VideoClipTest, HeadBufferDoesNotSkipFirstPlayback) {
    VideoClip clip(vid_path_.string(), kOutW, kOutH);
    clip.setHeadBufferSeconds(0.2);  // ~5 frames at 25 fps
    clip.prepare();

    EXPECT_EQ(countDistinctFrames(clip, kNumFrames), kNumFrames)
        << "head buffer must not consume frames from the live queue when "
           "head_playback_ has never been engaged (multi-clip transitions)";
}

// Self-loop semantics: once setHeadPlayback() has fired (rising edge), the
// next reset() must apply the skip — otherwise the user sees the head
// buffer played back-to-back from head_video_ and then again from the live
// queue. Verifies the head_was_played_ flag is wired through both directions.
TEST_F(VideoClipTest, ResetAppliesSkipOnlyAfterHeadPlayback) {
    VideoClip clip(vid_path_.string(), kOutW, kOutH);
    clip.setHeadBufferSeconds(0.2);  // ~5 frames at 25 fps
    clip.prepare();

    EXPECT_EQ(countDistinctFrames(clip, kNumFrames), kNumFrames);

    // Engage self-loop transition: head_playback rises, then falls.
    clip.setHeadPlayback(true);
    clip.setHeadPlayback(false);
    clip.reset();

    // After self-loop reset, live queue must skip the head: the user has
    // already seen those frames via head_video_, so distinct count drops by
    // the head buffer size (5 frames at 0.2s × 25 fps).
    const int after_self_loop = countDistinctFrames(clip, kNumFrames);
    EXPECT_LT(after_self_loop, kNumFrames)
        << "self-loop reset must apply skip so the live queue does not "
           "duplicate frames already shown via head_video_";

    // A subsequent reset WITHOUT a fresh setHeadPlayback rising edge must
    // NOT apply the skip — multi-clip wrap path.
    clip.reset();
    const int after_plain_reset = countDistinctFrames(clip, kNumFrames);
    EXPECT_EQ(after_plain_reset, kNumFrames)
        << "reset() following a non-self-loop wrap must serve the entire "
           "clip from frame 0";
}

// Opening a non-existent file must throw std::runtime_error.
TEST_F(VideoClipTest, PrepareThrowsOnMissingFile) {
    VideoClip clip("/nonexistent/path/video.mp4", kOutW, kOutH);
    EXPECT_THROW(clip.prepare(), std::runtime_error);
}
