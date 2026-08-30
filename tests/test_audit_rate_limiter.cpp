// AuditRateLimiter — token bucket with injected clock.

#include <gtest/gtest.h>

#include <chrono>

#include "audit/AuditRateLimiter.h"

using liveqx::audit::AuditRateLimiter;
using liveqx::audit::RateLimitConfig;

TEST(AuditRateLimiter, AllowsUpToBurstThenRejects) {
    RateLimitConfig cfg;
    cfg.tokens_per_minute = 60;
    cfg.burst_capacity    = 5;
    AuditRateLimiter rl(cfg);

    auto now = std::chrono::steady_clock::now();
    rl.setClockOverride([&] { return now; });

    for (int i = 0; i < 5; ++i)
        EXPECT_TRUE(rl.tryAcquireForUser(1)) << "attempt " << i;
    EXPECT_FALSE(rl.tryAcquireForUser(1));

    auto s = rl.stats();
    EXPECT_EQ(5u, s.allowed);
    EXPECT_EQ(1u, s.rejected);
}

TEST(AuditRateLimiter, RefillsOverTime) {
    RateLimitConfig cfg;
    cfg.tokens_per_minute = 60;   // 1 token / sec
    cfg.burst_capacity    = 5;
    AuditRateLimiter rl(cfg);

    auto now = std::chrono::steady_clock::now();
    rl.setClockOverride([&] { return now; });

    for (int i = 0; i < 5; ++i) EXPECT_TRUE(rl.tryAcquireForUser(1));
    EXPECT_FALSE(rl.tryAcquireForUser(1));

    // Advance 3 seconds → 3 tokens refill.
    now += std::chrono::seconds(3);
    EXPECT_TRUE(rl.tryAcquireForUser(1));
    EXPECT_TRUE(rl.tryAcquireForUser(1));
    EXPECT_TRUE(rl.tryAcquireForUser(1));
    EXPECT_FALSE(rl.tryAcquireForUser(1));
}

TEST(AuditRateLimiter, ActorsAreIndependent) {
    RateLimitConfig cfg;
    cfg.tokens_per_minute = 60;
    cfg.burst_capacity    = 2;
    AuditRateLimiter rl(cfg);

    auto now = std::chrono::steady_clock::now();
    rl.setClockOverride([&] { return now; });

    EXPECT_TRUE(rl.tryAcquireForUser(1));
    EXPECT_TRUE(rl.tryAcquireForUser(1));
    EXPECT_FALSE(rl.tryAcquireForUser(1));

    // Different user still has full bucket.
    EXPECT_TRUE(rl.tryAcquireForUser(2));
    EXPECT_TRUE(rl.tryAcquireForUser(2));

    // IP is a separate key namespace even if the numeric shape matches.
    EXPECT_TRUE(rl.tryAcquireForIp("10.0.0.1"));
    EXPECT_TRUE(rl.tryAcquireForIp("10.0.0.1"));
    EXPECT_FALSE(rl.tryAcquireForIp("10.0.0.1"));
    EXPECT_TRUE(rl.tryAcquireForIp("10.0.0.2"));
}

TEST(AuditRateLimiter, BurstCapIsRespected) {
    RateLimitConfig cfg;
    cfg.tokens_per_minute = 6000;  // very fast refill
    cfg.burst_capacity    = 3;
    AuditRateLimiter rl(cfg);

    auto now = std::chrono::steady_clock::now();
    rl.setClockOverride([&] { return now; });

    // Idle for a long time — bucket still capped at burst_capacity.
    now += std::chrono::minutes(1);
    EXPECT_TRUE(rl.tryAcquireForUser(1));
    EXPECT_TRUE(rl.tryAcquireForUser(1));
    EXPECT_TRUE(rl.tryAcquireForUser(1));
    EXPECT_FALSE(rl.tryAcquireForUser(1));
}
