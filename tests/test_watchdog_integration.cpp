// Integration test: RenderLoop + BadClip → ChannelMetrics → ChannelHealth.
//
// Validates the chain of components implemented in fix/fix.md stage 2.7:
//   - rate-limited logs don't crash or block (smoke check)
//   - RollingCounter is fed by safeGetFrame/safeGetAudio drop paths
//   - heartbeat (last_tick_ns) is written every tick
//   - ChannelHealth transitions correctly from a real metrics snapshot
//
// We don't run a full 30s+ to observe Degraded→Failed promotion (covered by
// unit tests). This integration only proves that the components wire up.

#include <gtest/gtest.h>
#include <chrono>
#include <memory>
#include <thread>

#include "clips/IClip.h"
#include "core/RenderLoop.h"
#include "core/Timeline.h"
#include "encoding/Encoder.h"
#include "metrics/ChannelHealth.h"
#include "metrics/ChannelMetrics.h"
#include "render/ICompositor.h"

namespace {

// Returns invalid frames forever — RenderLoop should book drops/underruns.
class BadClip : public IClip {
public:
    Frame      getFrame()       override { return Frame{}; }                 // invalid
    AudioFrame getAudio(int)    override { return AudioFrame{}; }            // valid=false
    Frame      getTailFrame()   override { return Frame{}; }
    AudioFrame getTailAudio(int)override { return AudioFrame{}; }
    double     getDuration() const override { return 100.0; }
    bool       hasAudio()    const override { return true; }
    bool       isPrepared()  const override { return true; }
    void       prepare()           override {}
    void       release()           override {}
    void       reset()             override {}
};

class PassthroughCompositor : public ICompositor {
public:
    Frame composite(const Frame& a, const Frame&, TransitionType, float,
                    Easing = Easing::Linear) override {
        return a;
    }
};

}  // namespace

TEST(WatchdogIntegration, BadClipPopulatesRollingCountersAndHealthGoesDegraded) {
    using namespace std::chrono;

    Encoder::Config ecfg;
    ecfg.width = 2; ecfg.height = 2; ecfg.fps = 25;
    Encoder encoder(ecfg);
    encoder.open();

    Timeline               timeline;
    PassthroughCompositor  compositor;

    std::vector<std::unique_ptr<IClip>> playlist;
    playlist.push_back(std::make_unique<BadClip>());
    timeline.setPlaylist(std::move(playlist), {TransitionConfig{}});

    auto metrics = std::make_shared<ChannelMetrics>();
    RenderLoop loop(25, timeline, compositor, encoder, metrics);
    loop.setChannelId("test");
    loop.start();
    std::this_thread::sleep_for(milliseconds(1500));  // ~37 ticks
    loop.stop();

    const auto snap = metrics->snapshot();

    // 1. Heartbeat was written.
    EXPECT_GT(snap.last_tick_ns, 0);

    // 2. Both atomic and rolling drop counters incremented.
    EXPECT_GT(snap.frames_dropped, 0u);
    EXPECT_GT(snap.audio_underruns, 0u);
    EXPECT_GT(snap.drops_10s, 10u);     // > Degraded threshold
    EXPECT_GT(snap.underruns_10s, 0u);

    // 3. Health evaluator transitions Running → Degraded on this snapshot.
    //    now_ns chosen past the 10s grace; heartbeat_age=0 (fresh).
    ChannelHealth health("test", 25);
    health.evaluate(snap, 11LL * 1'000'000'000LL, 0);
    EXPECT_EQ(health.state(), HealthState::Degraded);
}

TEST(WatchdogIntegration, HealthyClipKeepsHealthRunning) {
    // A trivial valid clip — no drops, no underruns, health must stay Running
    // even past the grace period.
    using namespace std::chrono;

    class GoodClip : public IClip {
    public:
        GoodClip() {
            buf_ = std::shared_ptr<uint8_t[]>(new uint8_t[16]{});
        }
        Frame getFrame() override {
            Frame f; f.data = buf_; f.width = 2; f.height = 2; f.pts = 0;
            return f;
        }
        AudioFrame getAudio(int n) override {
            AudioFrame a;
            a.sample_rate = 48000; a.channels = 2; a.num_samples = n;
            a.samples.assign(static_cast<size_t>(n) * 2, 0.0f);
            a.valid = true;
            return a;
        }
        Frame      getTailFrame()       override { return getFrame(); }
        AudioFrame getTailAudio(int n)  override { return getAudio(n); }
        double     getDuration() const  override { return 100.0; }
        bool       hasAudio()    const  override { return true; }
        bool       isPrepared()  const  override { return true; }
        void       prepare()            override {}
        void       release()            override {}
        void       reset()              override {}
    private:
        std::shared_ptr<uint8_t[]> buf_;
    };

    Encoder::Config ecfg;
    ecfg.width = 2; ecfg.height = 2; ecfg.fps = 25;
    Encoder encoder(ecfg);
    encoder.open();

    Timeline timeline;
    PassthroughCompositor compositor;
    std::vector<std::unique_ptr<IClip>> playlist;
    playlist.push_back(std::make_unique<GoodClip>());
    timeline.setPlaylist(std::move(playlist), {TransitionConfig{}});

    auto metrics = std::make_shared<ChannelMetrics>();
    RenderLoop loop(25, timeline, compositor, encoder, metrics);
    loop.start();
    std::this_thread::sleep_for(milliseconds(800));
    loop.stop();

    const auto snap = metrics->snapshot();
    EXPECT_EQ(snap.frames_dropped,   0u);
    EXPECT_EQ(snap.audio_underruns,  0u);
    EXPECT_GT(snap.frames_rendered,  10u);

    // Within grace window only — fps_starved is suppressed for the first 10s
    // and a real 10s+ run is impractical here. Post-grace healthy state is
    // covered by the ChannelHealth unit tests.
    ChannelHealth health("test", 25);
    health.evaluate(snap, 0, 0);
    health.evaluate(snap, 5LL * 1'000'000'000LL, 0);
    EXPECT_EQ(health.state(), HealthState::Running);
}
