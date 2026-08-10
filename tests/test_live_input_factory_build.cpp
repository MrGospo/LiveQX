#include <gtest/gtest.h>

#include "clips/LiveInputClip.h"
#include "clips/LiveInputFactory.h"

namespace {

// fix13 c2 — integration tests for LiveInputFactory::build().
//
// build() pulls in MulticastInput / RtmpInput, which carry FFmpeg through
// their pimpls, so this test lives in liveqx-itests rather than
// the cheap unit target. We construct each concrete input via the factory
// and verify the resulting object satisfies the LiveInputClip contract —
// no actual network I/O happens (open/prepare are not called here).

TEST(LiveInputFactoryBuild, MulticastDispatchesToMulticastInput) {
    auto cfg = nlohmann::json{
        {"type",    "multicast"},
        {"address", "239.0.0.1"},
        {"port",    5000},
    };
    auto clip = liveqx::live_input::build(cfg, 1280, 720);
    ASSERT_NE(clip, nullptr);
    EXPECT_EQ(clip->clipType(), "live");
    EXPECT_FALSE(clip->isHealthy());        // not prepared yet → not healthy
    EXPECT_EQ(clip->lastPacketNs(), 0);     // never received a packet
    EXPECT_TRUE(std::isinf(clip->getDuration()));
}

TEST(LiveInputFactoryBuild, RtmpDispatchesToRtmpInput) {
    auto cfg = nlohmann::json{
        {"type", "rtmp"},
        {"url",  "rtmp://localhost/live"},
    };
    auto clip = liveqx::live_input::build(cfg, 1280, 720);
    ASSERT_NE(clip, nullptr);
    EXPECT_EQ(clip->clipType(), "live");
    EXPECT_FALSE(clip->isHealthy());
    EXPECT_TRUE(std::isinf(clip->getDuration()));
}

TEST(LiveInputFactoryBuild, RejectsBadMulticastSubCfg) {
    // Discriminator parses fine, but multicast::parseInputCfg rejects the
    // missing 'address' field — caller gets a useful diagnostic rather than
    // a half-constructed object.
    auto cfg = nlohmann::json{
        {"type", "multicast"},
        {"port", 5000},
    };
    EXPECT_THROW(liveqx::live_input::build(cfg, 1280, 720),
                 std::invalid_argument);
}

TEST(LiveInputFactoryBuild, RejectsBadRtmpSubCfg) {
    auto cfg = nlohmann::json{
        {"type", "rtmp"},
        {"url",  "http://wrong-scheme/x"},
    };
    EXPECT_THROW(liveqx::live_input::build(cfg, 1280, 720),
                 std::invalid_argument);
}

TEST(LiveInputFactoryBuild, NdiDispatchesToNdiInput) {
    // fix19 c8: NdiInput. The factory wiring closes the multicast<->NDI
    // bridge use case from the design doc — instead of a heavyweight
    // gateway.kind=ndi abstraction, an operator composes a normal Channel
    // with input.type=ndi (this clip) and output.type=multicast (or vice
    // versa, NDI output + multicast input). c9 is therefore satisfied by
    // dispatch coverage rather than a new gateway type.
    auto cfg = nlohmann::json{
        {"type",        "ndi"},
        {"source_name", "STUDIO-A (Camera 1)"},
    };
    auto clip = liveqx::live_input::build(cfg, 1280, 720);
    ASSERT_NE(clip, nullptr);
    EXPECT_EQ(clip->clipType(), "live");
    EXPECT_FALSE(clip->isHealthy());        // not prepared yet
    EXPECT_EQ(clip->lastPacketNs(), 0);
    EXPECT_TRUE(std::isinf(clip->getDuration()));
    auto j = clip->statusJson();
    ASSERT_TRUE(j.is_object());
    EXPECT_EQ(j.value("type", std::string{}), "ndi");
    EXPECT_EQ(j.value("source_name", std::string{}), "STUDIO-A (Camera 1)");
}

TEST(LiveInputFactoryBuild, RejectsBadNdiSubCfg) {
    // Missing source_name — discriminator parses but parseInputCfg throws.
    auto cfg = nlohmann::json{
        {"type",   "ndi"},
        {"groups", "Studio"},
    };
    EXPECT_THROW(liveqx::live_input::build(cfg, 1280, 720),
                 std::invalid_argument);
}

TEST(LiveInputFactoryBuild, BuiltClipResponsesToBaseSurface) {
    // Sanity check: the surface we formalized in c1 actually dispatches on
    // the concrete RtmpInput / MulticastInput overrides — hold the result
    // through LiveInputClip* and exercise every virtual.
    auto cfg = nlohmann::json{
        {"type", "rtmp"},
        {"url",  "rtmp://h/p"},
    };
    auto clip = liveqx::live_input::build(cfg, 320, 240);
    LiveInputClip* base = clip.get();
    base->setNumaNode(3);                    // no-op cosmetic, must not throw
    auto j = base->statusJson();
    EXPECT_TRUE(j.is_object());              // every input emits at least an object
    EXPECT_FALSE(base->isHealthy());
    EXPECT_EQ(base->lastPacketNs(), 0);
}

} // namespace
