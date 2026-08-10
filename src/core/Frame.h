#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>

// RGBA video frame. Value type with shared-buffer semantics:
// copying a Frame is O(1) — both copies reference the same pixel data.
// An invalid frame has data == nullptr.
struct Frame {
    std::shared_ptr<uint8_t[]> data;  // RGBA packed, row-major, stride = width*4
    int     width  = 0;
    int     height = 0;
    int64_t pts    = 0;               // presentation timestamp, microseconds

    bool   valid()     const noexcept { return data != nullptr; }
    size_t sizeBytes() const noexcept { return static_cast<size_t>(width) * height * 4; }

    // Convenience raw-pointer access (same as data.get())
    uint8_t*       pixels()       noexcept { return data.get(); }
    const uint8_t* pixels() const noexcept { return data.get(); }
};
