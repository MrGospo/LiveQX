// fix28 — bit-identical scalar vs AVX2 AudioMixer crossfade.
//
// The scalar lerp `sa*(1-p) + sb*p` must produce floats equal to the
// AVX2 path bit-for-bit because we mirror the scalar instruction order
// (mul, mul, add — no FMADD). We use memcmp on the raw float bytes to
// flush out any accidental FMA-rounding drift.
//
// Edge cases covered:
//  - Equal-length frames (no zero-padding path).
//  - a shorter than b (zero-pad past a_n).
//  - b shorter than a (zero-pad past b_n).
//  - Non-multiple-of-8 length → scalar tail.
//  - progress=0/1 (one side dominates).

#include <gtest/gtest.h>
#include <cstring>
#include <random>

#include "audio/AudioMixer.h"
#include "utils/SimdRuntime.h"

namespace simd = liveqx::simd;

namespace {

AudioFrame makeRandomAudio(int num_samples, int channels, std::uint32_t seed) {
    AudioFrame f;
    f.sample_rate = 48000;
    f.channels    = channels;
    f.num_samples = num_samples;
    f.samples.resize(static_cast<std::size_t>(num_samples) * channels);
    f.valid = true;
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    for (auto& s : f.samples) s = dist(rng);
    return f;
}

AudioFrame mixWithMode(simd::Mode mode,
                       const AudioFrame& a, const AudioFrame& b, float p) {
    if (mode == simd::Mode::Avx2) simd::initFromConfig("avx2");
    else                          simd::initFromConfig("scalar");
    AudioMixer mixer;
    return mixer.crossfade(a, b, p);
}

// Bit-exact compare on the float buffer.
bool bitwiseEqual(const std::vector<float>& x, const std::vector<float>& y) {
    if (x.size() != y.size()) return false;
    return std::memcmp(x.data(), y.data(),
                       x.size() * sizeof(float)) == 0;
}

}  // namespace

class AudioMixerSimdTest : public ::testing::Test {
protected:
    void TearDown() override { simd::initFromConfig("auto"); }
};

TEST_F(AudioMixerSimdTest, ScalarMatchesAvx2EqualLength) {
    if (!simd::avx2Available()) GTEST_SKIP() << "host has no AVX2";

    // 1024 samples × 2 channels = 2048 floats — multiple of 8, no tail.
    const auto a = makeRandomAudio(1024, 2, 1);
    const auto b = makeRandomAudio(1024, 2, 2);

    for (float p : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f, 0.137f, 0.999f}) {
        const auto sc = mixWithMode(simd::Mode::Scalar, a, b, p);
        const auto av = mixWithMode(simd::Mode::Avx2,   a, b, p);
        EXPECT_TRUE(bitwiseEqual(sc.samples, av.samples))
            << "mismatch at progress=" << p;
    }
}

TEST_F(AudioMixerSimdTest, ScalarMatchesAvx2NonMultipleOf8) {
    if (!simd::avx2Available()) GTEST_SKIP() << "host has no AVX2";

    // 17 samples × 1 channel = 17 floats → 2 vectors + 1-lane tail.
    const auto a = makeRandomAudio(17, 1, 7);
    const auto b = makeRandomAudio(17, 1, 11);

    for (float p : {0.0f, 0.5f, 1.0f, 0.42f}) {
        const auto sc = mixWithMode(simd::Mode::Scalar, a, b, p);
        const auto av = mixWithMode(simd::Mode::Avx2,   a, b, p);
        EXPECT_TRUE(bitwiseEqual(sc.samples, av.samples))
            << "tail mismatch at progress=" << p;
    }
}

TEST_F(AudioMixerSimdTest, ScalarMatchesAvx2AShorterThanB) {
    if (!simd::avx2Available()) GTEST_SKIP() << "host has no AVX2";

    // a is shorter — past a_n the scalar path zero-pads. AVX2 must
    // fall back to scalar tail at that boundary and produce identical
    // bytes.
    const auto a = makeRandomAudio(13, 2, 3);   // 26 floats
    const auto b = makeRandomAudio(64, 2, 4);   // 128 floats

    for (float p : {0.5f, 0.137f}) {
        const auto sc = mixWithMode(simd::Mode::Scalar, a, b, p);
        const auto av = mixWithMode(simd::Mode::Avx2,   a, b, p);
        EXPECT_TRUE(bitwiseEqual(sc.samples, av.samples))
            << "padding mismatch at progress=" << p;
    }
}

TEST_F(AudioMixerSimdTest, ScalarMatchesAvx2BShorterThanA) {
    if (!simd::avx2Available()) GTEST_SKIP() << "host has no AVX2";

    const auto a = makeRandomAudio(64, 2, 5);
    const auto b = makeRandomAudio(13, 2, 6);

    for (float p : {0.5f, 0.137f}) {
        const auto sc = mixWithMode(simd::Mode::Scalar, a, b, p);
        const auto av = mixWithMode(simd::Mode::Avx2,   a, b, p);
        EXPECT_TRUE(bitwiseEqual(sc.samples, av.samples))
            << "padding mismatch at progress=" << p;
    }
}

TEST_F(AudioMixerSimdTest, EndpointsReturnInputs) {
    if (!simd::avx2Available()) GTEST_SKIP() << "host has no AVX2";

    const auto a = makeRandomAudio(32, 1, 100);
    const auto b = makeRandomAudio(32, 1, 200);

    // p=0 → out == a (within bit-equality)
    const auto out0 = mixWithMode(simd::Mode::Avx2, a, b, 0.0f);
    EXPECT_TRUE(bitwiseEqual(out0.samples, a.samples));

    // p=1 → out == b
    const auto out1 = mixWithMode(simd::Mode::Avx2, a, b, 1.0f);
    EXPECT_TRUE(bitwiseEqual(out1.samples, b.samples));
}
