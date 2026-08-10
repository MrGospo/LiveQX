#include "audio/AudioMixer.h"
#include "utils/SimdRuntime.h"

#include <algorithm>
#include <cstddef>

// Forward decl — defined in AudioMixerAvx2.cpp (built with -mavx2 only).
namespace liveqx::simd {
void audioCrossfadeAvx2(const float* a, std::size_t a_n,
                        const float* b, std::size_t b_n,
                        float* out, std::size_t n,
                        float progress) noexcept;
}

namespace {

void audioCrossfadeScalar(const float* a, std::size_t a_n,
                          const float* b, std::size_t b_n,
                          float* out, std::size_t n,
                          float progress) noexcept {
    const float inv = 1.0f - progress;
    for (std::size_t i = 0; i < n; ++i) {
        const float sa = i < a_n ? a[i] : 0.0f;
        const float sb = i < b_n ? b[i] : 0.0f;
        out[i] = sa * inv + sb * progress;
    }
}

}  // namespace

AudioFrame AudioMixer::crossfade(const AudioFrame& a, const AudioFrame& b, float progress) {
    AudioFrame out;
    out.sample_rate = a.sample_rate;
    out.channels    = a.channels;
    // Use the larger size — pad the shorter frame with silence (zeros).
    out.num_samples = std::max(a.num_samples, b.num_samples);
    out.pts         = a.pts;

    const std::size_t n = static_cast<std::size_t>(out.num_samples) * out.channels;
    out.samples.resize(n, 0.0f);

    if (liveqx::simd::current() == liveqx::simd::Mode::Avx2) {
        liveqx::simd::audioCrossfadeAvx2(
            a.samples.data(), a.samples.size(),
            b.samples.data(), b.samples.size(),
            out.samples.data(), n, progress);
    } else {
        audioCrossfadeScalar(
            a.samples.data(), a.samples.size(),
            b.samples.data(), b.samples.size(),
            out.samples.data(), n, progress);
    }

    out.valid = true;
    return out;
}

AudioFrame AudioMixer::silence(int num_samples, int sample_rate, int channels) {
    AudioFrame frame;
    frame.sample_rate = sample_rate;
    frame.channels = channels;
    frame.num_samples = num_samples;
    frame.samples.assign(static_cast<size_t>(num_samples * channels), 0.0f);
    frame.valid = true;
    return frame;
}
