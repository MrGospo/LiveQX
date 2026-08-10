// fix28 — pixel-identical scalar vs AVX2 CrossFade.
//
// The scalar formula `(a*beta + b*alpha) >> 8` and the AVX2 formula must
// produce byte-identical output across every progress value. We test:
//  - boundaries (0.0, 1.0) where one of {alpha, beta} is 0/256,
//  - midpoints (0.5, 0.25, 0.75),
//  - non-aligned progress (0.137, 0.999) — flush out rounding bugs,
//  - frame size = 320×240 RGBA = 307200 bytes (multiple of 32, no tail),
//  - frame size = 321×240 RGBA = 308160 bytes (NOT mul of 32 → tail path).
//
// AVX2 path is disabled at the dispatcher level on hosts without AVX2,
// so the test gates on simd::avx2Available() to avoid false negatives
// on CI runners with masked CPU features.

#include <gtest/gtest.h>
#include <cstring>
#include <random>
#include <vector>

#include "core/Frame.h"
#include "transitions/CrossFade.h"
#include "utils/SimdRuntime.h"

namespace simd = liveqx::simd;

namespace {

Frame makeRandomFrame(int w, int h, std::uint32_t seed) {
    Frame f;
    f.width  = w;
    f.height = h;
    const std::size_t n = static_cast<std::size_t>(w) * h * 4;
    f.data = std::make_shared<std::uint8_t[]>(n);
    std::mt19937 rng(seed);
    for (std::size_t i = 0; i < n; ++i)
        f.data[i] = static_cast<std::uint8_t>(rng() & 0xFF);
    return f;
}

Frame makeBlankFrame(int w, int h) {
    Frame f;
    f.width  = w;
    f.height = h;
    f.data = std::make_shared<std::uint8_t[]>(
        static_cast<std::size_t>(w) * h * 4);
    return f;
}

void blendOnceWithMode(simd::Mode mode,
                        const Frame& a, const Frame& b,
                        Frame& out, float progress) {
    // initFromConfig writes the global mode atomically. Tests are
    // single-threaded so no reader can race with the change.
    if (mode == simd::Mode::Avx2) simd::initFromConfig("avx2");
    else                          simd::initFromConfig("scalar");
    CrossFade cf;
    cf.apply(a, b, out, progress);
}

}  // namespace

class CrossFadeSimdTest : public ::testing::Test {
protected:
    void TearDown() override {
        // Reset to "auto" so other tests in the suite don't see a pinned
        // mode bleeding through.
        simd::initFromConfig("auto");
    }
};

TEST_F(CrossFadeSimdTest, ScalarMatchesAvx2OnAlignedFrame) {
    if (!simd::avx2Available()) GTEST_SKIP() << "host has no AVX2";

    const Frame a = makeRandomFrame(320, 240, 42);
    const Frame b = makeRandomFrame(320, 240, 1337);
    Frame scalar_out = makeBlankFrame(320, 240);
    Frame avx2_out   = makeBlankFrame(320, 240);

    for (float p : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f, 0.137f, 0.999f}) {
        blendOnceWithMode(simd::Mode::Scalar, a, b, scalar_out, p);
        blendOnceWithMode(simd::Mode::Avx2,   a, b, avx2_out,   p);
        const std::size_t n = static_cast<std::size_t>(scalar_out.sizeBytes());
        EXPECT_EQ(std::memcmp(scalar_out.pixels(), avx2_out.pixels(), n), 0)
            << "mismatch at progress=" << p;
    }
}

TEST_F(CrossFadeSimdTest, ScalarMatchesAvx2OnUnalignedFrame) {
    if (!simd::avx2Available()) GTEST_SKIP() << "host has no AVX2";

    // 321×240 → 308160 bytes; 308160 % 32 = 0 actually. Use 7×11 to force
    // a small frame whose byte count isn't a multiple of 32.
    // 7 * 11 * 4 = 308 bytes → 308 % 32 = 20 → 9 full vectors + 20-byte tail.
    const Frame a = makeRandomFrame(7, 11, 42);
    const Frame b = makeRandomFrame(7, 11, 1337);
    Frame scalar_out = makeBlankFrame(7, 11);
    Frame avx2_out   = makeBlankFrame(7, 11);

    for (float p : {0.0f, 0.5f, 1.0f, 0.137f}) {
        blendOnceWithMode(simd::Mode::Scalar, a, b, scalar_out, p);
        blendOnceWithMode(simd::Mode::Avx2,   a, b, avx2_out,   p);
        const std::size_t n = static_cast<std::size_t>(scalar_out.sizeBytes());
        EXPECT_EQ(std::memcmp(scalar_out.pixels(), avx2_out.pixels(), n), 0)
            << "tail mismatch at progress=" << p;
    }
}

TEST_F(CrossFadeSimdTest, Avx2EndpointsReturnInputs) {
    if (!simd::avx2Available()) GTEST_SKIP() << "host has no AVX2";

    const Frame a = makeRandomFrame(64, 48, 1);
    const Frame b = makeRandomFrame(64, 48, 2);
    Frame out = makeBlankFrame(64, 48);

    blendOnceWithMode(simd::Mode::Avx2, a, b, out, 0.0f);
    EXPECT_EQ(std::memcmp(out.pixels(), a.pixels(),
                          static_cast<std::size_t>(out.sizeBytes())), 0);

    blendOnceWithMode(simd::Mode::Avx2, a, b, out, 1.0f);
    EXPECT_EQ(std::memcmp(out.pixels(), b.pixels(),
                          static_cast<std::size_t>(out.sizeBytes())), 0);
}

TEST_F(CrossFadeSimdTest, ScalarPathIsAlwaysReachable) {
    // Test that the scalar path itself (no SIMD at all) produces the
    // exact textbook blend (a*beta + b*alpha) >> 8 at progress=0.5.
    simd::initFromConfig("scalar");
    Frame a = makeBlankFrame(2, 1);
    Frame b = makeBlankFrame(2, 1);
    Frame out = makeBlankFrame(2, 1);
    a.pixels()[0] = 100; a.pixels()[4] = 200;
    b.pixels()[0] =  50; b.pixels()[4] = 150;
    CrossFade cf;
    cf.apply(a, b, out, 0.5f);
    // alpha = 128, beta = 128 → (100*128 + 50*128) >> 8 = 75 (exact)
    EXPECT_EQ(out.pixels()[0], 75);
    // (200*128 + 150*128) >> 8 = 175
    EXPECT_EQ(out.pixels()[4], 175);
}
