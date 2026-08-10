#pragma once
#include <memory>
#include "core/Frame.h"

// Pre-allocated RGBA frame pool. All pixel buffers live in one contiguous
// arena allocated at construction — acquire() never calls malloc for buffer
// data. Thread-safe: frames can be released from any thread.
//
// Usage:
//   FramePool pool(8, 1280, 720);
//   Frame f = pool.acquire();          // O(1), no buffer malloc
//   // ... use f, copy f to other threads ...
//   // f goes out of scope → auto-returns to pool via custom deleter
class FramePool {
public:
    // Pre-allocates count * width * height * 4 bytes once.
    FramePool(int count, int width, int height);

    // Returns an invalid Frame (valid() == false) if pool is exhausted.
    // The frame buffer returns to the pool automatically when the last
    // copy of Frame::data is destroyed (custom deleter on shared_ptr).
    Frame acquire();

    // Apply MPOL_BIND to the entire pool arena so the pages physically land
     // on the given NUMA node regardless of which thread first-touches them.
     // No-op when node < 0, NUMA is unavailable, or mbind fails (warn-logged).
     // Idempotent; safe to call before any acquire().
     void bindMemoryToNumaNode(int node);

    int available() const;             // free slots right now (for monitoring)
    int capacity()  const noexcept { return count_; }
    int width()     const noexcept { return width_; }
    int height()    const noexcept { return height_; }
    // Total bytes pre-allocated in this pool (count * width * height * 4).
    std::size_t allocatedBytes() const noexcept {
        return static_cast<std::size_t>(count_) * width_ * height_ * 4;
    }

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;       // shared with every outstanding frame's deleter
    int count_;
    int width_;
    int height_;
};
