#pragma once

// fix29 c9: QsvVideoEncoder — IVideoEncoder over Intel Quick Sync
// (h264_qsv via VAAPI/MFX). Same compile-time gating model as NvencVideoEncoder:
// the class always links, but real implementation is behind
// LIVEQX_HAVE_QSV; otherwise open() returns false with a warning.

#include <memory>

#include <spdlog/logger.h>

#include "encoding/IVideoEncoder.h"

namespace liveqx::encoding {

class QsvVideoEncoder final : public IVideoEncoder {
public:
    QsvVideoEncoder(const Config& cfg, std::shared_ptr<spdlog::logger> logger);
    ~QsvVideoEncoder() override;

    bool open() override;
    void close() override;
    void pushFrame(const Frame& bgra) override;
    void drain() override;

    void forceKeyframe() noexcept override;
    void setPacketCallback(PacketCallback cb) override;

    bool fillStreamParameters(AVCodecParameters* dst) const override;
    AVRational timeBase() const noexcept override;

    const char* name() const noexcept override { return "qsv"; }

    static bool isBuiltIn() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace liveqx::encoding
