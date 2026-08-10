#include <gtest/gtest.h>
#include <chrono>
#include <thread>
#include "utils/SpscQueue.h"

TEST(SpscQueueTest, PushPop) {
    SpscQueue<int, 4> q;
    EXPECT_TRUE(q.empty());
    EXPECT_TRUE(q.push(42));
    EXPECT_FALSE(q.empty());
    auto v = q.pop();
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, 42);
    EXPECT_TRUE(q.empty());
}

TEST(SpscQueueTest, FullQueueRejectsPush) {
    SpscQueue<int, 4> q;
    EXPECT_TRUE(q.push(1));
    EXPECT_TRUE(q.push(2));
    EXPECT_TRUE(q.push(3));
    EXPECT_FALSE(q.push(4));  // capacity N-1 = 3
}

TEST(SpscQueueTest, PopWaitReturnsEmptyOnTimeout) {
    SpscQueue<int, 4> q;
    const auto t0 = std::chrono::steady_clock::now();
    auto v = q.pop_wait(std::chrono::milliseconds(30));
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    EXPECT_FALSE(v.has_value());
    EXPECT_GE(elapsed, std::chrono::milliseconds(25));
}

TEST(SpscQueueTest, PopWaitWakesOnNotify) {
    SpscQueue<int, 4> q;
    std::thread producer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        q.push(7);
        q.notify();
    });
    auto v = q.pop_wait(std::chrono::milliseconds(500));
    producer.join();
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, 7);
}

TEST(SpscQueueTest, PopWaitFastPathWhenAlreadyAvailable) {
    SpscQueue<int, 4> q;
    q.push(99);
    const auto t0 = std::chrono::steady_clock::now();
    auto v = q.pop_wait(std::chrono::milliseconds(500));
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, 99);
    EXPECT_LT(elapsed, std::chrono::milliseconds(50));
}
