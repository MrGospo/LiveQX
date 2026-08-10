// fix28 — AVX2 path for AudioMixer::crossfade.
//
// Per-sample lerp `out = sa*(1-p) + sb*p`. AVX2 processes 8 floats per
// _mm256_* instruction. We mirror the scalar instruction order exactly
// (two muls then one add — NOT FMADD) so the result is bit-for-bit
// equal to the scalar reference. The pixel/sample-identical test in
// test_audio_mixer_simd.cpp then memcmps the output buffers.
//
// The scalar path zero-pads when one input is shorter than `n`. We
// vectorise only the prefix where both sources fully cover all 8
// lanes; the scalar tail handles every lane past min(a_n, b_n) AND
// the non-multiple-of-8 remainder of `n`.
//
// Compiled with -mavx2 only (set_source_files_properties).

#include <algorithm>
#include <cstddef>
#include <immintrin.h>

namespace liveqx::simd {

void audioCrossfadeAvx2(const float* a, std::size_t a_n,
                         const float* b, std::size_t b_n,
                         float* out, std::size_t n,
                         float progress) noexcept {
    const float inv = 1.0f - progress;
    const __m256 vp   = _mm256_set1_ps(progress);
    const __m256 vinv = _mm256_set1_ps(inv);

    // Vectorise only the prefix where every lane has real data on both
    // sides. Anything past that boundary needs zero-padding and goes
    // through the scalar tail.
    const std::size_t safe_n = std::min({n, a_n, b_n});

    std::size_t i = 0;
    for (; i + 8 <= safe_n; i += 8) {
        const __m256 va = _mm256_loadu_ps(a + i);
        const __m256 vb = _mm256_loadu_ps(b + i);
        const __m256 ta = _mm256_mul_ps(va, vinv);
        const __m256 tb = _mm256_mul_ps(vb, vp);
        const __m256 sum = _mm256_add_ps(ta, tb);
        _mm256_storeu_ps(out + i, sum);
    }
    // Scalar tail — handles both the n%8 remainder and any lane past
    // a_n/b_n boundaries.
    for (; i < n; ++i) {
        const float sa = i < a_n ? a[i] : 0.0f;
        const float sb = i < b_n ? b[i] : 0.0f;
        out[i] = sa * inv + sb * progress;
    }
}

}  // namespace liveqx::simd
