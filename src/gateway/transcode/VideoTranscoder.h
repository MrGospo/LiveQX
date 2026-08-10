#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "gateway/GatewayCfg.h"

namespace liveqx::gateway::transcode {

// One output access unit produced by the encoder side. ES bytes are H.264 NAL
// units in Annex-B (start-code 0x00000001) form, ready to be wrapped into PES
// by the caller. PTS/DTS are in the MPEG-TS native 90 kHz clock so the caller
// can hand them straight to PesPacketizer without rate-conversion bookkeeping.
struct VideoOutFrame {
    std::vector<std::uint8_t> es;
    std::int64_t              pts_90khz   = 0;
    std::int64_t              dts_90khz   = 0;
    bool                      is_keyframe = false;
};

// Decode-and-re-encode primitive for the transcode gateway. The decoder is
// chosen at init() time by the input codec (PMT stream_type maps to one of
// AV_CODEC_ID_*); the encoder is libx264 (CPU H.264). Hardware backends from
// fix29 (NVENC/QSV/VAAPI) are out of scope for fix-A6 step 4 — they require
// AVHWFramesContext setup and a different decode path; the abstraction is
// designed so adding them later replaces only the encoder construction.
//
// Threading: not internally threaded. The owner (TranscodeGateway IO thread)
// drives both feed() and flush() and consumes output via the OutSink callback
// synchronously.
//
// Loss-protection (freeze-frame on input loss) is *not* implemented here —
// it's a property of the gateway, which decides when to stop calling feed()
// and start re-feeding the last decoded frame to the encoder. This class
// just owns the codec contexts.
class VideoTranscoder {
public:
    using OutSink = std::function<void(VideoOutFrame&&)>;

    explicit VideoTranscoder(TranscodeCfg cfg);
    ~VideoTranscoder();

    VideoTranscoder(const VideoTranscoder&) = delete;
    VideoTranscoder& operator=(const VideoTranscoder&) = delete;

    // Open the decoder for the given input codec. Pass an FFmpeg AV_CODEC_ID_*
    // constant — typical values: AV_CODEC_ID_H264 (PMT 0x1B), AV_CODEC_ID_HEVC
    // (PMT 0x24), AV_CODEC_ID_MPEG2VIDEO (PMT 0x02). Returns false if the
    // decoder cannot be opened (codec not registered, parameters invalid).
    //
    // The encoder is opened lazily on the first decoded frame so we know the
    // input dimensions for swscale setup.
    bool init(int input_avcodec_id);

    bool decoderOpen() const noexcept { return decoder_open_; }
    bool encoderOpen() const noexcept { return encoder_open_; }

    // Feed one PES packet's worth of ES bytes (one or more H.264 NAL units in
    // Annex-B form). PTS/DTS are in 90 kHz; missing values mean "decoder
    // chooses" and may produce frames with AV_NOPTS_VALUE that we forward
    // through. The sink fires zero or more times for each call (decoder may
    // accumulate frames before producing output).
    void feed(std::span<const std::uint8_t> es,
              std::optional<std::int64_t>   pts_90khz,
              std::optional<std::int64_t>   dts_90khz,
              const OutSink&                sink);

    // Drain remaining frames on EOF / shutdown (signals NULL packet to decoder
    // and NULL frame to encoder, then drains both).
    void flush(const OutSink& sink);

    // True once at least one decoded frame has been scaled into the
    // encoder-format buffer; gates emitFreeze().
    bool hasLastFrame() const noexcept;

    // Re-encode the most recently decoded frame at the supplied 90 kHz PTS.
    // Used by the gateway during input video starvation: holds the picture
    // for up to TranscodeCfg::loss_grace_ms while keeping the output stream
    // continuously framed at the configured fps. `force_keyframe` requests
    // an IDR so receivers that joined during the freeze can lock quickly —
    // typical pattern is to set it true on the first freeze frame of an
    // outage and let the encoder's GOP cadence handle subsequent ones.
    // No-op when hasLastFrame() is false.
    void emitFreeze(std::int64_t pts_90khz,
                    bool         force_keyframe,
                    const OutSink& sink);

    // Decode the still image at `path` (any container/codec libavformat can
    // open — typically PNG/JPEG) and cache it as the fallback freeze source.
    // The encoder must already be open: the gateway calls this only after the
    // first input frame has arrived, so encoder dims/pix-fmt are already set.
    // Returns false on any decode/scale error or if the encoder isn't open.
    bool loadFallbackImage(const std::string& path);

    bool hasFallback() const noexcept;

    // Re-encode the cached fallback image at the supplied 90 kHz PTS. Used
    // by the gateway after loss_grace_ms expires while input is still down —
    // the channel switches from "freeze last input" to "show logo". No-op
    // until loadFallbackImage() succeeds.
    void emitFallback(std::int64_t pts_90khz,
                      bool         force_keyframe,
                      const OutSink& sink);

    // ── stats ──────────────────────────────────────────────────────────────
    std::uint64_t framesIn()           const noexcept { return frames_in_; }
    std::uint64_t framesOut()          const noexcept { return frames_out_; }
    std::uint64_t freezeFramesOut()    const noexcept { return freeze_frames_out_; }
    std::uint64_t fallbackFramesOut()  const noexcept { return fallback_frames_out_; }
    std::uint64_t decodeErrors()       const noexcept { return decode_errors_; }
    std::uint64_t encodeErrors()       const noexcept { return encode_errors_; }
    std::uint64_t scaleErrors()        const noexcept { return scale_errors_; }

    int outputWidth()  const noexcept;
    int outputHeight() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    TranscodeCfg          cfg_;

    bool decoder_open_ = false;
    bool encoder_open_ = false;

    std::uint64_t frames_in_           = 0;
    std::uint64_t frames_out_          = 0;
    std::uint64_t freeze_frames_out_   = 0;
    std::uint64_t fallback_frames_out_ = 0;
    std::uint64_t decode_errors_       = 0;
    std::uint64_t encode_errors_       = 0;
    std::uint64_t scale_errors_        = 0;

    bool openEncoderForFrameDims(int input_w, int input_h, int input_pix_fmt);
    void drainDecoder(const OutSink& sink);
    void drainEncoder(const OutSink& sink);
};

}  // namespace liveqx::gateway::transcode
