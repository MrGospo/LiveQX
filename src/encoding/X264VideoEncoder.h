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

    // Diagnostic: value actually programmed into the AVCodecContext after
    // open(). Exposed for tests that verify caller-side gop_size flows
    // through verbatim. Returns -1 if not opened.
    int effectiveGopSize() const noexcept;

    // Diagnostic: profile/level actually programmed into the H.264 SPS
    // after open(). libx264 does NOT populate AVCodecContext::profile/level —
    // those stay at AV_PROFILE_UNKNOWN/FF_LEVEL_UNKNOWN. To verify the
    // AVDictionary options really reached libx264, parse them out of the
    // SPS NAL in extradata (open() enables global_header for this).
    // Match AV_PROFILE_H264_* constants (BASELINE=66, MAIN=77, HIGH=100,
    // High Predictive 4:4:4=244). Level is byte value (3.1 → 31, 4.0 → 40).
    // Returns -1 if not opened or extradata missing/malformed.
    int effectiveProfileIdc() const noexcept;
    int effectiveLevelIdc()   const noexcept;

    // Diagnostic: rate-control values on the AVCodecContext after open().
    // CBR sets bit_rate == rc_max == rc_min. VBR sets bit_rate + rc_max
    // (rc_min == 0). CRF sets bit_rate == 0. Return -1 if not opened.
    int64_t effectiveBitrate()    const noexcept;
    int64_t effectiveMaxBitrate() const noexcept;
    int64_t effectiveMinBitrate() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace liveqx::encoding
