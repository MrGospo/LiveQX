#include "audio/AudioNormalizer.h"

#include <algorithm>
#include <cmath>

// ─── K-weighting filter coefficients at 48 kHz (ITU-R BS.1770-4) ─────────────
//
// Stage 1 — high-shelf pre-filter (+4 dB above ~1 kHz):
//   b = [1.53512485958697, -2.69169618940638, 1.19839281085285]
//   a = [1.0, -1.69065929318241, 0.73248077421585]
//
// Stage 2 — RLB high-pass (removes sub-bass, fc ≈ 38 Hz):
//   b = [1.0, -2.0, 1.0]
//   a = [1.0, -1.99004745483398, 0.99007225036621]

namespace {

constexpr double kS1_b0 =  1.53512485958697;
constexpr double kS1_b1 = -2.69169618940638;
constexpr double kS1_b2 =  1.19839281085285;
constexpr double kS1_a1 = -1.69065929318241;
constexpr double kS1_a2 =  0.73248077421585;

constexpr double kS2_b0 =  1.0;
constexpr double kS2_b1 = -2.0;
constexpr double kS2_b2 =  1.0;
constexpr double kS2_a1 = -1.99004745483398;
constexpr double kS2_a2 =  0.99007225036621;

// Direct-form II transposed biquad.
inline double biquad(double x, double b0, double b1, double b2,
                     double a1, double a2,
                     double& x1, double& x2, double& y1, double& y2) {
    double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
    x2 = x1; x1 = x;
    y2 = y1; y1 = y;
    return y;
}

} // namespace

// ─── AudioNormalizer ──────────────────────────────────────────────────────────

AudioNormalizer::AudioNormalizer(float target_lufs)
    : target_lufs_(target_lufs) {}

void AudioNormalizer::reset() {
    stage1_ = {};
    stage2_ = {};
    sum_sq_    = 0.0;
    n_samples_ = 0;
    measured_lufs_ = -70.0f;
    gain_linear_   = 1.0f;
}

void AudioNormalizer::measure(const AudioFrame& frame) {
    if (!frame.valid || frame.num_samples <= 0) return;

    const int ch = std::min(frame.channels, 2);

    for (int i = 0; i < frame.num_samples; ++i) {
        double sum_ch = 0.0;
        for (int c = 0; c < ch; ++c) {
            double s = static_cast<double>(frame.samples[i * frame.channels + c]);

            // Stage 1: high-shelf pre-filter
            s = biquad(s, kS1_b0, kS1_b1, kS1_b2, kS1_a1, kS1_a2,
                       stage1_[c].x1, stage1_[c].x2,
                       stage1_[c].y1, stage1_[c].y2);

            // Stage 2: RLB high-pass
            s = biquad(s, kS2_b0, kS2_b1, kS2_b2, kS2_a1, kS2_a2,
                       stage2_[c].x1, stage2_[c].x2,
                       stage2_[c].y1, stage2_[c].y2);

            sum_ch += s * s;
        }
        // Mean across channels (mono or stereo treated equally per BS.1770)
        sum_sq_ += sum_ch / ch;
    }
    n_samples_ += frame.num_samples;
}

void AudioNormalizer::commit() {
    if (n_samples_ > 0) {
        const double mean_sq = sum_sq_ / static_cast<double>(n_samples_);
        // LUFS = -0.691 + 10 * log10(mean_sq) per BS.1770 (exact formula)
        measured_lufs_ = static_cast<float>(
            -0.691 + 10.0 * std::log10(std::max(mean_sq, 1e-10)));

        const float gain_db = target_lufs_ - measured_lufs_;
        // Clamp to ±30 dB to avoid destructive gain on near-silent content
        const float clamped = std::clamp(gain_db, -30.0f, 30.0f);
        gain_linear_ = std::pow(10.0f, clamped / 20.0f);
    }

    // Reset measurement state — ready to measure the next clip.
    stage1_ = {};
    stage2_ = {};
    sum_sq_    = 0.0;
    n_samples_ = 0;
}

float AudioNormalizer::gainDb() const {
    return 20.0f * std::log10(std::max(gain_linear_, 1e-6f));
}

AudioFrame AudioNormalizer::normalize(const AudioFrame& input) const {
    if (!input.valid || gain_linear_ == 1.0f) return input;

    AudioFrame out = input;
    for (float& s : out.samples) {
        s *= gain_linear_;
        // Soft clip at ±1.0 to prevent hard clipping artifacts.
        if      (s >  1.0f) s =  1.0f - std::exp(-(s -  1.0f));
        else if (s < -1.0f) s = -1.0f + std::exp(-(-s - 1.0f));
    }
    return out;
}
