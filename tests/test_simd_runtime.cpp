// fix28 — SimdRuntime config parse + mode selection.
//
// We can't unit-test the AVX2-vs-scalar dispatch from here (that's covered
// by test_crossfade.cpp + test_audio_mixer.cpp), but we CAN verify:
//  - parse() accepts auto/avx2/scalar (case-insensitive),
//  - empty/unknown values fall through to "auto",
//  - "avx2" force gracefully degrades on no-AVX2 hosts,
//  - "scalar" force always sticks (doesn't read CPU).
// Tests run on any x86_64 host. They also cover the avx2Available()
// caching boundary — calling repeatedly must stay consistent.

#include <gtest/gtest.h>

#include "utils/SimdRuntime.h"

namespace simd = liveqx::simd;

TEST(SimdRuntime, AvailabilityIsStable) {
    // First and second calls must agree — we cache cpuid result behind
    // a static. A flapping return would break dispatch consistency.
    EXPECT_EQ(simd::avx2Available(), simd::avx2Available());
}

TEST(SimdRuntime, ScalarConfigForcesScalar) {
    simd::initFromConfig("scalar");
    EXPECT_EQ(simd::current(), simd::Mode::Scalar);
    // Different case should also work.
    simd::initFromConfig("SCALAR");
    EXPECT_EQ(simd::current(), simd::Mode::Scalar);
}

TEST(SimdRuntime, AutoMatchesCpu) {
    simd::initFromConfig("auto");
    if (simd::avx2Available()) {
        EXPECT_EQ(simd::current(), simd::Mode::Avx2);
    } else {
        EXPECT_EQ(simd::current(), simd::Mode::Scalar);
    }
}

TEST(SimdRuntime, EmptyTreatedAsAuto) {
    simd::initFromConfig("");
    if (simd::avx2Available()) {
        EXPECT_EQ(simd::current(), simd::Mode::Avx2);
    } else {
        EXPECT_EQ(simd::current(), simd::Mode::Scalar);
    }
}

TEST(SimdRuntime, UnknownTreatedAsAuto) {
    simd::initFromConfig("sse99");  // gibberish, must not crash
    if (simd::avx2Available()) {
        EXPECT_EQ(simd::current(), simd::Mode::Avx2);
    } else {
        EXPECT_EQ(simd::current(), simd::Mode::Scalar);
    }
}

TEST(SimdRuntime, Avx2ForceFallsBackOnNoAvx2) {
    simd::initFromConfig("avx2");
    if (simd::avx2Available()) {
        EXPECT_EQ(simd::current(), simd::Mode::Avx2);
    } else {
        // Graceful degrade — test environment without AVX2 must not crash
        // on "avx2" force, just warn + scalar.
        EXPECT_EQ(simd::current(), simd::Mode::Scalar);
    }
}

TEST(SimdRuntime, ModeNameStringification) {
    EXPECT_STREQ(simd::modeName(simd::Mode::Scalar), "scalar");
    EXPECT_STREQ(simd::modeName(simd::Mode::Avx2),   "avx2");
}
