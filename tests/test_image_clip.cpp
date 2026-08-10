#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "clips/ImageClip.h"

namespace fs = std::filesystem;

// ─── helpers ─────────────────────────────────────────────────────────────────

// Writes a minimal 24-bit BMP file that FFmpeg can decode.
// Returns the path to the temporary file.
static fs::path createBmpImage(int w, int h) {
    auto path = fs::temp_directory_path() / "liveqx_test_image.bmp";
    std::ofstream f(path, std::ios::binary);

    const int rowStride = (w * 3 + 3) & ~3;
    const int pixelDataSize = rowStride * h;
    const int fileSize = 54 + pixelDataSize;

    auto w16 = [&](uint16_t v){ f.write(reinterpret_cast<char*>(&v), 2); };
    auto w32 = [&](uint32_t v){ f.write(reinterpret_cast<char*>(&v), 4); };
    auto ws32= [&](int32_t  v){ f.write(reinterpret_cast<char*>(&v), 4); };

    // File header
    f << 'B' << 'M';
    w32(static_cast<uint32_t>(fileSize));
    w16(0); w16(0);   // reserved
    w32(54);           // pixel data offset

    // DIB header (BITMAPINFOHEADER)
    w32(40);            // header size
    ws32(w);
    ws32(-h);           // negative = top-down storage
    w16(1);             // color planes
    w16(24);            // bits per pixel
    w32(0);             // compression (BI_RGB)
    w32(0);             // image size (0 allowed for BI_RGB)
    ws32(0); ws32(0);   // pixels per meter
    w32(0); w32(0);     // color table

    // Pixel data: BGR order, padded rows
    std::vector<uint8_t> row(static_cast<size_t>(rowStride), 0);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            row[x * 3 + 2] = static_cast<uint8_t>(x * 255 / (w > 1 ? w - 1 : 1)); // R
            row[x * 3 + 1] = 128;                                                    // G
            row[x * 3 + 0] = static_cast<uint8_t>(y * 255 / (h > 1 ? h - 1 : 1)); // B
        }
        f.write(reinterpret_cast<const char*>(row.data()),
                static_cast<std::streamsize>(rowStride));
    }
    return path;
}

// ─── fixture ─────────────────────────────────────────────────────────────────

class ImageClipTest : public ::testing::Test {
protected:
    static constexpr int kSrcW = 16, kSrcH = 16;
    static constexpr int kOutW = 64, kOutH = 48;

    fs::path img_path_;

    void SetUp() override {
        img_path_ = createBmpImage(kSrcW, kSrcH);
    }

    void TearDown() override {
        fs::remove(img_path_);
    }
};

// ─── tests ───────────────────────────────────────────────────────────────────

// Before prepare(), getFrame() must return an invalid (null) frame.
TEST_F(ImageClipTest, GetFrameBeforePrepareIsInvalid) {
    ImageClip clip(img_path_.string(), 5.0, kOutW, kOutH);
    EXPECT_FALSE(clip.getFrame().valid());
}

// After prepare(), frame must be valid with correct geometry and RGBA byte count.
TEST_F(ImageClipTest, PrepareProducesValidFrame) {
    ImageClip clip(img_path_.string(), 5.0, kOutW, kOutH);
    clip.prepare();

    Frame f = clip.getFrame();
    EXPECT_TRUE(f.valid());
    EXPECT_EQ(f.width,  kOutW);
    EXPECT_EQ(f.height, kOutH);
    EXPECT_EQ(f.sizeBytes(), static_cast<size_t>(kOutW) * kOutH * 4u);
}

// getDuration() must return the value passed in the constructor (UI-controlled).
TEST_F(ImageClipTest, GetDurationReturnsConstructorValue) {
    ImageClip clip(img_path_.string(), 7.5, kOutW, kOutH);
    EXPECT_DOUBLE_EQ(clip.getDuration(), 7.5);
}

// Images carry no audio.
TEST_F(ImageClipTest, HasNoAudio) {
    ImageClip clip(img_path_.string(), 5.0, kOutW, kOutH);
    EXPECT_FALSE(clip.hasAudio());
}

// getAudio() returns valid silence (all zeros).
TEST_F(ImageClipTest, GetAudioReturnsSilence) {
    ImageClip clip(img_path_.string(), 5.0, kOutW, kOutH);
    AudioFrame af = clip.getAudio(1920);
    EXPECT_TRUE(af.valid);
    EXPECT_GT(af.num_samples, 0);
    bool all_zero = std::all_of(af.samples.begin(), af.samples.end(),
                                [](float v) { return v == 0.0f; });
    EXPECT_TRUE(all_zero);
}

// Core contract: getFrame() called 100× returns the same underlying pixel buffer.
// Validates zero-copy semantics — no buffer is re-allocated between calls.
TEST_F(ImageClipTest, GetFrame100TimesSamePointer) {
    ImageClip clip(img_path_.string(), 5.0, kOutW, kOutH);
    clip.prepare();

    const uint8_t* const expected = clip.getFrame().pixels();
    ASSERT_NE(expected, nullptr);

    for (int i = 0; i < 100; ++i) {
        Frame f = clip.getFrame();
        EXPECT_EQ(f.pixels(), expected) << "pointer changed on call " << i;
    }
}

// Copying a Frame shares the buffer; both copies point to the same memory.
TEST_F(ImageClipTest, FrameCopySharesBuffer) {
    ImageClip clip(img_path_.string(), 5.0, kOutW, kOutH);
    clip.prepare();

    Frame a = clip.getFrame();
    Frame b = a;   // shared_ptr copy

    EXPECT_EQ(a.pixels(), b.pixels());
    EXPECT_EQ(a.data.use_count(), b.data.use_count());
    EXPECT_GE(a.data.use_count(), 2);   // at least a and b hold it
}

// After release(), getFrame() returns an invalid frame again.
TEST_F(ImageClipTest, ReleaseInvalidatesFrame) {
    ImageClip clip(img_path_.string(), 5.0, kOutW, kOutH);
    clip.prepare();
    EXPECT_TRUE(clip.getFrame().valid());

    clip.release();
    EXPECT_FALSE(clip.getFrame().valid());
}

// prepare() → release() → prepare() must work correctly (re-entrant).
TEST_F(ImageClipTest, PrepareAfterRelease) {
    ImageClip clip(img_path_.string(), 5.0, kOutW, kOutH);

    clip.prepare();
    const uint8_t* ptr1 = clip.getFrame().pixels();
    clip.release();

    clip.prepare();
    const uint8_t* ptr2 = clip.getFrame().pixels();

    EXPECT_NE(ptr2, nullptr);
    // A new prepare() may legitimately allocate a new buffer, so we only
    // check that it is valid — not that it is the same pointer.
    EXPECT_TRUE(clip.getFrame().valid());
    (void)ptr1;
}

// Opening a non-existent file must throw.
TEST_F(ImageClipTest, PrepareThrowsOnMissingFile) {
    ImageClip clip("/nonexistent/path/image.jpg", 5.0, kOutW, kOutH);
    EXPECT_THROW(clip.prepare(), std::runtime_error);
}
