// fix23 commit 6 — PreviewEncoder unit tests.
//
// Smoke + behaviour: scale RGBA → YUV420P → x264, observe NAL emission.
// Linked against the FFmpeg integration suite because libavcodec/libswscale
// are required at runtime.

#include <atomic>
#include <cstring>
#include <vector>

#include <gtest/gtest.h>

#include "core/Frame.h"
#include "preview/PreviewEncoder.h"

namespace {

using liveqx::preview::PreviewEncoder;

Frame makeRgbaFrame(int w, int h, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    Frame f;
    f.width  = w;
    f.height = h;
    f.data.reset(new std::uint8_t[static_cast<std::size_t>(w) * h * 4]);
    auto* px = f.data.get();
    for (int i = 0; i < w * h; ++i) {
        px[i * 4 + 0] = r;
        px[i * 4 + 1] = g;
        px[i * 4 + 2] = b;
        px[i * 4 + 3] = 255;
    }
    f.pts = 0;
    return f;
}

}  // namespace

TEST(PreviewEncoder, StartFailsTwice) {
    PreviewEncoder enc{{}};
    ASSERT_TRUE(enc.start([](auto*, auto, auto, auto) {}));
    EXPECT_FALSE(enc.start([](auto*, auto, auto, auto) {}));
}

TEST(PreviewEncoder, EmitsKeyframeOnFirstFrame) {
    PreviewEncoder enc{{}};

    int  cb_count = 0;
    bool first_was_keyframe = false;

    ASSERT_TRUE(enc.start(
        [&](const std::uint8_t* /*data*/, std::size_t size,
            bool is_kf, std::int64_t /*pts_us*/) {
            if (cb_count == 0) {
                EXPECT_GT(size, 0u);
                first_was_keyframe = is_kf;
            }
            ++cb_count;
        }));

    auto frame = makeRgbaFrame(1280, 720, 255, 128, 64);
    // ultrafast/zerolatency emits the IDR access unit on the first
    // submitted frame — no pipeline lookahead.
    ASSERT_TRUE(enc.encode(frame));

    EXPECT_GE(cb_count, 1);
    EXPECT_TRUE(first_was_keyframe);
    EXPECT_GT(enc.bytesEmitted(), 0);

    enc.stop();
}

TEST(PreviewEncoder, EmitsKeyframeAtGopBoundary) {
    PreviewEncoder::Config cfg;
    cfg.gop = 10;  // tight gop so the test stays small
    PreviewEncoder enc{cfg};

    int  total_kf  = 0;

    ASSERT_TRUE(enc.start(
        [&](const std::uint8_t*, std::size_t, bool is_kf, std::int64_t) {
            if (is_kf) ++total_kf;
        }));

    auto frame = makeRgbaFrame(640, 360, 0, 200, 100);
    for (int i = 0; i < 30; ++i) {
        frame.pts = i * 40'000;  // 25fps in microseconds
        ASSERT_TRUE(enc.encode(frame));
    }
    enc.stop();

    // At least the initial IDR + one or two GOP boundaries (within 30
    // frames @ gop=10). x264 may also emit a final flush IDR; we only
    // require ≥2 to confirm gop_size honoured.
    EXPECT_GE(total_kf, 2);
    EXPECT_EQ(enc.framesEncoded(), 30);
}

TEST(PreviewEncoder, RejectsInvalidFrame) {
    PreviewEncoder enc{{}};
    ASSERT_TRUE(enc.start([](auto*, auto, auto, auto) {}));

    Frame empty;  // valid()==false
    EXPECT_FALSE(enc.encode(empty));

    enc.stop();
}

TEST(PreviewEncoder, EncodeBeforeStartReturnsFalse) {
    PreviewEncoder enc{{}};
    auto frame = makeRgbaFrame(320, 180, 0, 0, 0);
    EXPECT_FALSE(enc.encode(frame));
}
