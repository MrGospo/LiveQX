#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "output/OutputManager.h"

namespace {

class CountingDriver : public IOutput {
public:
    void send(const Packet&) override { ++packets_; }
    bool isHealthy() const override { return healthy_; }
    OutputStats getStats() const override {
        OutputStats s;
        s.packets_sent = packets_;
        return s;
    }
    nlohmann::json statusJson() const override {
        return nlohmann::json{{"transport", "mock"},
                              {"packets_sent", packets_.load()}};
    }

    std::atomic<int> packets_{0};
    bool             healthy_ = true;
};

// Helper: poll until predicate is true or `timeout` elapses.
template <typename F>
bool waitFor(F&& pred, std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return pred();
}

TEST(OutputManager, AddDriverRejectsDuplicateId) {
    OutputManager mgr;
    auto a = std::make_shared<CountingDriver>();
    auto b = std::make_shared<CountingDriver>();
    EXPECT_TRUE (mgr.addDriver("a", a));
    EXPECT_FALSE(mgr.addDriver("a", b));   // id collision
    EXPECT_TRUE (mgr.addDriver("b", b));
}

TEST(OutputManager, AddDriverRejectsNullDriver) {
    OutputManager mgr;
    EXPECT_FALSE(mgr.addDriver("x", nullptr));
}

TEST(OutputManager, GetDriverReturnsRegistered) {
    OutputManager mgr;
    auto d = std::make_shared<CountingDriver>();
    mgr.addDriver("yt", d);
    EXPECT_EQ(mgr.getDriver("yt").get(), d.get());
    EXPECT_EQ(mgr.getDriver("missing"), nullptr);
}

TEST(OutputManager, RemoveDriverRemovesEntry) {
    OutputManager mgr;
    auto d = std::make_shared<CountingDriver>();
    mgr.addDriver("yt", d);
    EXPECT_TRUE (mgr.removeDriver("yt"));
    EXPECT_FALSE(mgr.removeDriver("yt"));   // already gone
    EXPECT_EQ(mgr.getDriver("yt"), nullptr);
}

TEST(OutputManager, SendFansOutToEveryDriver) {
    OutputManager mgr;
    auto a = std::make_shared<CountingDriver>();
    auto b = std::make_shared<CountingDriver>();
    auto c = std::make_shared<CountingDriver>();
    mgr.addDriver("a", a);
    mgr.addDriver("b", b);
    mgr.addDriver("c", c);

    Packet p;
    p.data = {1, 2, 3};
    for (int i = 0; i < 5; ++i) mgr.send(p);

    EXPECT_TRUE(waitFor([&] {
        return a->packets_ == 5 && b->packets_ == 5 && c->packets_ == 5;
    }));
}

TEST(OutputManager, AllHealthyTrueWhenEmpty) {
    OutputManager mgr;
    EXPECT_TRUE(mgr.allHealthy());
}

TEST(OutputManager, AllHealthyReflectsAggregate) {
    OutputManager mgr;
    auto a = std::make_shared<CountingDriver>();
    auto b = std::make_shared<CountingDriver>();
    mgr.addDriver("a", a);
    mgr.addDriver("b", b);
    EXPECT_TRUE(mgr.allHealthy());
    b->healthy_ = false;
    EXPECT_FALSE(mgr.allHealthy());
}

TEST(OutputManager, StatusJsonArrayWithIdField) {
    OutputManager mgr;
    auto a = std::make_shared<CountingDriver>();
    auto b = std::make_shared<CountingDriver>();
    mgr.addDriver("srt-headend", a);
    mgr.addDriver("rtmp-yt",     b);

    Packet p;
    mgr.send(p);
    ASSERT_TRUE(waitFor([&] { return a->packets_ == 1 && b->packets_ == 1; }));

    const auto j = mgr.statusJson();
    ASSERT_TRUE(j.is_array());
    ASSERT_EQ(j.size(), 2u);
    EXPECT_EQ(j[0]["id"], "srt-headend");
    EXPECT_EQ(j[1]["id"], "rtmp-yt");
    EXPECT_EQ(j[0]["transport"], "mock");
    EXPECT_EQ(j[0]["packets_sent"], 1);
    EXPECT_EQ(j[0]["queue_drops"], 0u);
}

TEST(OutputManager, ClearRemovesAll) {
    OutputManager mgr;
    mgr.addDriver("a", std::make_shared<CountingDriver>());
    mgr.addDriver("b", std::make_shared<CountingDriver>());
    mgr.clear();
    EXPECT_EQ(mgr.getDriver("a"), nullptr);
    EXPECT_EQ(mgr.getDriver("b"), nullptr);
    EXPECT_TRUE(mgr.statusJson().empty());
}

// fix12 c2: per-driver SPSC + pump thread.
// A slow driver must not block fan-out to faster siblings.
class SleepyDriver : public IOutput {
public:
    explicit SleepyDriver(std::chrono::milliseconds delay) : delay_(delay) {}
    void send(const Packet&) override {
        std::this_thread::sleep_for(delay_);
        ++packets_;
    }
    bool isHealthy() const override { return true; }
    OutputStats getStats() const override { return {}; }
    nlohmann::json statusJson() const override {
        return nlohmann::json{{"transport", "sleepy"}};
    }
    std::chrono::milliseconds delay_;
    std::atomic<int>          packets_{0};
};

TEST(OutputManager, SlowDriverDoesNotBlockOthers) {
    OutputManager mgr;
    auto slow = std::make_shared<SleepyDriver>(std::chrono::milliseconds(40));
    auto fast = std::make_shared<CountingDriver>();
    mgr.addDriver("slow", slow);
    mgr.addDriver("fast", fast);

    Packet p;
    constexpr int N = 30;
    for (int i = 0; i < N; ++i) mgr.send(p);

    // Fast pump drains immediately; slow is still working through its queue.
    ASSERT_TRUE(waitFor([&] { return fast->packets_ == N; }));
    EXPECT_LT(slow->packets_.load(), N);
}

// Blocking driver — pump thread parks inside send() until released, so the
// SPSC queue saturates and overflowing pushes get counted as drops.
class BlockingDriver : public IOutput {
public:
    void send(const Packet&) override {
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait(lk, [this] { return released_; });
        ++packets_;
    }
    bool isHealthy() const override { return true; }
    OutputStats getStats() const override { return {}; }
    nlohmann::json statusJson() const override {
        return nlohmann::json{{"transport", "blocking"}};
    }
    void release() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            released_ = true;
        }
        cv_.notify_all();
    }
    std::mutex              mu_;
    std::condition_variable cv_;
    bool                    released_ = false;
    std::atomic<int>        packets_{0};
};

TEST(OutputManager, DropsCountedWhenQueueOverflows) {
    auto stuck = std::make_shared<BlockingDriver>();
    {
        OutputManager mgr;
        mgr.addDriver("stuck", stuck);

        // Queue depth = 1024. The pump pulls one packet which then blocks
        // inside send(), so subsequent pushes accumulate until overflow.
        Packet p;
        for (int i = 0; i < 2000; ++i) mgr.send(p);

        const auto j = mgr.statusJson();
        ASSERT_EQ(j.size(), 1u);
        EXPECT_GT(j[0]["queue_drops"].get<uint64_t>(), 0u);

        // Release before destructor so the pump can exit cleanly.
        stuck->release();
    }
}

// fix12 c8: per-driver byte budget is enforced and surfaced via statusJson.

TEST(OutputManager, StatusJsonExposesByteBudget) {
    OutputManager mgr;
    auto d = std::make_shared<CountingDriver>();
    mgr.addDriver("d", d, 16384);
    const auto j = mgr.statusJson();
    ASSERT_EQ(j.size(), 1u);
    EXPECT_EQ(j[0]["queue_bytes_limit"].get<uint64_t>(), 16384u);
    EXPECT_EQ(j[0]["queue_bytes_used"].get<uint64_t>(),  0u);
}

TEST(OutputManager, ZeroLimitFallsBackToDefault) {
    OutputManager mgr;
    auto d = std::make_shared<CountingDriver>();
    mgr.addDriver("d", d, 0);  // 0 → default
    const auto j = mgr.statusJson();
    EXPECT_EQ(j[0]["queue_bytes_limit"].get<uint64_t>(),
              OutputManager::kDefaultQueueBytesLimit);
}

TEST(OutputManager, ByteLimitDropsOversizedPackets) {
    auto stuck = std::make_shared<BlockingDriver>();
    {
        OutputManager mgr;
        // 1 KiB budget, 256-byte packets → after ~5 packets every push
        // is dropped (one is in-flight inside the pump's send()).
        mgr.addDriver("stuck", stuck, /*queue_bytes_limit=*/1024);

        Packet p;
        p.data.assign(256, uint8_t{0});
        for (int i = 0; i < 200; ++i) mgr.send(p);

        const auto j = mgr.statusJson();
        ASSERT_EQ(j.size(), 1u);
        EXPECT_GT(j[0]["queue_drops"].get<uint64_t>(), 0u);
        // Used bytes never grew past the limit (with possible single-pkt
        // slop while the pump is mid-send).
        EXPECT_LE(j[0]["queue_bytes_used"].get<uint64_t>(), 1024u + 256u);

        stuck->release();
    }
}

TEST(OutputManager, BytesUsedDecreasesAfterPumpDrain) {
    OutputManager mgr;
    auto d = std::make_shared<CountingDriver>();
    mgr.addDriver("d", d, /*queue_bytes_limit=*/65536);

    Packet p;
    p.data.assign(512, uint8_t{0});
    for (int i = 0; i < 10; ++i) mgr.send(p);

    ASSERT_TRUE(waitFor([&] { return d->packets_ == 10; }));
    // After the pump drains, used bytes return to zero.
    EXPECT_TRUE(waitFor([&] {
        return mgr.statusJson()[0]["queue_bytes_used"].get<uint64_t>() == 0u;
    }));
}

} // namespace
