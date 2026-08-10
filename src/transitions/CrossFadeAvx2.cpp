// fix28 — AVX2 path for CrossFade::apply.
//
// 32 bytes/iteration (one __m256i load), zero-extend to u16 in two halves,
// multiply by alpha/beta, sum, shift, pack back. Byte-identical to the
// scalar path because the scalar formula's intermediate fits in u16:
// max(a*beta + b*alpha) = 255*256 = 65280 < 65536. No saturation in
// practice, but we use packus for the final 16->8 step which gives the
// same result on values < 256.
//
// This file is compiled with -mavx2 ONLY (set_source_files_properties).
// The dispatcher in CrossFade.cpp guards calls with simd::current() ==
// Avx2, which is gated by avx2Available(). On a non-AVX2 host the
// function is never entered and therefore never decoded.

#include <cstddef>
#include <cstdint>
#include <immintrin.h>

namespace liveqx::simd {

void crossfadeBlendAvx2(const std::uint8_t* pa, const std::uint8_t* pb,
                         std::uint8_t* po, std::size_t n,
                         int alpha) noexcept {
    const int beta = 256 - alpha;
    const __m256i va_beta  =
        _mm256_set1_epi16(static_cast<std::int16_t>(beta));
    const __m256i vb_alpha =
        _mm256_set1_epi16(static_cast<std::int16_t>(alpha));

    std::size_t i = 0;
    for (; i + 32 <= n; i += 32) {
        const __m256i a32 = _mm256_loadu_si256(
            reinterpret_cast<const __m256i*>(pa + i));
        const __m256i b32 = _mm256_loadu_si256(
            reinterpret_cast<const __m256i*>(pb + i));

        const __m128i a_lo = _mm256_castsi256_si128(a32);
        const __m128i a_hi = _mm256_extracti128_si256(a32, 1);
        const __m128i b_lo = _mm256_castsi256_si128(b32);
        const __m128i b_hi = _mm256_extracti128_si256(b32, 1);

        __m256i a_lo16 = _mm256_cvtepu8_epi16(a_lo);
        __m256i a_hi16 = _mm256_cvtepu8_epi16(a_hi);
        __m256i b_lo16 = _mm256_cvtepu8_epi16(b_lo);
        __m256i b_hi16 = _mm256_cvtepu8_epi16(b_hi);

        a_lo16 = _mm256_mullo_epi16(a_lo16, va_beta);
        a_hi16 = _mm256_mullo_epi16(a_hi16, va_beta);
        b_lo16 = _mm256_mullo_epi16(b_lo16, vb_alpha);
        b_hi16 = _mm256_mullo_epi16(b_hi16, vb_alpha);

        __m256i sum_lo = _mm256_add_epi16(a_lo16, b_lo16);
        __m256i sum_hi = _mm256_add_epi16(a_hi16, b_hi16);

        sum_lo = _mm256_srli_epi16(sum_lo, 8);
        sum_hi = _mm256_srli_epi16(sum_hi, 8);

        // _mm256_packus_epi16 packs per-128-lane: result lanes are
        // [lo[lane0], hi[lane0], lo[lane1], hi[lane1]] interleaved
        // → permute 4×64-bit chunks via 0xD8 = 0b11_01_10_00 to
        // restore [lo[lane0], lo[lane1], hi[lane0], hi[lane1]] order.
        const __m256i packed = _mm256_packus_epi16(sum_lo, sum_hi);
        const __m256i result = _mm256_permute4x64_epi64(packed, 0xD8);

        _mm256_storeu_si256(reinterpret_cast<__m256i*>(po + i), result);
    }
    // Scalar tail for trailing < 32 bytes.
    for (; i < n; ++i)
        po[i] = static_cast<std::uint8_t>((pa[i] * beta + pb[i] * alpha) >> 8);
}

}  // namespace liveqx::simd
