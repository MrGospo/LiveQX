#include <gtest/gtest.h>
#include <algorithm>
#include <thread>
#include <vector>
#include "core/Frame.h"
#include "core/FramePool.h"

// ─── helpers ──────────────────────────────────────────────────────────────────

static constexpr int kW = 64, kH = 48, kN = 5;

// ─── basic geometry ───────────────────────────────────────────────────────────

TEST(FramePoolTest, ConstructionSetsGeometry) {
    FramePool pool(kN, kW, kH);
    EXPECT_EQ(pool.capacity(),  kN);
    EXPECT_EQ(pool.available(), kN);
    EXPECT_EQ(pool.width(),     kW);
    EXPECT_EQ(pool.height(),    kH);
}

TEST(FramePoolTest, AcquiredFrameHasCorrectGeometry) {
    FramePool pool(1, kW, kH);
    Frame f = pool.acquire();
    EXPECT_TRUE(f.valid());
    EXPECT_EQ(f.width,  kW);
    EXPECT_EQ(f.height, kH);
    EXPECT_EQ(f.sizeBytes(), static_cast<size_t>(kW) * kH * 4);
}

// ─── core scenario: acquire all → exhaust → release → re-acquire ──────────────

TEST(FramePoolTest, AcquireAllReleaseAcquireAgain) {
    FramePool pool(kN, kW, kH);

    // 1. Acquire all 5 frames
    std::vector<Frame> held;
    held.reserve(kN);
    for (int i = 0; i < kN; ++i) {
        Frame f = pool.acquire();
        EXPECT_TRUE(f.valid())  << "frame " << i << " should be valid";
        held.push_back(std::move(f));
    }
    EXPECT_EQ(pool.available(), 0);

    // 2. Pool is exhausted
    EXPECT_FALSE(pool.acquire().valid()) << "6th acquire must return invalid frame";

    // 3. Release all — frames go out of scope, custom deleter returns them
    held.clear();
    EXPECT_EQ(pool.available(), kN);

    // 4. Acquire all 5 again — pool must be fully refilled
    for (int i = 0; i < kN; ++i) {
        Frame f = pool.acquire();
        EXPECT_TRUE(f.valid()) << "re-acquired frame " << i << " should be valid";
        held.push_back(std::move(f));
    }
    EXPECT_EQ(pool.available(), 0);
}

// ─── shared_ptr copy semantics: buffer is only returned once ──────────────────

TEST(FramePoolTest, CopySharesBufferReturnedOnce) {
    FramePool pool(1, kW, kH);

    Frame f1 = pool.acquire();
    EXPECT_EQ(pool.available(), 0);

    Frame f2 = f1;                         // shared_ptr copy, refcount 1→2
    EXPECT_EQ(f1.pixels(), f2.pixels());   // same underlying buffer
    EXPECT_EQ(pool.available(), 0);

    f1 = Frame{};                          // refcount 2→1, buffer NOT returned yet
    EXPECT_EQ(pool.available(), 0);

    f2 = Frame{};                          // refcount 1→0, custom deleter fires
    EXPECT_EQ(pool.available(), 1);
}

// ─── pixel write/read through the pool buffer ─────────────────────────────────

TEST(FramePoolTest, PixelWriteReadRoundtrip) {
    FramePool pool(1, kW, kH);
    Frame f = pool.acquire();

    f.pixels()[0] = 0xDE;
    f.pixels()[1] = 0xAD;
    f.pixels()[2] = 0xBE;
    f.pixels()[3] = 0xEF;

    Frame g = f;   // copy shares buffer
    EXPECT_EQ(g.pixels()[0], 0xDE);
    EXPECT_EQ(g.pixels()[3], 0xEF);
}

// ─── no malloc for buffer data after construction ─────────────────────────────
// We can't intercept malloc portably, but we verify the buffer pointer
// stays within the pre-allocated arena across acquire/release cycles.

TEST(FramePoolTest, BuffersStayWithinArena) {
    FramePool pool(kN, kW, kH);

    // Collect buffer addresses from first cycle
    std::vector<uint8_t*> ptrs_first;
    {
        std::vector<Frame> frames;
        for (int i = 0; i < kN; ++i) {
            frames.push_back(pool.acquire());
            ptrs_first.push_back(frames.back().pixels());
        }
    }  // all returned

    // Second cycle — same pointers must come back (pool recycles, not re-allocates)
    std::vector<uint8_t*> ptrs_second;
    {
        std::vector<Frame> frames;
        for (int i = 0; i < kN; ++i) {
            frames.push_back(pool.acquire());
            ptrs_second.push_back(frames.back().pixels());
        }
    }

    std::sort(ptrs_first.begin(),  ptrs_first.end());
    std::sort(ptrs_second.begin(), ptrs_second.end());
    EXPECT_EQ(ptrs_first, ptrs_second);
}

// ─── thread safety: release from a different thread ───────────────────────────

TEST(FramePoolTest, ReleaseFromOtherThread) {
    FramePool pool(1, kW, kH);
    Frame f = pool.acquire();
    EXPECT_EQ(pool.available(), 0);

    // Move frame to another thread, which destroys it
    std::thread t([f = std::move(f)]() mutable {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        f = Frame{};   // deleter runs in this thread
    });
    t.join();

    EXPECT_EQ(pool.available(), 1);
}
