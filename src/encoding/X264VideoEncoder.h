#pragma once

// fix29 c4: X264VideoEncoder — IVideoEncoder backend over libx264 (CPU H.264).
// Default backend when encoder_mode = "cpu" (or unset). Wrap-only-class —
// no public state, the entire FFmpeg-side machinery is hidden in the .cpp's
// anonymous namespace.

#include <memory>

#include <spdlog/logger.h>

#include "encoding/IVideoEncoder.h"

namespace liveqx::encoding {

class X264VideoEncoder final : public IVideoEncoder {
public:
    X264VideoEncoder(const Config& cfg, std::shared_ptr<spdlog::logger> logger);
    ~X264VideoEncoder() override;

    bool open() override;
    void close() override;
    void pushFrame(const Frame& bgra) override;
    void drain() override;

    void forceKeyframe() noexcept override;
    void setPacketCallback(PacketCallback cb) override;

    bool fillStreamParameters(AVCodecParameters* dst) const override;
    AVRational timeBase() const noexcept override;

    const char* name() const noexcept override { return "x264"; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace liveqx::encoding
