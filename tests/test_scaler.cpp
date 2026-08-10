#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "scaler/Scaler.h"

// ─── helpers ─────────────────────────────────────────────────────────────────

static Frame solidFrame(int w, int h,
                         uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
    Frame f;
    f.width  = w;
    f.height = h;
    const size_t sz = static_cast<size_t>(w) * h * 4;
    f.data = std::make_shared<uint8_t[]>(sz);
    for (size_t i = 0; i < sz / 4; ++i) {
        f.data[i * 4 + 0] = r;
        f.data[i * 4 + 1] = g;
        f.data[i * 4 + 2] = b;
        f.data[i * 4 + 3] = a;
    }
    return f;
}

// Returns RGBA of pixel (x, y) in frame f.
static std::array<uint8_t, 4> px(const Frame& f, int x, int y) {
    const uint8_t* p = f.pixels() + (y * f.width + x) * 4;
    return { p[0], p[1], p[2], p[3] };
}

static bool isBlack(const Frame& f, int x, int y) {
    auto c = px(f, x, y);
    return c[0] == 0 && c[1] == 0 && c[2] == 0;
}

// ─── Stretch ─────────────────────────────────────────────────────────────────

TEST(ScalerStretch, OutputDimensions) {
    Scaler s(64, 48, ScaleMode::Stretch);
    auto out = s.scale(solidFrame(16, 16, 200, 100, 50));
    EXPECT_TRUE(out.valid());
    EXPECT_EQ(out.width,  64);
    EXPECT_EQ(out.height, 48);
    EXPECT_EQ(out.sizeBytes(), static_cast<size_t>(64) * 48 * 4u);
}

TEST(ScalerStretch, SolidColorPreserved) {
    // Solid red → stretch → should still be all red.
    Scaler s(32, 32, ScaleMode::Stretch);
    auto out = s.scale(solidFrame(16, 16, 255, 0, 0));
    ASSERT_TRUE(out.valid());
    // sws_scale with bilinear: interior pixels should be exactly red.
    auto c = px(out, 16, 16);
    EXPECT_EQ(c[0], 255u);
    EXPECT_EQ(c[1], 0u);
    EXPECT_EQ(c[2], 0u);
}

TEST(ScalerStretch, PtsPreserved) {
    Frame in = solidFrame(16, 16, 0, 255, 0);
    in.pts = 123456;
    Scaler s(32, 32, ScaleMode::Stretch);
    EXPECT_EQ(s.scale(in).pts, 123456);
}

// ─── Fit (letterbox / pillarbox) ─────────────────────────────────────────────

// Input 16×8 (2:1), output 32×32 (1:1).
// Scale = min(32/16, 32/8) = min(2, 4) = 2 → scaled 32×16.
// off_y = (32-16)/2 = 8.  Rows 0-7 and 24-31 must be black.
TEST(ScalerFit, LetterboxBlackBarsTopBottom) {
    Scaler s(32, 32, ScaleMode::Fit);
    auto out = s.scale(solidFrame(16, 8, 255, 0, 0));
    ASSERT_TRUE(out.valid());
    EXPECT_EQ(out.width,  32);
    EXPECT_EQ(out.height, 32);

    // Top bar (rows 0..7) must be black.
    for (int y = 0; y < 8; ++y)
        EXPECT_TRUE(isBlack(out, 16, y)) << "top bar row " << y;

    // Bottom bar (rows 24..31) must be black.
    for (int y = 24; y < 32; ++y)
        EXPECT_TRUE(isBlack(out, 16, y)) << "bottom bar row " << y;

    // Middle rows must have content (not black).
    auto mid = px(out, 16, 16);
    EXPECT_GT(mid[0] + mid[1] + mid[2], 0u) << "middle row is unexpectedly black";
}

// Input 8×16 (1:2), output 32×32 (1:1).
// Scale = min(32/8, 32/16) = min(4, 2) = 2 → scaled 16×32.
// off_x = (32-16)/2 = 8.  Columns 0-7 and 24-31 must be black.
TEST(ScalerFit, PillarboxBlackBarsSides) {
    Scaler s(32, 32, ScaleMode::Fit);
    auto out = s.scale(solidFrame(8, 16, 0, 0, 255));
    ASSERT_TRUE(out.valid());

    for (int x = 0; x < 8; ++x)
        EXPECT_TRUE(isBlack(out, x, 16)) << "left bar col " << x;

    for (int x = 24; x < 32; ++x)
        EXPECT_TRUE(isBlack(out, x, 16)) << "right bar col " << x;

    auto mid = px(out, 16, 16);
    EXPECT_GT(mid[0] + mid[1] + mid[2], 0u) << "centre is unexpectedly black";
}

// Same aspect → no bars.
TEST(ScalerFit, SameAspectNoBlackBars) {
    Scaler s(32, 32, ScaleMode::Fit);
    auto out = s.scale(solidFrame(16, 16, 0, 200, 0));
    ASSERT_TRUE(out.valid());

    // No border pixel should be black.
    EXPECT_FALSE(isBlack(out, 0,  0));
    EXPECT_FALSE(isBlack(out, 31, 31));
}

// ─── Fill (crop, no black bars) ───────────────────────────────────────────────

// Input 16×8 (2:1), output 32×32 (1:1).
// Scale = max(32/16, 32/8) = max(2, 4) = 4 → scaled 64×32; crop x by 16 each side.
// All output pixels should have content (no black).
TEST(ScalerFill, NoBlackBars_WideInput) {
    Scaler s(32, 32, ScaleMode::Fill);
    auto out = s.scale(solidFrame(16, 8, 255, 128, 0));
    ASSERT_TRUE(out.valid());
    EXPECT_EQ(out.width,  32);
    EXPECT_EQ(out.height, 32);

    // Corner pixels must not be black.
    for (auto [x, y] : std::vector<std::pair<int,int>>{{0,0},{31,0},{0,31},{31,31}}) {
        EXPECT_FALSE(isBlack(out, x, y))
            << "corner (" << x << "," << y << ") is black";
    }
}

// Input 8×16 (1:2), output 32×32 (1:1).
TEST(ScalerFill, NoBlackBars_TallInput) {
    Scaler s(32, 32, ScaleMode::Fill);
    auto out = s.scale(solidFrame(8, 16, 100, 200, 50));
    ASSERT_TRUE(out.valid());

    for (auto [x, y] : std::vector<std::pair<int,int>>{{0,0},{31,0},{0,31},{31,31}}) {
        EXPECT_FALSE(isBlack(out, x, y))
            << "corner (" << x << "," << y << ") is black";
    }
}

// ─── Edge cases ───────────────────────────────────────────────────────────────

TEST(ScalerEdge, InvalidInputReturnsEmpty) {
    Scaler s(64, 48, ScaleMode::Stretch);
    Frame inv;
    auto out = s.scale(inv);
    EXPECT_FALSE(out.valid());
}

TEST(ScalerEdge, ScaleModeFromString) {
    EXPECT_EQ(scaleModeFromString("fit"),     ScaleMode::Fit);
    EXPECT_EQ(scaleModeFromString("fill"),    ScaleMode::Fill);
    EXPECT_EQ(scaleModeFromString("stretch"), ScaleMode::Stretch);
    EXPECT_EQ(scaleModeFromString("unknown"), ScaleMode::Fit); // default
}

TEST(ScalerEdge, CacheReuseDoesNotCrash) {
    // Call scale() multiple times with the same input dimensions to exercise
    // the SwsContext cache hit path.
    Scaler s(32, 32, ScaleMode::Stretch);
    auto in = solidFrame(16, 16, 128, 128, 128);
    for (int i = 0; i < 5; ++i) {
        auto out = s.scale(in);
        EXPECT_TRUE(out.valid());
    }
}
