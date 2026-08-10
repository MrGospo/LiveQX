// fix23 commit 1/N — EventBus core unit tests.

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "events/EventBus.h"

namespace sce = liveqx::events;
using namespace std::chrono_literals;

TEST(EventBus, MonotonicIdsAndPayloadRoundTrip) {
    sce::EventBus bus;
    auto sub = bus.subscribe();

    bus.publish(sce::EventType::ChannelStateChange, 1, {{"state", "running"}});
    bus.publish(sce::EventType::ClipChange,         1, {{"clip_id", 42}});

    auto evs = sub->drain(100ms);
    ASSERT_EQ(evs.size(), 2u);
    EXPECT_EQ(evs[0].id, 1u);
    EXPECT_EQ(evs[1].id, 2u);
    EXPECT_EQ(evs[0].type, sce::EventType::ChannelStateChange);
    EXPECT_EQ(evs[1].type, sce::EventType::ClipChange);
    EXPECT_EQ(evs[0].channel_id, 1);
    EXPECT_EQ(evs[0].payload["state"], "running");
    EXPECT_EQ(evs[1].payload["clip_id"], 42);
    EXPECT_GT(evs[0].ts_unix_ms, 0);
}

TEST(EventBus, MultipleSubscribersFanOut) {
    sce::EventBus bus;
    auto a = bus.subscribe();
    auto b = bus.subscribe();
    bus.publish(sce::EventType::HealthChange, 7, {{"health", "degraded"}});

    auto ea = a->drain(50ms);
    auto eb = b->drain(50ms);
    ASSERT_EQ(ea.size(), 1u);
    ASSERT_EQ(eb.size(), 1u);
    EXPECT_EQ(ea[0].id, eb[0].id);
}

TEST(EventBus, ReplayViaSinceId) {
    sce::EventBus bus(/*replay=*/16);
    bus.publish(sce::EventType::ChannelStateChange, 1, {});
    bus.publish(sce::EventType::ChannelStateChange, 2, {});
    bus.publish(sce::EventType::ChannelStateChange, 3, {});

    // Reader who got id=1 reconnects with since_id=1 — should receive 2 and 3.
    auto sub = bus.subscribe(/*since_id=*/1);
    auto evs = sub->drain(50ms);
    ASSERT_EQ(evs.size(), 2u);
    EXPECT_EQ(evs[0].id, 2u);
    EXPECT_EQ(evs[1].id, 3u);
}

TEST(EventBus, ReplayDroppedWhenRingTooSmall) {
    sce::EventBus bus(/*replay=*/2);
    for (int i = 0; i < 10; ++i)
        bus.publish(sce::EventType::ChannelStateChange, i, {});
    // Ring kept only the last 2: ids 9, 10.
    auto sub = bus.subscribe(/*since_id=*/0);
    auto evs = sub->drain(50ms);
    ASSERT_EQ(evs.size(), 2u);
    EXPECT_EQ(evs[0].id, 9u);
    EXPECT_EQ(evs[1].id, 10u);
}

TEST(EventBus, SlowSubscriberOverflows) {
    sce::EventBus bus(/*replay=*/16, /*cap=*/4);
    auto sub = bus.subscribe();
    for (int i = 0; i < 20; ++i)
        bus.publish(sce::EventType::ClipChange, 1, {{"i", i}});

    EXPECT_TRUE(sub->overflowed());
    auto evs = sub->drain(50ms);
    // Capacity holds last 4 events.
    ASSERT_EQ(evs.size(), 4u);
}

TEST(EventBus, BlockingDrainTimesOut) {
    sce::EventBus bus;
    auto sub = bus.subscribe();
    auto t0 = std::chrono::steady_clock::now();
    auto evs = sub->drain(60ms);
    auto t1 = std::chrono::steady_clock::now();
    EXPECT_TRUE(evs.empty());
    EXPECT_GE(t1 - t0, 50ms);
}

TEST(EventBus, BlockingDrainWakesOnPublish) {
    sce::EventBus bus;
    auto sub = bus.subscribe();
    std::thread writer([&] {
        std::this_thread::sleep_for(20ms);
        bus.publish(sce::EventType::ClipChange, 1, {});
    });
    auto t0 = std::chrono::steady_clock::now();
    auto evs = sub->drain(500ms);
    auto t1 = std::chrono::steady_clock::now();
    writer.join();
    ASSERT_EQ(evs.size(), 1u);
    EXPECT_LT(t1 - t0, 200ms);
}

TEST(EventBus, ExpiredSubscribersAreCleanedUp) {
    sce::EventBus bus;
    {
        auto a = bus.subscribe();
        EXPECT_EQ(bus.subscriberCount(), 1u);
    }
    // Drop, then publish — expired weak_ptr is cleaned up on next publish.
    bus.publish(sce::EventType::ClipChange, 1, {});
    EXPECT_EQ(bus.subscriberCount(), 0u);
}

TEST(EventBus, WakeUpUnblocksSubscriber) {
    sce::EventBus bus;
    auto sub = bus.subscribe();
    std::atomic<bool> done{false};
    std::thread t([&] {
        sub->drain(5s);
        done.store(true);
    });
    std::this_thread::sleep_for(20ms);
    sub->wakeUp();
    for (int i = 0; i < 50 && !done.load(); ++i)
        std::this_thread::sleep_for(5ms);
    EXPECT_TRUE(done.load());
    t.join();
}

TEST(EventBus, EventTypeNameAndParse) {
    EXPECT_STREQ(sce::eventTypeName(sce::EventType::ChannelStateChange),
                 "channel_state_change");
    EXPECT_EQ(sce::parseEventType("clip_change").value(),
              sce::EventType::ClipChange);
    EXPECT_FALSE(sce::parseEventType("nope").has_value());
}
