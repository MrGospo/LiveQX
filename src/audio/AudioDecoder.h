#pragma once
#include <memory>
#include <string>
#include "core/AudioFrame.h"

class AudioDecoder {
public:
    explicit AudioDecoder(const std::string& path);
    ~AudioDecoder();

    bool open();
    void close();

    AudioFrame decodeNext();
    bool atEnd() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
