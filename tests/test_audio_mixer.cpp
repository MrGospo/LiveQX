#include <gtest/gtest.h>
#include "audio/AudioMixer.h"

TEST(AudioMixerTest, SilenceHasZeroSamples) {
    AudioMixer m;
    auto s = m.silence(1024);
    EXPECT_EQ(s.num_samples, 1024);
    for (float v : s.samples)
        EXPECT_FLOAT_EQ(v, 0.0f);
}

TEST(AudioMixerTest, CrossfadeAtZeroReturnsA) {
    AudioMixer m;
    AudioFrame a = m.silence(4);
    AudioFrame b = m.silence(4);
    for (auto& s : b.samples) s = 1.0f;
    auto out = m.crossfade(a, b, 0.0f);
    for (float v : out.samples)
        EXPECT_FLOAT_EQ(v, 0.0f);
}
