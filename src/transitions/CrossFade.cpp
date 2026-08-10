#include "transitions/CrossFade.h"

#include "utils/SimdRuntime.h"

#include <cstdint>

// Forward decl — defined in CrossFadeAvx2.cpp (built with -mavx2 only on
// that file via set_source_files_properties).
namespace liveqx::simd {
void crossfadeBlendAvx2(const std::uint8_t* a, const std::uint8_t* b,
                        std::uint8_t* out, std::size_t n,
                        int alpha) noexcept;
}

namespace {

// The reference scalar blend. AVX2 path must be byte-identical.
void crossfadeBlendScalar(const std::uint8_t* pa, const std::uint8_t* pb,
                          std::uint8_t* po, std::size_t n,
                          int alpha) noexcept {
    const int beta = 256 - alpha;
    for (std::size_t i = 0; i < n; ++i)
        po[i] = static_cast<std::uint8_t>((pa[i] * beta + pb[i] * alpha) >> 8);
}

}  // namespace

void CrossFade::apply(const Frame& a, const Frame& b, Frame& out, float progress) {
    if (!a.valid() || !b.valid() || !out.valid()) return;
    const std::size_t n     = static_cast<std::size_t>(out.sizeBytes());
    const int         alpha = static_cast<int>(progress * 256.0f);

    if (liveqx::simd::current() == liveqx::simd::Mode::Avx2) {
        liveqx::simd::crossfadeBlendAvx2(
            a.pixels(), b.pixels(), out.pixels(), n, alpha);
    } else {
        crossfadeBlendScalar(a.pixels(), b.pixels(), out.pixels(), n, alpha);
    }
}
