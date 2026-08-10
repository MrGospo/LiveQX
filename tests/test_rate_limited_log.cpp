#include "utils/RateLimitedLog.h"
#include <gtest/gtest.h>
#include <chrono>
#include <thread>

TEST(RateLimiter, AdmitsUpToBudget) {
    ratelog::Limiter lim(1'000'000'000LL, 5);  // 5 / 1s
    int allowed = 0;
    uint64_t total_suppressed = 0;
    for (int i = 0; i < 100; ++i) {
        auto d = lim.admit();
        if (d.allow) ++allowed;
        total_suppressed += d.suppressed;
    }
    EXPECT_EQ(allowed, 5);
    // First admission of a fresh window has no preceding suppressions.
    EXPECT_EQ(total_suppressed, 0u);
}

TEST(RateLimiter, RollsWindowAndReportsSuppressed) {
    ratelog::Limiter lim(20'000'000LL, 3);  // 3 per 20ms

    int allowed1 = 0;
    for (int i = 0; i < 50; ++i)
        if (lim.admit().allow) ++allowed1;
    EXPECT_EQ(allowed1, 3);

    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    auto d = lim.admit();
    EXPECT_TRUE(d.allow);
    EXPECT_GT(d.suppressed, 0u);  // 47 dropped in prior window
}

TEST(RateLimiter, SuppressedCountResetsAfterReport) {
    ratelog::Limiter lim(20'000'000LL, 1);
    lim.admit();                        // budget consumed
    for (int i = 0; i < 5; ++i) lim.admit();  // suppressed +=5

    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    auto d1 = lim.admit();
    EXPECT_TRUE(d1.allow);
    EXPECT_EQ(d1.suppressed, 5u);

    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    auto d2 = lim.admit();
    EXPECT_TRUE(d2.allow);
    EXPECT_EQ(d2.suppressed, 0u);  // no suppressions between reports
}
