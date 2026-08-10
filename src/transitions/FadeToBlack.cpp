#include "transitions/FadeToBlack.h"

#include <algorithm>
#include <cstring>

void FadeToBlack::apply(const Frame& a, const Frame& b,
                        Frame& out, float progress) {
    if (!out.valid()) return;
    const size_t n = out.sizeBytes();
    uint8_t*     po = out.pixels();

    // Halfway: pure black. We synthesise the black side rather than ask
    // the compositor to provide it — keeps the transition self-contained.
    if (progress <= 0.0f) {
        if (a.valid()) std::memcpy(po, a.pixels(), n);
        else           std::memset(po, 0, n);
        return;
    }
    if (progress >= 1.0f) {
        if (b.valid()) std::memcpy(po, b.pixels(), n);
        else           std::memset(po, 0, n);
        return;
    }

    const bool fading_out = progress < 0.5f;
    if (fading_out) {
        // A → black. alpha is the share of A still visible.
        const float p     = progress * 2.0f;        // 0 → 1 across the first half
        const int   alpha = static_cast<int>((1.0f - p) * 256.0f);
        if (!a.valid()) {
            std::memset(po, 0, n);
            return;
        }
        const uint8_t* pa = a.pixels();
        for (size_t i = 0; i < n; ++i)
            po[i] = static_cast<uint8_t>((pa[i] * alpha) >> 8);
    } else {
        // black → B. alpha is the share of B now visible.
        const float p     = (progress - 0.5f) * 2.0f;   // 0 → 1 across the second half
        const int   alpha = static_cast<int>(p * 256.0f);
        if (!b.valid()) {
            std::memset(po, 0, n);
            return;
        }
        const uint8_t* pb = b.pixels();
        for (size_t i = 0; i < n; ++i)
            po[i] = static_cast<uint8_t>((pb[i] * alpha) >> 8);
    }
}
