#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>
#include <utility>

#include "clips/IClip.h"
#include "clips/OnLossProvider.h"

using liveqx::OnLossSources;
using liveqx::makeOnLossProvider;

namespace {

// fix13 c6 — makeOnLossProvider helper. Verifies each mode dispatches
// to the right source and that misconfigured combinations throw at
// build time (the channel can't legally start with an invalid
// fallback config).

class StubClip : public IClip {
public:
    int video_calls = 0;
    int audio_calls = 0;
    Frame      getFrame() override { ++video_calls; Frame f; f.width = 8; f.height = 8;
        f.data = std::shared_ptr<std::uint8_t[]>(new std::uint8_t[256]()); return f; }
    AudioFrame getAudio(int n) override {
        ++audio_calls; AudioFrame af; af.num_samples = n;
        af.samples.assign(n * 2, 0.5f); af.valid = true; return af;
    }
    double getDuration() const override { return 1.0; }
    bool   hasAudio()  const override { return true; }
    bool   isPrepared() const override { return true; }
    void   prepare()        override {}
    void   release()        override {}
    Frame      getTailFrame()        override { return {}; }
    AudioFrame getTailAudio(int)     override { return {}; }
    void       reset()               override {}
};

TEST(OnLossProvider, FallbackClipMode) {
    auto stub = std::make_shared<StubClip>();
    OnLossSources s;
    s.fallback_clip = stub;
    auto p = makeOnLossProvider("fallback_clip", std::move(s));
    ASSERT_TRUE(p.video);
    ASSERT_TRUE(p.audio);

    auto f = p.video();
    EXPECT_TRUE(f.valid());
    EXPECT_EQ(stub->video_calls, 1);

    auto af = p.audio(480);
    EXPECT_TRUE(af.valid);
    EXPECT_EQ(stub->audio_calls, 1);
    EXPECT_EQ(af.num_samples, 480);
}

TEST(OnLossProvider, FallbackClipModeMissingClipThrows) {
    OnLossSources s;
    EXPECT_THROW(makeOnLossProvider("fallback_clip", std::move(s)),
                 std::invalid_argument);
}

TEST(OnLossProvider, FreezeMode) {
    int tail_calls = 0;
    OnLossSources s;
    s.tail_video = [&]() {
        ++tail_calls;
        Frame f; f.width = 16; f.height = 9;
        f.data = std::shared_ptr<std::uint8_t[]>(new std::uint8_t[16*9*4]());
        return f;
    };
    auto p = makeOnLossProvider("freeze", std::move(s));
    ASSERT_TRUE(p.video);
    ASSERT_TRUE(p.audio);

    auto f = p.video();
    EXPECT_TRUE(f.valid());
    EXPECT_EQ(tail_calls, 1);

    // Audio in freeze mode is silence — call shouldn't blow up
    auto af = p.audio(512);
    EXPECT_EQ(af.num_samples, 512);
    EXPECT_EQ(af.channels, 2);
    ASSERT_EQ(af.samples.size(), 1024u);
    for (float v : af.samples) EXPECT_EQ(v, 0.0f);
}

TEST(OnLossProvider, FreezeModeMissingTailThrows) {
    OnLossSources s;
    EXPECT_THROW(makeOnLossProvider("freeze", std::move(s)),
                 std::invalid_argument);
}

TEST(OnLossProvider, BlackMode) {
    OnLossSources s;
    s.black_width  = 64;
    s.black_height = 36;
    auto p = makeOnLossProvider("black", std::move(s));
    ASSERT_TRUE(p.video);
    ASSERT_TRUE(p.audio);

    auto f = p.video();
    ASSERT_TRUE(f.valid());
    EXPECT_EQ(f.width,  64);
    EXPECT_EQ(f.height, 36);
    const auto* pix = f.pixels();
    for (size_t i = 0; i < f.sizeBytes(); ++i) EXPECT_EQ(pix[i], 0);

    auto af = p.audio(256);
    EXPECT_EQ(af.num_samples, 256);
    for (float v : af.samples) EXPECT_EQ(v, 0.0f);
}

TEST(OnLossProvider, BlackModeDefaultsTo720p) {
    OnLossSources s;  // width/height defaults
    auto p = makeOnLossProvider("black", std::move(s));
    auto f = p.video();
    EXPECT_EQ(f.width,  1280);
    EXPECT_EQ(f.height, 720);
}

TEST(OnLossProvider, BlackModeReusesCachedFrame) {
    OnLossSources s;
    s.black_width  = 32;
    s.black_height = 18;
    auto p = makeOnLossProvider("black", std::move(s));

    auto f1 = p.video();
    auto f2 = p.video();
    // Same shared buffer — Frame is copy-cheap by design
    EXPECT_EQ(f1.pixels(), f2.pixels());
}

TEST(OnLossProvider, UnknownModeThrows) {
    OnLossSources s;
    EXPECT_THROW(makeOnLossProvider("",         std::move(s)), std::invalid_argument);
    OnLossSources s2;
    EXPECT_THROW(makeOnLossProvider("freezee",   std::move(s2)), std::invalid_argument);
    OnLossSources s3;
    EXPECT_THROW(makeOnLossProvider("fallback",  std::move(s3)), std::invalid_argument);
}

} // namespace
