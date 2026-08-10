#include "transitions/Wipe.h"
#include <algorithm>
#include <cstring>

Wipe::Wipe(TransitionType direction) : direction_(direction) {}

void Wipe::apply(const Frame& a, const Frame& b, Frame& out, float progress) {
    if (!a.valid() || !b.valid() || !out.valid()) return;

    const int w        = out.width;
    const int h        = out.height;
    const int row_bytes = w * 4;

    switch (direction_) {

    case TransitionType::WipeLeft: {
        // Reveal B from left to right
        const int split = static_cast<int>(w * progress);
        for (int row = 0; row < h; ++row) {
            uint8_t*       dst = out.pixels() + row * row_bytes;
            const uint8_t* sa  = a.pixels()   + row * row_bytes;
            const uint8_t* sb  = b.pixels()   + row * row_bytes;
            const int b_bytes  = std::min(split * 4, row_bytes);
            if (b_bytes > 0)
                std::memcpy(dst, sb, static_cast<size_t>(b_bytes));
            if (b_bytes < row_bytes)
                std::memcpy(dst + b_bytes, sa + b_bytes,
                            static_cast<size_t>(row_bytes - b_bytes));
        }
        break;
    }

    case TransitionType::WipeRight: {
        // Reveal B from right to left
        const int split = static_cast<int>(w * (1.0f - progress));
        for (int row = 0; row < h; ++row) {
            uint8_t*       dst = out.pixels() + row * row_bytes;
            const uint8_t* sa  = a.pixels()   + row * row_bytes;
            const uint8_t* sb  = b.pixels()   + row * row_bytes;
            const int a_bytes  = std::min(split * 4, row_bytes);
            if (a_bytes > 0)
                std::memcpy(dst, sa, static_cast<size_t>(a_bytes));
            if (a_bytes < row_bytes)
                std::memcpy(dst + a_bytes, sb + a_bytes,
                            static_cast<size_t>(row_bytes - a_bytes));
        }
        break;
    }

    case TransitionType::WipeDown: {
        // Reveal B from top to bottom
        const int split = static_cast<int>(h * progress);
        for (int row = 0; row < h; ++row) {
            uint8_t*       dst = out.pixels() + row * row_bytes;
            const uint8_t* src = (row < split ? b : a).pixels() + row * row_bytes;
            std::memcpy(dst, src, static_cast<size_t>(row_bytes));
        }
        break;
    }

    case TransitionType::WipeUp: {
        // Reveal B from bottom to top
        const int split = static_cast<int>(h * (1.0f - progress));
        for (int row = 0; row < h; ++row) {
            uint8_t*       dst = out.pixels() + row * row_bytes;
            const uint8_t* src = (row < split ? a : b).pixels() + row * row_bytes;
            std::memcpy(dst, src, static_cast<size_t>(row_bytes));
        }
        break;
    }

    default:
        std::memcpy(out.pixels(), a.pixels(), static_cast<size_t>(a.sizeBytes()));
        break;
    }
}
