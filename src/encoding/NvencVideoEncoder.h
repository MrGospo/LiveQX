#pragma once

// fix29 c6: NvencVideoEncoder — IVideoEncoder backend over NVIDIA's
// h264_nvenc encoder. Compiled as a real backend when ENABLE_NVENC=ON
// (which sets LIVEQX_HAVE_NVENC). With the flag OFF the class is
// still defined and linkable, but open() returns false and logs a warning
// — that lets EncoderFactory unconditionally include the header without
// per-call #ifdef ladders.

#include <memory>

#include <spdlog/logger.h>

#include "encoding/IVideoEncoder.h"

namespace liveqx::encoding {

class NvencVideoEncoder final : public IVideoEncoder {
public:
    NvencVideoEncoder(const Config& cfg, std::shared_ptr<spdlog::logger> logger);
    ~NvencVideoEncoder() override;

    bool open() override;
    void close() override;
    void pushFrame(const Frame& bgra) override;
    void drain() override;

    void forceKeyframe() noexcept override;
    void setPacketCallback(PacketCallback cb) override;

    bool fillStreamParameters(AVCodecParameters* dst) const override;
    AVRational timeBase() const noexcept override;

    const char* name() const noexcept override { return "nvenc"; }

    // Compile-time probe so /api/system/gpu and EncoderFactory can decide
    // whether to even attempt this backend. True only when the binary was
    // built with ENABLE_NVENC=ON.
    static bool isBuiltIn() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace liveqx::encoding
