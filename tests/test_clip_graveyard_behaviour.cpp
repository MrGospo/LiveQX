#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "clips/ClipGraveyard.h"
#include "clips/IClip.h"

namespace {

// Mock clip that simulates ~VideoClip's heavy stopThreads() with a
// configurable sleep. Records on which thread the destructor ran.
class HeavyClip : public IClip {
public:
    HeavyClip(std::atomic<int>&             destroyed,
              std::atomic<std::thread::id>& dtor_thread,
              std::chrono::milliseconds     dtor_cost)
        : destroyed_(destroyed),
          dtor_thread_(dtor_thread),
          dtor_cost_(dtor_cost) {}

    ~HeavyClip() override {
        std::this_thread::sleep_for(dtor_cost_);
        dtor_thread_.store(std::this_thread::get_id(),
                           std::memory_order_release);
        destroyed_.fetch_add(1, std::memory_order_acq_rel);
    }

    // IClip stubs.
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
    std::atomic<std::thread::id>& dtor_thread_;
    std::chrono::milliseconds     dtor_cost_;
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

// 1. The custom-deleter shared_ptr produced by wrapClip(uniq, graveyard) MUST
//    route the destructor through the graveyard worker. The drop site (the
//    "render thread" in production) returns within microseconds even when the
//    clip's destructor is itself slow.
TEST(ClipGraveyardBehaviour, LastSharedPtrDropOffloadsToGraveyard) {
    std::atomic<int>             destroyed{0};
    std::atomic<std::thread::id> dtor_tid{};

    ClipGraveyard graveyard;

    const auto caller_tid = std::this_thread::get_id();

    using Clock = std::chrono::steady_clock;
    Clock::duration drop_latency{0};

    {
        auto uniq = std::make_unique<HeavyClip>(destroyed, dtor_tid,
                                                std::chrono::milliseconds(60));
        auto sp   = wrapClip(std::move(uniq), graveyard);
        // Drop: this is the moment the production code hits inside
        // Preloader::tick. With graveyard wiring, the dtor must NOT run here.
        const auto t0 = Clock::now();
        sp.reset();
        drop_latency = Clock::now() - t0;
    }

    // The drop should be cheap — well under the 60ms dtor cost.
    EXPECT_LT(drop_latency, std::chrono::milliseconds(15))
        << "shared_ptr drop blocked on dtor — observed "
        << std::chrono::duration_cast<std::chrono::microseconds>(drop_latency)
               .count() << "us";

    // Eventually the graveyard worker finishes the dtor.
    ASSERT_TRUE(waitFor([&] { return destroyed.load() == 1; }));
    EXPECT_NE(dtor_tid.load(), caller_tid)
        << "destructor must run off the calling thread";
}

// 2. wrapClip with a vector — wrapClips() — produces a vector of
//    graveyard-routed shared_ptrs. Mass-drop (clearing the vector) is non-
//    blocking; every dtor lands on the graveyard worker.
TEST(ClipGraveyardBehaviour, WrapClipsBulkDropOffloads) {
    constexpr int kCount = 50;
    std::atomic<int>             destroyed{0};
    std::atomic<std::thread::id> dtor_tid{};

    ClipGraveyard graveyard;

    std::vector<std::unique_ptr<IClip>> sources;
    sources.reserve(kCount);
    for (int i = 0; i < kCount; ++i) {
        sources.emplace_back(std::make_unique<HeavyClip>(
            destroyed, dtor_tid, std::chrono::milliseconds(20)));
    }
    auto shared = wrapClips(std::move(sources), graveyard);
    EXPECT_EQ(shared.size(), static_cast<std::size_t>(kCount));

    using Clock = std::chrono::steady_clock;
    const auto t0 = Clock::now();
    shared.clear();           // last shared_ptr drops → bury × kCount
    const auto drop_latency = Clock::now() - t0;

    // 50 × 20ms = 1000ms if synchronous. Off-thread drop should be <50ms.
    EXPECT_LT(drop_latency, std::chrono::milliseconds(50))
        << "wrapClips drop blocked — observed "
        << std::chrono::duration_cast<std::chrono::microseconds>(drop_latency)
               .count() << "us";

    ASSERT_TRUE(waitFor([&] { return destroyed.load() == kCount; },
                        std::chrono::seconds(10)));
    EXPECT_NE(dtor_tid.load(), std::this_thread::get_id());
}

// 3. drainSync() blocks until every queued + in-flight dtor finishes.
//    Used by ChannelInstance::stop() to barrier the graveyard before the
//    next play() reuses file handles.
TEST(ClipGraveyardBehaviour, DrainSyncWaitsForInFlightDtors) {
    std::atomic<int>             destroyed{0};
    std::atomic<std::thread::id> dtor_tid{};

    ClipGraveyard graveyard;

    constexpr int kCount = 5;
    for (int i = 0; i < kCount; ++i) {
        auto uniq = std::make_unique<HeavyClip>(destroyed, dtor_tid,
                                                std::chrono::milliseconds(40));
        auto sp = wrapClip(std::move(uniq), graveyard);
        sp.reset();           // bury immediately
    }

    // After drainSync(), the post-condition is "no pending and no in-flight".
    graveyard.drainSync();
    EXPECT_EQ(destroyed.load(), kCount);
    EXPECT_EQ(graveyard.pendingCount(), 0u);
}
