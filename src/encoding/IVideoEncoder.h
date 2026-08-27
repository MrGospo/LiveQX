#pragma once

// fix29 c3: pluggable video-encoder backend.
//
// Encoder (the facade) owns: SWS context (RGBA→YUV420P), audio AAC encoder,
// MPEG-TS muxer, AVIO write callback. For video it delegates to an
// IVideoEncoder implementation chosen at runtime (CPU x264 by default,
// NVENC/QSV/VAAPI when ENABLE_* CMake flags + matching hardware available).
//
// Contract:
//   - open(): allocate codec context, open codec, allocate AVFrame, return
//     true on success.
//   - pushFrame(): consume one BGRA Frame, produce zero or more AVPackets
//     via packet_cb (set by the facade — it forwards to the muxer).
//   - drain(): flush pending packets on close().
//   - forceKeyframe(): next pushFrame should emit IDR (atomic, callable
//     from any thread).
//   - fillStreamParameters(): copy codec params into AVCodecParameters
//     (used by avformat_new_stream after open()).
//   - timeBase(): codec context's time_base, used by muxer rescale.

#include <functional>
#include <memory>

#include "core/Frame.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/rational.h>
}

namespace liveqx::encoding {

class IVideoEncoder {
public:
    struct Config {
        int         width         = 1280;
        int         height        = 720;
        int         fps           = 25;
        int64_t     bitrate       = 4'000'000;
        std::string preset        = "medium";
        int         max_b_frames  = 0;
        // Distance between key-frames (I-frames), in frames. Zero means
        // "auto" — each backend picks its own historical default:
        //   x264/NVENC/QSV/VAAPI: fps (≈ 1 s at nominal rate),
        //   mpeg2video: max(fps/2, 6) (~ 0.5 s, DVB set-top expectation).
        // Positive values are honored verbatim (matching the max_b_frames
        // contract — no silent clamp). Typical broadcast/IPTV picks
        // 25..50 frames; OTT/HLS often uses 2×fps for seek-friendliness.
        int         gop_size      = 0;
        // H.264 stream constraint hints. Ignored by non-H.264 backends
        // (mpeg2video). Empty / zero mean "let the encoder pick".
        //   h264_profile — libavcodec profile name: "baseline", "main",
        //     "high", "high10", "high422", "high444". Middleware and
        //     set-top decoders often demand specific profiles: cheap
        //     hospitality decoders may only handle Main@3.1, while modern
        //     boxes can chew High@4.x. Wrong profile = decode failure.
        //   h264_level   — AVCC level integer (major*10 + minor, so
        //     3.1 → 31, 4.0 → 40, 4.1 → 41, 5.0 → 50). Level caps the
        //     max bitrate, buffer size, and frame dimensions that the
        //     decoder is required to support. Setting it too low for the
        //     chosen bitrate/resolution makes the stream non-compliant
        //     and some hardware decoders will refuse it.
        std::string h264_profile  = "";
        int         h264_level    = 0;
        // Whether the muxer requested AVFMT_GLOBALHEADER. Encoder must set
        // AV_CODEC_FLAG_GLOBAL_HEADER on the codec context before open()
        // when this is true (or extradata will be written inline and
        // formats like MP4 won't decode).
        bool        global_header = false;
        // GPU-only fields, ignored by CPU backends.
        int         gpu_index     = 0;
    };

    using PacketCallback = std::function<void(AVPacket*)>;

    virtual ~IVideoEncoder() = default;

    virtual bool open()                                                   = 0;
    virtual void close()                                                  = 0;
    virtual void pushFrame(const Frame& bgra)                             = 0;
    virtual void drain()                                                  = 0;

    virtual void forceKeyframe() noexcept                                 = 0;
    virtual void setPacketCallback(PacketCallback cb)                     = 0;

    // Stream parameters for muxer setup. Must be called after open().
    virtual bool fillStreamParameters(AVCodecParameters* dst) const       = 0;
    virtual AVRational timeBase() const noexcept                          = 0;

    // Diagnostic: short codec identifier, e.g. "x264", "nvenc", "qsv",
    // "vaapi". Used in logs and the /api/system/gpu endpoint.
    virtual const char* name() const noexcept                             = 0;
};

}  // namespace liveqx::encoding
