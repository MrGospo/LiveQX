#include <gtest/gtest.h>
#include "render/CpuCompositor.h"
#include "transitions/CrossFade.h"

// ─── helpers ─────────────────────────────────────────────────────────────────

static Frame makeFrame(int w, int h,
                       uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
    Frame f;
    f.width  = w;
    f.height = h;
    f.data   = std::make_shared<uint8_t[]>(
        static_cast<size_t>(w) * h * 4);
    for (int i = 0; i < w * h; ++i) {
        f.data[i * 4 + 0] = r;
        f.data[i * 4 + 1] = g;
        f.data[i * 4 + 2] = b;
        f.data[i * 4 + 3] = a;
    }
    return f;
}

static void checkAllPixels(const Frame& f,
                            uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
    ASSERT_TRUE(f.valid());
    for (int i = 0; i < f.width * f.height; ++i) {
        EXPECT_EQ(f.pixels()[i * 4 + 0], r) << "R differs at pixel " << i;
        EXPECT_EQ(f.pixels()[i * 4 + 1], g) << "G differs at pixel " << i;
        EXPECT_EQ(f.pixels()[i * 4 + 2], b) << "B differs at pixel " << i;
        EXPECT_EQ(f.pixels()[i * 4 + 3], a) << "A differs at pixel " << i;
    }
}

// ─── CrossFade unit tests ─────────────────────────────────────────────────────

TEST(CrossFadeTest, HalfwayRedBlueGivesPurple) {
    // progress=0.5: alpha=128, beta=128
    // R: (255*128 + 0*128)>>8 = 127
    // B: (0*128 + 255*128)>>8 = 127
    auto red  = makeFrame(4, 4, 255, 0, 0);
    auto blue = makeFrame(4, 4,   0, 0, 255);

    Frame out;
    out.width  = 4;
    out.height = 4;
    out.data   = std::make_shared<uint8_t[]>(4 * 4 * 4);

    CrossFade cf;
    cf.apply(red, blue, out, 0.5f);

    checkAllPixels(out, 127, 0, 127);
}

TEST(CrossFadeTest, Progress0ReturnsA) {
    auto red  = makeFrame(2, 2, 255, 0, 0);
    auto blue = makeFrame(2, 2,   0, 0, 255);

    Frame out;
    out.width  = 2;
    out.height = 2;
    out.data   = std::make_shared<uint8_t[]>(2 * 2 * 4);

    CrossFade cf;
    cf.apply(red, blue, out, 0.0f);

    // alpha=0, beta=256 → out = (a*256)>>8 = a (for a≤255, (a*256)>>8 = a)
    checkAllPixels(out, 255, 0, 0);
}

TEST(CrossFadeTest, Progress1ReturnsB) {
    auto red  = makeFrame(2, 2, 255, 0, 0);
    auto blue = makeFrame(2, 2,   0, 0, 255);

    Frame out;
    out.width  = 2;
    out.height = 2;
    out.data   = std::make_shared<uint8_t[]>(2 * 2 * 4);

    CrossFade cf;
    cf.apply(red, blue, out, 1.0f);

    // alpha=256, beta=0 → out = (b*256)>>8 = b
    checkAllPixels(out, 0, 0, 255);
}

TEST(CrossFadeTest, InvalidFrameNoCrash) {
    Frame valid = makeFrame(2, 2, 100, 100, 100);
    Frame inv;   // data == nullptr

    Frame out;
    out.width  = 2;
    out.height = 2;
    out.data   = std::make_shared<uint8_t[]>(2 * 2 * 4);

    CrossFade cf;
    cf.apply(inv, valid, out, 0.5f); // must not crash, out unchanged
    cf.apply(valid, inv, out, 0.5f);
}

// ─── CpuCompositor tests ──────────────────────────────────────────────────────

class CompositorTest : public ::testing::Test {
protected:
    CpuCompositor comp_;
    Frame red_  = makeFrame(4, 4, 255, 0, 0);
    Frame blue_ = makeFrame(4, 4,   0, 0, 255);
};

TEST_F(CompositorTest, HardCutReturnsA) {
    auto out = comp_.composite(red_, blue_, TransitionType::HardCut, 0.5f);
    // HardCut → returns A as-is (zero-copy shared_ptr)
    EXPECT_EQ(out.pixels(), red_.pixels());
}

TEST_F(CompositorTest, Progress0ReturnsA) {
    auto out = comp_.composite(red_, blue_, TransitionType::CrossFade, 0.0f);
    EXPECT_EQ(out.pixels(), red_.pixels());
}

TEST_F(CompositorTest, Progress1ReturnsB) {
    auto out = comp_.composite(red_, blue_, TransitionType::CrossFade, 1.0f);
    EXPECT_EQ(out.pixels(), blue_.pixels());
}

TEST_F(CompositorTest, CrossFadeAt50PercentGivesPurple) {
    auto out = comp_.composite(red_, blue_, TransitionType::CrossFade, 0.5f);
    ASSERT_TRUE(out.valid());
    EXPECT_EQ(out.width,  4);
    EXPECT_EQ(out.height, 4);
    checkAllPixels(out, 127, 0, 127);
}

TEST_F(CompositorTest, InvalidAReturnsB) {
    Frame inv;
    auto out = comp_.composite(inv, blue_, TransitionType::CrossFade, 0.5f);
    EXPECT_EQ(out.pixels(), blue_.pixels());
}

TEST_F(CompositorTest, InvalidBReturnsA) {
    Frame inv;
    auto out = comp_.composite(red_, inv, TransitionType::CrossFade, 0.5f);
    EXPECT_EQ(out.pixels(), red_.pixels());
}

TEST_F(CompositorTest, OutputHasSameDimensionsAsA) {
    auto out = comp_.composite(red_, blue_, TransitionType::CrossFade, 0.3f);
    EXPECT_EQ(out.width,  red_.width);
    EXPECT_EQ(out.height, red_.height);
}

// ─── Wipe direction sanity checks ─────────────────────────────────────────────

TEST_F(CompositorTest, WipeLeftProgressHalfSplitsColumns) {
    // Left half should be blue (B), right half should be red (A)
    auto out = comp_.composite(red_, blue_, TransitionType::WipeLeft, 0.5f);
    ASSERT_TRUE(out.valid());

    const int half = out.width / 2; // = 2
    for (int y = 0; y < out.height; ++y) {
        for (int x = 0; x < half; ++x) {
            // left side → B (blue)
            EXPECT_EQ(out.pixels()[(y * out.width + x) * 4 + 2], 255u)
                << "B channel at (" << x << "," << y << ")";
        }
        for (int x = half; x < out.width; ++x) {
            // right side → A (red)
            EXPECT_EQ(out.pixels()[(y * out.width + x) * 4 + 0], 255u)
                << "R channel at (" << x << "," << y << ")";
        }
    }
}

TEST_F(CompositorTest, WipeDownProgressHalfSplitsRows) {
    // Top half should be blue (B), bottom half should be red (A)
    auto out = comp_.composite(red_, blue_, TransitionType::WipeDown, 0.5f);
    ASSERT_TRUE(out.valid());

    const int half = out.height / 2; // = 2
    for (int y = 0; y < half; ++y)
        EXPECT_EQ(out.pixels()[(y * out.width) * 4 + 2], 255u) << "top row " << y;
    for (int y = half; y < out.height; ++y)
        EXPECT_EQ(out.pixels()[(y * out.width) * 4 + 0], 255u) << "bottom row " << y;
}

// ─── FadeToBlack (fix13 c7) ──────────────────────────────────────────────────

TEST_F(CompositorTest, FadeToBlackMidpointIsBlack) {
    // progress=0.5 — both halves at the boundary, every channel should
    // collapse to (or be on the brink of) zero.
    auto out = comp_.composite(red_, blue_, TransitionType::FadeToBlack, 0.5f);
    ASSERT_TRUE(out.valid());
    for (int i = 0; i < out.width * out.height; ++i) {
        EXPECT_EQ(out.pixels()[i * 4 + 0], 0u) << "R at " << i;
        EXPECT_EQ(out.pixels()[i * 4 + 1], 0u) << "G at " << i;
        EXPECT_EQ(out.pixels()[i * 4 + 2], 0u) << "B at " << i;
        EXPECT_EQ(out.pixels()[i * 4 + 3], 0u) << "A at " << i;
    }
}

TEST_F(CompositorTest, FadeToBlackQuarterFadesAOut) {
    // progress=0.25 — first half of the curve, A at 50% intensity.
    // R: (255 * 128) >> 8 = 127. B side gone entirely.
    auto out = comp_.composite(red_, blue_, TransitionType::FadeToBlack, 0.25f);
    ASSERT_TRUE(out.valid());
    checkAllPixels(out, 127, 0, 0, 127);
}

TEST_F(CompositorTest, FadeToBlackThreeQuartersFadesBIn) {
    // progress=0.75 — second half of the curve, B at 50% intensity.
    // B: (255 * 128) >> 8 = 127. A side gone.
    auto out = comp_.composite(red_, blue_, TransitionType::FadeToBlack, 0.75f);
    ASSERT_TRUE(out.valid());
    checkAllPixels(out, 0, 0, 127, 127);
}

TEST_F(CompositorTest, FadeToBlackEdges) {
    auto out0 = comp_.composite(red_, blue_, TransitionType::FadeToBlack, 0.0f);
    EXPECT_EQ(out0.pixels(), red_.pixels());
    auto out1 = comp_.composite(red_, blue_, TransitionType::FadeToBlack, 1.0f);
    EXPECT_EQ(out1.pixels(), blue_.pixels());
}

TEST_F(CompositorTest, FadeToBlackRunsEvenWithInvalidA) {
    // Live boundary case: LiveClip Lost, no fallback installed → A is
    // invalid. Other transitions short-circuit to B; FadeToBlack must
    // still fade properly so the cut feels like a feed switch.
    Frame inv;
    auto out = comp_.composite(inv, blue_, TransitionType::FadeToBlack, 0.25f);
    ASSERT_TRUE(out.valid());
    // First half: A is invalid → output is solid black.
    for (int i = 0; i < out.width * out.height; ++i) {
        EXPECT_EQ(out.pixels()[i * 4 + 0], 0u);
        EXPECT_EQ(out.pixels()[i * 4 + 2], 0u);
    }
}

TEST_F(CompositorTest, FadeToBlackRunsEvenWithInvalidB) {
    Frame inv;
    auto out = comp_.composite(red_, inv, TransitionType::FadeToBlack, 0.75f);
    ASSERT_TRUE(out.valid());
    // Second half: B is invalid → output is solid black.
    for (int i = 0; i < out.width * out.height; ++i) {
        EXPECT_EQ(out.pixels()[i * 4 + 0], 0u);
        EXPECT_EQ(out.pixels()[i * 4 + 2], 0u);
    }
}

// ─── Push direction pixel-perfect checks (fix20) ─────────────────────────────
//
// Push slides both clips in lockstep (no reveal split like Wipe). For a 4×4
// frame at progress=0.5 the slide offset is exactly 2 px — at that precise
// boundary every column/row of the output is purely A or purely B with no
// per-pixel blending.

TEST_F(CompositorTest, PushLeftHalfAOnLeftBOnRight) {
    // PushLeft: A slides out left, B slides in from right.
    // off=2 → out[col<2] = A[col+2] (red), out[col>=2] = B[col-2] (blue).
    auto out = comp_.composite(red_, blue_, TransitionType::PushLeft, 0.5f);
    ASSERT_TRUE(out.valid());
    const int half = out.width / 2;
    for (int y = 0; y < out.height; ++y) {
        for (int x = 0; x < half; ++x) {
            EXPECT_EQ(out.pixels()[(y * out.width + x) * 4 + 0], 255u)
                << "left half should be red at (" << x << "," << y << ")";
        }
        for (int x = half; x < out.width; ++x) {
            EXPECT_EQ(out.pixels()[(y * out.width + x) * 4 + 2], 255u)
                << "right half should be blue at (" << x << "," << y << ")";
        }
    }
}

TEST_F(CompositorTest, PushRightHalfBOnLeftAOnRight) {
    // PushRight: A slides out right, B slides in from left.
    // off=2 → out[col<2] = B[col+(w-off)=col+2] (blue),
    //          out[col>=2] = A[col-off=col-2] (red).
    auto out = comp_.composite(red_, blue_, TransitionType::PushRight, 0.5f);
    ASSERT_TRUE(out.valid());
    const int half = out.width / 2;
    for (int y = 0; y < out.height; ++y) {
        for (int x = 0; x < half; ++x) {
            EXPECT_EQ(out.pixels()[(y * out.width + x) * 4 + 2], 255u)
                << "left half should be blue at (" << x << "," << y << ")";
        }
        for (int x = half; x < out.width; ++x) {
            EXPECT_EQ(out.pixels()[(y * out.width + x) * 4 + 0], 255u)
                << "right half should be red at (" << x << "," << y << ")";
        }
    }
}

TEST_F(CompositorTest, PushUpHalfATopBBottom) {
    // PushUp: A slides up, B slides in from bottom.
    // off=2 → out[row<h-off=2] = A[row+2] (red), out[row>=2] = B (blue).
    auto out = comp_.composite(red_, blue_, TransitionType::PushUp, 0.5f);
    ASSERT_TRUE(out.valid());
    const int half = out.height / 2;
    for (int y = 0; y < half; ++y) {
        for (int x = 0; x < out.width; ++x) {
            EXPECT_EQ(out.pixels()[(y * out.width + x) * 4 + 0], 255u)
                << "top half should be red at (" << x << "," << y << ")";
        }
    }
    for (int y = half; y < out.height; ++y) {
        for (int x = 0; x < out.width; ++x) {
            EXPECT_EQ(out.pixels()[(y * out.width + x) * 4 + 2], 255u)
                << "bottom half should be blue at (" << x << "," << y << ")";
        }
    }
}

TEST_F(CompositorTest, PushDownHalfBTopABottom) {
    // PushDown: A slides down, B slides in from top.
    // off=2 → out[row<2] = B[row+(h-off)=row+2] (blue),
    //          out[row>=2] = A[row-off=row-2] (red).
    auto out = comp_.composite(red_, blue_, TransitionType::PushDown, 0.5f);
    ASSERT_TRUE(out.valid());
    const int half = out.height / 2;
    for (int y = 0; y < half; ++y) {
        for (int x = 0; x < out.width; ++x) {
            EXPECT_EQ(out.pixels()[(y * out.width + x) * 4 + 2], 255u)
                << "top half should be blue at (" << x << "," << y << ")";
        }
    }
    for (int y = half; y < out.height; ++y) {
        for (int x = 0; x < out.width; ++x) {
            EXPECT_EQ(out.pixels()[(y * out.width + x) * 4 + 0], 255u)
                << "bottom half should be red at (" << x << "," << y << ")";
        }
    }
}

TEST_F(CompositorTest, PushProgress0ReturnsA) {
    // At progress=0 the compositor short-circuits to A regardless of direction.
    for (auto t : {TransitionType::PushLeft, TransitionType::PushRight,
                   TransitionType::PushUp,   TransitionType::PushDown}) {
        auto out = comp_.composite(red_, blue_, t, 0.0f);
        EXPECT_EQ(out.pixels(), red_.pixels()) << "direction " << static_cast<int>(t);
    }
}

TEST_F(CompositorTest, PushProgress1ReturnsB) {
    for (auto t : {TransitionType::PushLeft, TransitionType::PushRight,
                   TransitionType::PushUp,   TransitionType::PushDown}) {
        auto out = comp_.composite(red_, blue_, t, 1.0f);
        EXPECT_EQ(out.pixels(), blue_.pixels()) << "direction " << static_cast<int>(t);
    }
}

// ─── Easing integration through CpuCompositor (fix20) ───────────────────────
//
// Easing is applied generically before the transition runs. CrossFade is
// the easiest victim to verify: at progress=0.5 with Linear we get the
// 127/127 purple midpoint (already covered above); with EaseIn the eased
// progress is 0.25 → A still dominates; with EaseOut it's 0.75 → B
// dominates. The numeric expectations come from CrossFade's fixed-point
// formula: alpha = round(eased * 256), out = (a*alpha_out + b*alpha)>>8
// where alpha_out = 256 - alpha.

TEST_F(CompositorTest, EasingDefaultPreservesLinearMidpoint) {
    // No easing argument → defaults to Linear → identical to the existing
    // CrossFadeAt50PercentGivesPurple test. Guards the default-arg contract.
    auto out = comp_.composite(red_, blue_, TransitionType::CrossFade, 0.5f);
    ASSERT_TRUE(out.valid());
    checkAllPixels(out, 127, 0, 127);
}

TEST_F(CompositorTest, EasingEaseInShiftsTowardA) {
    // EaseIn: f(0.5) = 0.25 → A weighs ~75%, B ~25%.
    auto out = comp_.composite(red_, blue_, TransitionType::CrossFade, 0.5f,
                                Easing::EaseIn);
    ASSERT_TRUE(out.valid());
    // R channel from A=255 should clearly dominate B=0; B channel from
    // B=255 should be small. We allow ±2 LSB for fixed-point rounding.
    const uint8_t r = out.pixels()[0];
    const uint8_t b = out.pixels()[2];
    EXPECT_GT(r, 180u) << "A should dominate, R=" << static_cast<int>(r);
    EXPECT_LT(b, 80u)  << "B should be light, B=" << static_cast<int>(b);
}

TEST_F(CompositorTest, EasingEaseOutShiftsTowardB) {
    // EaseOut: f(0.5) = 0.75 → A ~25%, B ~75%. Mirror of EaseIn.
    auto out = comp_.composite(red_, blue_, TransitionType::CrossFade, 0.5f,
                                Easing::EaseOut);
    ASSERT_TRUE(out.valid());
    const uint8_t r = out.pixels()[0];
    const uint8_t b = out.pixels()[2];
    EXPECT_LT(r, 80u)  << "A should be light, R=" << static_cast<int>(r);
    EXPECT_GT(b, 180u) << "B should dominate, B=" << static_cast<int>(b);
}

TEST_F(CompositorTest, EasingEaseInOutCrossesAtMidpoint) {
    // EaseInOut: f(0.5) = 0.5 (smoothstep is symmetric) → same purple as Linear.
    auto out = comp_.composite(red_, blue_, TransitionType::CrossFade, 0.5f,
                                Easing::EaseInOut);
    ASSERT_TRUE(out.valid());
    checkAllPixels(out, 127, 0, 127);
}

TEST_F(CompositorTest, EasingPreservesEndpoints) {
    // Endpoints must remain exact regardless of curve — Compositor's
    // progress<=0/>=1 short-circuit relies on this.
    for (auto e : {Easing::EaseIn, Easing::EaseOut, Easing::EaseInOut}) {
        auto out0 = comp_.composite(red_, blue_, TransitionType::CrossFade, 0.0f, e);
        EXPECT_EQ(out0.pixels(), red_.pixels())  << "easing=" << static_cast<int>(e);
        auto out1 = comp_.composite(red_, blue_, TransitionType::CrossFade, 1.0f, e);
        EXPECT_EQ(out1.pixels(), blue_.pixels()) << "easing=" << static_cast<int>(e);
    }
}

TEST_F(CompositorTest, PushInvalidFrameSkipsApply) {
    // Push has no synthesized mid-frame (unlike FadeToBlack), so an invalid
    // side must short-circuit the same way other transitions do.
    Frame inv;
    auto outA = comp_.composite(inv, blue_, TransitionType::PushLeft, 0.5f);
    EXPECT_EQ(outA.pixels(), blue_.pixels());
    auto outB = comp_.composite(red_, inv, TransitionType::PushUp, 0.5f);
    EXPECT_EQ(outB.pixels(), red_.pixels());
}
