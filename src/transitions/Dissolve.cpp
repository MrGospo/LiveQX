#include "transitions/Dissolve.h"

void Dissolve::apply(const Frame& a, const Frame& b, Frame& out, float progress) {
    if (!a.valid() || !b.valid() || !out.valid()) return;
    // TODO: per-pixel threshold dissolve using a Bayer/noise pattern
    const size_t   n   = out.sizeBytes();
    const uint8_t* pa  = a.pixels();
    const uint8_t* pb  = b.pixels();
    uint8_t*       po  = out.pixels();
    const uint8_t  thr = static_cast<uint8_t>(progress * 255);
    for (size_t i = 0; i < n; ++i)
        po[i] = (pa[i] < thr) ? pb[i] : pa[i];
}
