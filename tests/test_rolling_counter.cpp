#include "metrics/RollingCounter.h"
#include <gtest/gtest.h>
#include <thread>
#include <vector>

TEST(RollingCounter, EmptyRateIsZero) {
    RollingCounter<60> rc;
    EXPECT_EQ(rc.rate(1), 0u);
    EXPECT_EQ(rc.rate(60), 0u);
}

TEST(RollingCounter, AddedEventsVisibleInCurrentBucket) {
    RollingCounter<60> rc;
    for (int i = 0; i < 100; ++i) rc.add(1);
    EXPECT_EQ(rc.rate(1), 100u);
    EXPECT_EQ(rc.rate(10), 100u);
    EXPECT_EQ(rc.rate(60), 100u);
}

TEST(RollingCounter, TickAdvancesAndZeroesNewCurrent) {
    RollingCounter<60> rc;
    for (int i = 0; i < 100; ++i) rc.add(1);
    rc.tick();
    // New current bucket is zero; old bucket still in 10s window.
    EXPECT_EQ(rc.rate(1), 0u);
    EXPECT_EQ(rc.rate(10), 100u);
    EXPECT_EQ(rc.rate(60), 100u);
}

TEST(RollingCounter, ValuesFallOutOfWindow) {
    RollingCounter<5> rc;
    rc.add(7);
    for (int i = 0; i < 5; ++i) rc.tick();  // wrap completely past it
    EXPECT_EQ(rc.rate(5), 0u);
}

TEST(RollingCounter, ClampsWindowToCapacity) {
    RollingCounter<10> rc;
    rc.add(3);
    EXPECT_EQ(rc.rate(1000), 3u);  // clamped to 10, all buckets summed
}

TEST(RollingCounter, ConcurrentAddsAreCounted) {
    RollingCounter<60> rc;
    constexpr int N_THREADS = 4;
    constexpr int PER       = 10000;
    std::vector<std::thread> threads;
    for (int i = 0; i < N_THREADS; ++i)
        threads.emplace_back([&] {
            for (int k = 0; k < PER; ++k) rc.add(1);
        });
    for (auto& t : threads) t.join();
    EXPECT_EQ(rc.rate(60), static_cast<uint64_t>(N_THREADS) * PER);
}
