#pragma once

// fix29 c10: VaapiVideoEncoder — IVideoEncoder over VAAPI (h264_vaapi).
// Same compile-gating model as the other GPU backends; behind
// LIVEQX_HAVE_VAAPI when ENABLE_VAAPI=ON, otherwise a graceful
// "not built in" stub.

#include <memory>

#include <spdlog/logger.h>

#include "encoding/IVideoEncoder.h"

namespace liveqx::encoding {

class VaapiVideoEncoder final : public IVideoEncoder {
public:
    VaapiVideoEncoder(const Config& cfg, std::shared_ptr<spdlog::logger> logger);
    ~VaapiVideoEncoder() override;

    bool open() override;
    void close() override;
    void pushFrame(const Frame& bgra) override;
    void drain() override;

    void forceKeyframe() noexcept override;
    void setPacketCallback(PacketCallback cb) override;

    bool fillStreamParameters(AVCodecParameters* dst) const override;
    AVRational timeBase() const noexcept override;

    const char* name() const noexcept override { return "vaapi"; }

    static bool isBuiltIn() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace liveqx::encoding
