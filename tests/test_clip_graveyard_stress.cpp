#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "clips/ClipGraveyard.h"
#include "clips/IClip.h"

namespace {

// HeavyClip — sleeps in dtor to mimic ~VideoClip's stopThreads() join cost.
class HeavyClip : public IClip {
public:
    HeavyClip(std::atomic<int>&         destroyed,
              std::chrono::milliseconds dtor_cost)
        : destroyed_(destroyed), dtor_cost_(dtor_cost) {}
    ~HeavyClip() override {
        std::this_thread::sleep_for(dtor_cost_);
        destroyed_.fetch_add(1, std::memory_order_acq_rel);
    }
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
    std::chrono::milliseconds     dtor_cost_;
};

}  // namespace

// Stress: simulate the production failure mode.
//
// In production, Preloader::tick() runs on the render thread at 25fps and
// performs assignments like `last_clip_ = cur_sp` that drop the previous
// shared_ptr. Without graveyard, the dtor (~VideoClip with 30-50ms join)
// runs on the render thread and blows the 40ms frame budget.
//
// This test simulates 5 seconds (125 ticks) of that pattern under raid
// conditions: every tick a fresh wrapClip-routed shared_ptr replaces the
// previous one, and the previous one's dtor cost is 35ms (typical
// VideoClip.release). The PER-TICK latency on the simulated render thread
// must stay well under the frame budget — proof that the graveyard does
// in fact offload the work.
TEST(ClipGraveyardStress, RenderThreadDoesNotBlockOnSequentialDrops) {
    constexpr int kTicks      = 125;       // 5 seconds @ 25fps
    constexpr auto kBudget    = std::chrono::milliseconds(40);
    constexpr auto kDtorCost  = std::chrono::milliseconds(35);

    std::atomic<int> destroyed{0};
    ClipGraveyard graveyard;

    std::shared_ptr<IClip> last_clip;   // simulates Preloader::last_clip_
    std::chrono::nanoseconds max_tick_latency{0};
    std::chrono::nanoseconds total_tick_latency{0};

    using Clock = std::chrono::steady_clock;

    for (int t = 0; t < kTicks; ++t) {
        auto fresh = wrapClip(std::make_unique<HeavyClip>(destroyed, kDtorCost),
                              graveyard);

        const auto t0 = Clock::now();
        // The dangerous moment: previous shared_ptr drops here.
        last_clip = std::move(fresh);
        const auto t1 = Clock::now();

        max_tick_latency  = std::max<std::chrono::nanoseconds>(
            max_tick_latency, t1 - t0);
        total_tick_latency += (t1 - t0);

        // Pace the simulated render loop at 25fps so the graveyard worker
        // has realistic time to chew through dtors. Without this, all 125
        // dtors land back-to-back and bury() can't show non-blocking
        // behaviour the way it does in production.
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }
    // Drop the final reference too.
    last_clip.reset();

    // Per-tick budget check — the whole point of the graveyard.
    EXPECT_LT(max_tick_latency, kBudget)
        << "render-thread tick blocked: max="
        << std::chrono::duration_cast<std::chrono::microseconds>(
               max_tick_latency).count() << "us";

    // Average should be tiny (microseconds). Sanity check that we didn't
    // pass the budget on average even if one spike fell under kBudget.
    const auto avg_us =
        std::chrono::duration_cast<std::chrono::microseconds>(
            total_tick_latency).count() / kTicks;
    EXPECT_LT(avg_us, 1000)
        << "render-thread average tick latency too high: " << avg_us << "us";

    // Drain & verify all dtors actually ran.
    graveyard.drainSync();
    EXPECT_EQ(destroyed.load(), kTicks);
}
