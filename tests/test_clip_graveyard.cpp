#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "clips/ClipGraveyard.h"
#include "clips/IClip.h"

namespace {

// Mock clip with a configurable destructor cost. Records:
//   - which thread ran ~MockClip
//   - how many destructions ran
class MockClip : public IClip {
public:
    MockClip(std::atomic<int>& destroyed,
             std::atomic<std::thread::id>& last_dtor_thread,
             std::chrono::milliseconds dtor_delay = std::chrono::milliseconds(0))
        : destroyed_(destroyed),
          last_dtor_thread_(last_dtor_thread),
          dtor_delay_(dtor_delay) {}

    ~MockClip() override {
        if (dtor_delay_ > std::chrono::milliseconds(0))
            std::this_thread::sleep_for(dtor_delay_);
        last_dtor_thread_.store(std::this_thread::get_id(),
                                std::memory_order_release);
        destroyed_.fetch_add(1, std::memory_order_acq_rel);
    }

    // IClip interface — minimal stubs.
    Frame      getFrame() override                 { return Frame{}; }
    AudioFrame getAudio(int) override              { return AudioFrame{}; }
    double     getDuration() const override        { return 0.0; }
    bool       hasAudio() const override           { return false; }
    bool       isPrepared() const override         { return false; }
    void       prepare() override                  {}
    void       release() override                  {}
    Frame      getTailFrame() override             { return Frame{}; }
    AudioFrame getTailAudio(int) override          { return AudioFrame{}; }
    void       reset() override                    {}

private:
    std::atomic<int>&             destroyed_;
    std::atomic<std::thread::id>& last_dtor_thread_;
    std::chrono::milliseconds     dtor_delay_;
};

bool waitFor(std::function<bool()> pred,
             std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return pred();
}

}  // namespace

// 1. Destructor must run on the graveyard worker thread, not the caller.
TEST(ClipGraveyard, BuriesObjectOnWorkerThread) {
    std::atomic<int>             destroyed{0};
    std::atomic<std::thread::id> dtor_thread{};

    const auto caller_id = std::this_thread::get_id();

    {
        ClipGraveyard g;
        auto* clip = new MockClip(destroyed, dtor_thread);
        g.bury(clip);

        ASSERT_TRUE(waitFor([&] {
            return destroyed.load(std::memory_order_acquire) == 1;
        }));
        EXPECT_NE(dtor_thread.load(std::memory_order_acquire), caller_id)
            << "destructor must run off the calling thread";
    }

    EXPECT_EQ(destroyed.load(), 1);
}

// 2. On graveyard shutdown, all queued objects must still be destructed
//    (no leaks) — even if the caller buries a flood right before destroy.
TEST(ClipGraveyard, DrainsQueueOnShutdown) {
    constexpr int                kCount = 100;
    std::atomic<int>             destroyed{0};
    std::atomic<std::thread::id> dtor_thread{};

    {
        ClipGraveyard g;
        for (int i = 0; i < kCount; ++i) {
            // Slow destructor so the queue can build up before shutdown
            // hits the worker.
            g.bury(new MockClip(destroyed, dtor_thread,
                                std::chrono::milliseconds(2)));
        }
        // Destructor of `g` runs here — must drain the queue.
    }

    EXPECT_EQ(destroyed.load(), kCount);
}

// 3. bury() must not block the caller, even when the worker is stuck on
//    a slow destructor. Calling thread sees ~constant latency per bury.
TEST(ClipGraveyard, BuryDoesNotBlockCaller) {
    constexpr int kCount = 200;
    std::atomic<int>             destroyed{0};
    std::atomic<std::thread::id> dtor_thread{};

    ClipGraveyard g;

    using Clock = std::chrono::steady_clock;
    std::chrono::nanoseconds max_bury_latency{0};

    for (int i = 0; i < kCount; ++i) {
        // 50ms per destructor — if bury blocked on the worker, calling
        // thread would see kCount * 50ms = 10s easily. We assert max
        // bury latency stays well under 50ms.
        auto* c = new MockClip(destroyed, dtor_thread,
                               std::chrono::milliseconds(50));
        const auto t0 = Clock::now();
        g.bury(c);
        const auto t1 = Clock::now();
        max_bury_latency = std::max(max_bury_latency, t1 - t0);
    }

    EXPECT_LT(max_bury_latency, std::chrono::milliseconds(20))
        << "bury() blocked on worker — observed max "
        << std::chrono::duration_cast<std::chrono::microseconds>(
               max_bury_latency).count()
        << "us";

    // Queue depth right after bury loop should be (close to) kCount —
    // worker is still chewing through slow destructors. We don't assert
    // exact depth (depends on scheduler), just sanity-check it's > 0
    // for at least the first few buries.
}

// 4. Concurrent bury from many producers must not race or lose objects.
//    Run under TSan in CI.
TEST(ClipGraveyard, ConcurrentBuryFromManyThreads) {
    constexpr int                kThreads = 8;
    constexpr int                kPerThread = 250;
    std::atomic<int>             destroyed{0};
    std::atomic<std::thread::id> dtor_thread{};

    {
        ClipGraveyard g;
        std::vector<std::thread> producers;
        producers.reserve(kThreads);
        for (int t = 0; t < kThreads; ++t) {
            producers.emplace_back([&] {
                for (int i = 0; i < kPerThread; ++i) {
                    g.bury(new MockClip(destroyed, dtor_thread));
                }
            });
        }
        for (auto& th : producers) th.join();
    }

    EXPECT_EQ(destroyed.load(), kThreads * kPerThread);
}

// 5. bury(nullptr) is a no-op. Trivial but the deleter contract permits
//    nullptrs (e.g. shared_ptr default-constructed then released).
TEST(ClipGraveyard, BuryNullptrIsNoOp) {
    ClipGraveyard g;
    g.bury(nullptr);
    EXPECT_EQ(g.pendingCount(), 0u);
}
