#pragma once
#include <cstdint>
#include <vector>

struct AudioFrame {
    std::vector<float> samples;  // interleaved, stereo: L R L R ...
    int sample_rate = 48000;
    int channels = 2;
    int num_samples = 0;         // per channel
    int64_t pts = 0;             // presentation timestamp, microseconds
    bool valid = false;
    uint32_t epoch = 0;          // loop generation — used to discard pre-seek stale frames
};
