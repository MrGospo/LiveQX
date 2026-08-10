#include "core/Watchdog.h"
#include "metrics/ChannelHealth.h"
#include "metrics/ChannelMetrics.h"
#include <gtest/gtest.h>
#include <chrono>
#include <memory>

using namespace std::chrono;

TEST(Watchdog, SkipsChannelWithoutHeartbeat) {
    Watchdog wd;
    auto m = std::make_shared<ChannelMetrics>();
    auto h = std::make_shared<ChannelHealth>("1", 25);
    wd.registerChannel(m, h);
    // last_tick_ns == 0 → evaluate skips
    wd.evaluateOnce(seconds(10));
    EXPECT_EQ(h->state(), HealthState::Running);
}

TEST(Watchdog, FlipsToFailedOnStaleHeartbeat) {
    Watchdog wd;
    auto m = std::make_shared<ChannelMetrics>();
    auto h = std::make_shared<ChannelHealth>("1", 25);
    wd.registerChannel(m, h);
    // Simulate a tick at t=1s, then evaluate at t=10s → 9s heartbeat age.
    m->last_tick_ns.store(seconds(1).count() * 1'000'000'000LL,
                          std::memory_order_relaxed);
    // Pretend rendered counter healthy so only heartbeat triggers.
    m->rolling_rendered.add(250);
    wd.evaluateOnce(seconds(10));
    EXPECT_EQ(h->state(), HealthState::Failed);
}

TEST(Watchdog, ThreadStartsAndStops) {
    Watchdog wd;
    wd.start();
    std::this_thread::sleep_for(milliseconds(50));
    wd.stop();
    SUCCEED();
}

// fix12 c7: outputs probe drives Degraded transition.
TEST(Watchdog, OutputProbeDegradesOnPartialFailure) {
    Watchdog wd;
    auto m = std::make_shared<ChannelMetrics>();
    auto h = std::make_shared<ChannelHealth>("1", 25);
    int healthy_count = 1;
    wd.registerChannel(m, h, [&] { return OutputHealthSummary{healthy_count, 2}; });
    auto tick_at = [&](int sec) {
        m->last_tick_ns.store(seconds(sec).count() * 1'000'000'000LL,
                              std::memory_order_relaxed);
    };
    m->rolling_rendered.add(250);
    tick_at(10); wd.evaluateOnce(seconds(10));
    EXPECT_EQ(h->state(), HealthState::Degraded);
    // Both outputs back up → after 30s clean recovers.
    healthy_count = 2;
    tick_at(11); wd.evaluateOnce(seconds(11));
    tick_at(45); wd.evaluateOnce(seconds(45));
    EXPECT_EQ(h->state(), HealthState::Running);
}

TEST(Watchdog, OutputProbeFailsWhenAllDown) {
    Watchdog wd;
    auto m = std::make_shared<ChannelMetrics>();
    auto h = std::make_shared<ChannelHealth>("1", 25);
    wd.registerChannel(m, h, [] { return OutputHealthSummary{0, 1}; });
    m->last_tick_ns.store(seconds(10).count() * 1'000'000'000LL,
                          std::memory_order_relaxed);
    m->rolling_rendered.add(250);
    wd.evaluateOnce(seconds(10));
    EXPECT_EQ(h->state(), HealthState::Failed);
}
