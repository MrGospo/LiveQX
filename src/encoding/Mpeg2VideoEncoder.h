#pragma once

// Mpeg2VideoEncoder — IVideoEncoder backend over libavcodec's mpeg2video
// (CPU MPEG-2 Video). Selected when Encoder::Config.video_codec ==
// "mpeg2video". Same wrap-only shape as X264VideoEncoder: FFmpeg-side
// state lives in the .cpp's anonymous namespace.
//
// Rationale: legacy hospitality set-top boxes and some low-cost IPTV
// receivers only decode MPEG-2 reliably. MPEG-2 is CPU-only in this
// backend — GPU MPEG-2 encoders (mpeg2_qsv, mpeg2_vaapi) exist but are
// rare in practice, so we keep the surface minimal.

#include <memory>

#include <spdlog/logger.h>

#include "encoding/IVideoEncoder.h"

namespace liveqx::encoding {

class Mpeg2VideoEncoder final : public IVideoEncoder {
public:
    Mpeg2VideoEncoder(const Config& cfg, std::shared_ptr<spdlog::logger> logger);
    ~Mpeg2VideoEncoder() override;

    bool open() override;
    void close() override;
    void pushFrame(const Frame& bgra) override;
    void drain() override;

    void forceKeyframe() noexcept override;
    void setPacketCallback(PacketCallback cb) override;

    bool fillStreamParameters(AVCodecParameters* dst) const override;
    AVRational timeBase() const noexcept override;

    const char* name() const noexcept override { return "mpeg2video"; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace liveqx::encoding
