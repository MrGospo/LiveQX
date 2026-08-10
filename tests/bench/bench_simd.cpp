// fix28 — micro-bench for the two AVX2 hot paths.
//
// Not part of ctest. Standalone executable, run manually:
//   ./liveqx-bench
//
// Reports ms/call for scalar and AVX2 paths and the speedup ratio.
// Sizes are realistic for the engine: 720p RGBA frames (3.5 MB) for
// CrossFade, 1024-sample stereo audio for AudioMixer.
//
// On hosts without AVX2 the AVX2 lines are printed as "n/a" — the
// bench still runs the scalar baseline so operators can capture a
// number for comparison across machines.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#include "audio/AudioMixer.h"
#include "core/Frame.h"
#include "transitions/CrossFade.h"
#include "utils/SimdRuntime.h"

namespace simd = liveqx::simd;
using clk = std::chrono::steady_clock;

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

AudioFrame makeRandomAudio(int n, int channels, std::uint32_t seed) {
    AudioFrame f;
    f.sample_rate = 48000;
    f.channels    = channels;
    f.num_samples = n;
    f.samples.resize(static_cast<std::size_t>(n) * channels);
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    for (auto& s : f.samples) s = dist(rng);
    f.valid = true;
    return f;
}

double benchCrossFade(simd::Mode mode, int iters) {
    if (mode == simd::Mode::Avx2) simd::initFromConfig("avx2");
    else                          simd::initFromConfig("scalar");

    Frame a = makeRandomFrame(1280, 720, 1);
    Frame b = makeRandomFrame(1280, 720, 2);
    Frame out;
    out.width  = 1280;
    out.height = 720;
    out.data = std::make_shared<std::uint8_t[]>(1280 * 720 * 4);

    CrossFade cf;
    cf.apply(a, b, out, 0.5f);  // warm-up

    const auto t0 = clk::now();
    for (int i = 0; i < iters; ++i) {
        cf.apply(a, b, out, 0.42f);
    }
    const auto t1 = clk::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count() / iters;
}

double benchAudioMixer(simd::Mode mode, int iters) {
    if (mode == simd::Mode::Avx2) simd::initFromConfig("avx2");
    else                          simd::initFromConfig("scalar");

    const auto a = makeRandomAudio(1024, 2, 3);
    const auto b = makeRandomAudio(1024, 2, 4);
    AudioMixer mixer;
    (void)mixer.crossfade(a, b, 0.5f);  // warm-up

    const auto t0 = clk::now();
    for (int i = 0; i < iters; ++i) {
        (void)mixer.crossfade(a, b, 0.137f);
    }
    const auto t1 = clk::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count() / iters;
}

}  // namespace

int main() {
    std::printf("liveqx SIMD bench\n");
    std::printf("AVX2 available: %s\n",
                simd::avx2Available() ? "yes" : "no");
    std::printf("\n");

    constexpr int frame_iters = 200;
    constexpr int audio_iters = 5000;

    // CrossFade — 720p RGBA = 3,686,400 bytes per blend
    const double cf_scalar = benchCrossFade(simd::Mode::Scalar, frame_iters);
    std::printf("CrossFade 720p RGBA   scalar: %7.3f ms/call\n", cf_scalar);
    if (simd::avx2Available()) {
        const double cf_avx2 = benchCrossFade(simd::Mode::Avx2, frame_iters);
        std::printf("CrossFade 720p RGBA   avx2  : %7.3f ms/call  (%.2fx)\n",
                    cf_avx2, cf_scalar / cf_avx2);
    } else {
        std::printf("CrossFade 720p RGBA   avx2  : n/a (host has no AVX2)\n");
    }

    // AudioMixer — 1024 samples × 2 channels = 8192 bytes
    const double am_scalar = benchAudioMixer(simd::Mode::Scalar, audio_iters);
    std::printf("AudioMixer 1024×2     scalar: %7.3f ms/call\n", am_scalar);
    if (simd::avx2Available()) {
        const double am_avx2 = benchAudioMixer(simd::Mode::Avx2, audio_iters);
        std::printf("AudioMixer 1024×2     avx2  : %7.3f ms/call  (%.2fx)\n",
                    am_avx2, am_scalar / am_avx2);
    } else {
        std::printf("AudioMixer 1024×2     avx2  : n/a (host has no AVX2)\n");
    }

    return 0;
}
