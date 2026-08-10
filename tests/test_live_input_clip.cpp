#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>

#include "clips/LiveInputClip.h"

namespace {

// Minimal concrete subclass — exercises the LiveInputClip defaults.
// Concrete drivers (multicast/RTMP/RTSP) will fill these with real network
// I/O, but for testing the base contract we only need stubs.
class FakeLiveInputClip : public LiveInputClip {
public:
    Frame      getFrame() override                        { return Frame{}; }
    AudioFrame getAudio(int /*num_samples*/) override     { return AudioFrame{}; }
    bool       hasAudio() const override                  { return false; }
    bool       isPrepared() const override                { return prepared_; }
    void       prepare() override                         { prepared_ = true; }
    void       release() override                         { prepared_ = false; }

    // ---- fix13 c1: ILiveInput surface stubs ----
    bool         isHealthy() const override               { return healthy_; }
    std::int64_t lastPacketNs() const noexcept override   { return last_packet_ns_; }
    void         setNumaNode(int node) noexcept override  { numa_node_ = node; }
    nlohmann::json statusJson() const override            {
        return nlohmann::json{
            {"transport",  "fake"},
            {"healthy",    healthy_},
            {"numa_node",  numa_node_},
        };
    }

    // Test hooks: drivers update last_frame_ from their decode thread.
    void publishTail(Frame f) { last_frame_ = std::move(f); }
    void setHealthy(bool h)              { healthy_ = h; }
    void setLastPacketNs(std::int64_t v) { last_packet_ns_ = v; }
    int  numaNode() const                { return numa_node_; }

private:
    bool         prepared_       = false;
    bool         healthy_        = false;
    std::int64_t last_packet_ns_ = 0;
    int          numa_node_      = -1;
};

// Live sources are infinite — Timeline uses isinf() to skip duration boundaries.
TEST(LiveInputClipTest, DurationIsInfinite) {
    FakeLiveInputClip clip;
    EXPECT_TRUE(std::isinf(clip.getDuration()));
}

// clipType() identifies live sources for proof-of-play logging.
TEST(LiveInputClipTest, ClipTypeIsLive) {
    FakeLiveInputClip clip;
    EXPECT_EQ(clip.clipType(), "live");
}

// reset() must be a no-op — there is nothing to rewind in a network stream.
TEST(LiveInputClipTest, ResetIsNoop) {
    FakeLiveInputClip clip;
    clip.prepare();
    clip.reset();
    EXPECT_TRUE(clip.isPrepared());
}

// getTailFrame() returns whatever the driver published into last_frame_.
TEST(LiveInputClipTest, TailFrameMirrorsLastFrame) {
    FakeLiveInputClip clip;
    EXPECT_FALSE(clip.getTailFrame().valid());

    Frame f;
    f.width  = 1280;
    f.height = 720;
    f.data   = std::shared_ptr<uint8_t[]>(new uint8_t[1280 * 720 * 4]());
    clip.publishTail(f);

    Frame tail = clip.getTailFrame();
    EXPECT_TRUE(tail.valid());
    EXPECT_EQ(tail.width,  1280);
    EXPECT_EQ(tail.height, 720);
}

// getTailAudio() returns silence of the requested length — the broadcast
// crossfade has *something* to fade to even with no recent audio.
TEST(LiveInputClipTest, TailAudioReturnsSilence) {
    FakeLiveInputClip clip;
    AudioFrame af = clip.getTailAudio(1920);
    EXPECT_TRUE(af.valid);
    EXPECT_EQ(af.num_samples, 1920);
    EXPECT_EQ(af.samples.size(), static_cast<size_t>(1920) * af.channels);
    bool all_zero = std::all_of(af.samples.begin(), af.samples.end(),
                                [](float v) { return v == 0.0f; });
    EXPECT_TRUE(all_zero);
}

// ---- fix13 c1: ILiveInput surface contract ----
//
// LiveClip (c3) and ILiveInputFactory (c2) interact with concrete drivers
// only through the LiveInputClip base pointer. These tests pin the
// virtual dispatch contract: every method must be reachable polymorphically
// and return what the concrete driver wrote.

TEST(LiveInputClipTest, IsHealthyDispatchesPolymorphically) {
    FakeLiveInputClip   clip;
    LiveInputClip*      base = &clip;
    EXPECT_FALSE(base->isHealthy());
    clip.setHealthy(true);
    EXPECT_TRUE(base->isHealthy());
}

TEST(LiveInputClipTest, LastPacketNsDispatchesPolymorphically) {
    FakeLiveInputClip   clip;
    LiveInputClip*      base = &clip;
    EXPECT_EQ(base->lastPacketNs(), 0);
    clip.setLastPacketNs(123'456'789);
    EXPECT_EQ(base->lastPacketNs(), 123'456'789);
}

TEST(LiveInputClipTest, SetNumaNodeDispatchesPolymorphically) {
    FakeLiveInputClip   clip;
    LiveInputClip*      base = &clip;
    base->setNumaNode(2);
    EXPECT_EQ(clip.numaNode(), 2);
}

TEST(LiveInputClipTest, StatusJsonDispatchesPolymorphically) {
    FakeLiveInputClip   clip;
    clip.setHealthy(true);
    clip.setNumaNode(7);
    LiveInputClip* base = &clip;
    auto j = base->statusJson();
    EXPECT_EQ(j.value("transport", ""),  "fake");
    EXPECT_EQ(j.value("healthy",   false), true);
    EXPECT_EQ(j.value("numa_node", -1),  7);
}

} // namespace
