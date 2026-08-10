#pragma once
#include <memory>
#include "core/AudioFrame.h"
#include "output/IOutput.h"

// Encodes AudioFrame to AAC and produces Packet(s).
class AudioEncoder {
public:
    AudioEncoder(int bitrate = 128000, int sample_rate = 48000, int channels = 2);
    ~AudioEncoder();

    bool open();
    void close();

    Packet encode(const AudioFrame& frame);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    int bitrate_;
    int sample_rate_;
    int channels_;
};
