#pragma once
#include "core/AudioFrame.h"

// Crossfade audio during transitions.
class AudioMixer {
public:
    AudioFrame crossfade(const AudioFrame& a, const AudioFrame& b, float progress);
    AudioFrame silence(int num_samples, int sample_rate = 48000, int channels = 2);
};
