#include "transitions/Push.h"

#include <algorithm>
#include <cstring>

Push::Push(TransitionType direction) : direction_(direction) {}

// Geometry shared with Wipe — RGBA pitch = w*4 — but Push slides whole
// rows/cols rather than splitting them by reveal. For the horizontal
// directions we copy `w-offset` bytes-of-A starting at `offset` into
// the head of the output row, then `offset` bytes-of-B from B's head
// into the tail. Vertical directions copy whole rows from A or B
// depending on their position relative to the slide front.
void Push::apply(const Frame& a, const Frame& b,
                 Frame& out, float progress) {
    if (!a.valid() || !b.valid() || !out.valid()) return;

    const int w         = out.width;
    const int h         = out.height;
    const int row_bytes = w * 4;

    // Clamp progress and turn it into a pixel offset on the relevant axis.
    const float p = std::clamp(progress, 0.0f, 1.0f);

    switch (direction_) {

    case TransitionType::PushLeft: {
        // A slides left out of frame; B slides in from the right edge.
        // out[0 .. w-off)  = A[off .. w)
        // out[w-off .. w)  = B[0 .. off)
        const int off       = std::clamp(static_cast<int>(p * w), 0, w);
        const int a_bytes   = (w - off) * 4;
        const int b_bytes   = off * 4;
        const int a_offset  = off * 4;
        for (int row = 0; row < h; ++row) {
            uint8_t*       dst = out.pixels() + row * row_bytes;
            const uint8_t* sa  = a.pixels()   + row * row_bytes + a_offset;
            const uint8_t* sb  = b.pixels()   + row * row_bytes;
            if (a_bytes > 0) std::memcpy(dst,             sa, static_cast<size_t>(a_bytes));
            if (b_bytes > 0) std::memcpy(dst + a_bytes,   sb, static_cast<size_t>(b_bytes));
        }
        break;
    }

    case TransitionType::PushRight: {
        // A slides right out of frame; B slides in from the left edge.
        // out[0 .. off)    = B[w-off .. w)
        // out[off .. w)    = A[0 .. w-off)
        const int off       = std::clamp(static_cast<int>(p * w), 0, w);
        const int b_bytes   = off * 4;
        const int a_bytes   = (w - off) * 4;
        const int b_offset  = (w - off) * 4;
        for (int row = 0; row < h; ++row) {
            uint8_t*       dst = out.pixels() + row * row_bytes;
            const uint8_t* sb  = b.pixels()   + row * row_bytes + b_offset;
            const uint8_t* sa  = a.pixels()   + row * row_bytes;
            if (b_bytes > 0) std::memcpy(dst,             sb, static_cast<size_t>(b_bytes));
            if (a_bytes > 0) std::memcpy(dst + b_bytes,   sa, static_cast<size_t>(a_bytes));
        }
        break;
    }

    case TransitionType::PushUp: {
        // A slides up out of frame; B slides in from the bottom edge.
        // out[row]  = A[row + off]      for row < h-off
        // out[row]  = B[row - (h-off)]  for row >= h-off
        const int off = std::clamp(static_cast<int>(p * h), 0, h);
        for (int row = 0; row < h; ++row) {
            uint8_t* dst = out.pixels() + row * row_bytes;
            if (row < h - off) {
                const uint8_t* sa = a.pixels() + (row + off) * row_bytes;
                std::memcpy(dst, sa, static_cast<size_t>(row_bytes));
            } else {
                const uint8_t* sb = b.pixels() + (row - (h - off)) * row_bytes;
                std::memcpy(dst, sb, static_cast<size_t>(row_bytes));
            }
        }
        break;
    }

    case TransitionType::PushDown: {
        // A slides down out of frame; B slides in from the top edge.
        // out[row]  = B[row + (h-off)]  for row < off
        // out[row]  = A[row - off]      for row >= off
        const int off = std::clamp(static_cast<int>(p * h), 0, h);
        for (int row = 0; row < h; ++row) {
            uint8_t* dst = out.pixels() + row * row_bytes;
            if (row < off) {
                const uint8_t* sb = b.pixels() + (row + (h - off)) * row_bytes;
                std::memcpy(dst, sb, static_cast<size_t>(row_bytes));
            } else {
                const uint8_t* sa = a.pixels() + (row - off) * row_bytes;
                std::memcpy(dst, sa, static_cast<size_t>(row_bytes));
            }
        }
        break;
    }

    default:
        std::memcpy(out.pixels(), a.pixels(),
                    static_cast<size_t>(a.sizeBytes()));
        break;
    }
}
