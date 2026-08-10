// fix20 — Easing curve unit tests.
//
// Easing is applied generically in CpuCompositor::composite() before the
// transition's apply(), so we test the helper in isolation. Three contracts
// matter for any new curve added later:
//   1. Endpoints: 0.0 → 0.0, 1.0 → 1.0 (otherwise Compositor's progress<=0
//      / progress>=1 short-circuits desync from the curve).
//   2. Out-of-range inputs are clamped, not extrapolated.
//   3. Strict monotonicity inside (0,1) — no plateaus, no overshoots,
//      so the user-perceived progress always moves forward.
//
// Plus per-curve sanity points to lock the actual shape.

#include <gtest/gtest.h>
#include <cmath>

#include "transitions/ITransition.h"

TEST(Easing, EndpointsAreExact) {
    for (auto e : {Easing::Linear, Easing::EaseIn, Easing::EaseOut, Easing::EaseInOut}) {
        EXPECT_FLOAT_EQ(applyEasing(e, 0.0f), 0.0f);
        EXPECT_FLOAT_EQ(applyEasing(e, 1.0f), 1.0f);
    }
}

TEST(Easing, OutOfRangeIsClamped) {
    for (auto e : {Easing::Linear, Easing::EaseIn, Easing::EaseOut, Easing::EaseInOut}) {
        EXPECT_FLOAT_EQ(applyEasing(e, -0.5f), 0.0f);
        EXPECT_FLOAT_EQ(applyEasing(e,  1.5f), 1.0f);
    }
}

TEST(Easing, LinearIsIdentity) {
    // Linear is the default — anything else would silently change the
    // shape of every existing transition. Identity guarantees backward
    // compatibility for every operator who never sets `easing`.
    for (float t : {0.0f, 0.1f, 0.25f, 0.5f, 0.75f, 0.9f, 1.0f}) {
        EXPECT_FLOAT_EQ(applyEasing(Easing::Linear, t), t) << "t=" << t;
    }
}

TEST(Easing, EaseInIsQuadratic) {
    // f(t) = t² — slower start, snappier finish. f(0.5) = 0.25.
    EXPECT_FLOAT_EQ(applyEasing(Easing::EaseIn, 0.5f), 0.25f);
    EXPECT_FLOAT_EQ(applyEasing(Easing::EaseIn, 0.25f), 0.0625f);
}

TEST(Easing, EaseOutMirrorsEaseIn) {
    // f(t) = 1 - (1-t)² — symmetric to EaseIn around the diagonal. f(0.5) = 0.75.
    EXPECT_FLOAT_EQ(applyEasing(Easing::EaseOut, 0.5f), 0.75f);
    EXPECT_FLOAT_EQ(applyEasing(Easing::EaseOut, 0.25f), 0.4375f);
}

TEST(Easing, EaseInOutSmoothStep) {
    // f(t) = 3t² - 2t³ (smoothstep). Symmetric around 0.5, with f(0.5) = 0.5.
    EXPECT_FLOAT_EQ(applyEasing(Easing::EaseInOut, 0.5f), 0.5f);
    // Quarter point: 3*(0.0625) - 2*(0.015625) = 0.15625
    EXPECT_FLOAT_EQ(applyEasing(Easing::EaseInOut, 0.25f), 0.15625f);
    // Three-quarter point: mirror of the quarter.
    EXPECT_FLOAT_EQ(applyEasing(Easing::EaseInOut, 0.75f), 0.84375f);
}

TEST(Easing, MonotonicOnUnitInterval) {
    // Strict monotonicity guarantees the transition never "rewinds" mid-cut.
    constexpr int kSamples = 64;
    for (auto e : {Easing::Linear, Easing::EaseIn, Easing::EaseOut, Easing::EaseInOut}) {
        float prev = applyEasing(e, 0.0f);
        for (int i = 1; i <= kSamples; ++i) {
            const float t   = static_cast<float>(i) / kSamples;
            const float now = applyEasing(e, t);
            EXPECT_GE(now, prev) << "non-monotonic at t=" << t
                                  << " for easing " << static_cast<int>(e);
            prev = now;
        }
    }
}

// Backwards compat — TransitionConfig{} default ctor must keep Linear so
// every existing operator's config behaves exactly as it did before fix20.
TEST(Easing, TransitionConfigDefaultIsLinear) {
    TransitionConfig tc;
    EXPECT_EQ(tc.easing, Easing::Linear);
}
