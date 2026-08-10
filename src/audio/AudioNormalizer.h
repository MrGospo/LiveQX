#pragma once
#include "core/AudioFrame.h"
#include <array>
#include <cstdint>

// EBU R128 loudness normalization (ITU-R BS.1770-4), target -23 LUFS.
//
// Usage (two-pass):
//   AudioNormalizer norm(-23.0f);
//   for (auto& f : clip_frames) norm.measure(f);  // first pass: full clip
//   norm.commit();                                  // compute gain
//   AudioFrame out = norm.normalize(input_frame);   // second pass: real-time
//
// For streaming without a pre-scan, call commit() with no prior measure()
// calls — gain defaults to 1.0 (no change). Reset() restarts measurement.
class AudioNormalizer {
public:
    explicit AudioNormalizer(float target_lufs = -23.0f);

    // First-pass: feed audio frames to accumulate loudness measurements.
    void measure(const AudioFrame& frame);

    // Compute gain from accumulated measurements and reset measurement state.
    // Must be called before normalize(). Safe to call with no prior measure().
    void commit();

    // Reset measurement state (allows re-measuring a new clip).
    void reset();

    // Second-pass: apply gain correction. Thread-safe after commit().
    AudioFrame normalize(const AudioFrame& input) const;

    float measuredLufs() const { return measured_lufs_; }
    float gainDb()        const;

private:
    // K-weighting biquad filter state (per channel, two stages).
    struct BiquadState { double x1 = 0, x2 = 0, y1 = 0, y2 = 0; };

    // Per-channel, two-stage filter state for measurement pass.
    std::array<BiquadState, 2> stage1_{}; // high-shelf pre-filter
    std::array<BiquadState, 2> stage2_{}; // RLB high-pass

    double sum_sq_     = 0.0; // accumulated K-weighted mean square
    int64_t n_samples_ = 0;   // total sample count (per channel)

    float target_lufs_  = -23.0f;
    float measured_lufs_ = -70.0f; // before commit(): undefined
    float gain_linear_   = 1.0f;
};
