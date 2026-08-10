#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "gateway/GatewayCfg.h"

namespace liveqx::gateway::transcode {

// One output access unit for audio. The ES bytes are AAC frames in ADTS
// framing (7-byte header per frame), ready for PesPacketizer wrapping.
// Multiple ADTS frames may be concatenated when the encoder buffers more than
// one frame per drain — but in practice with FFmpeg native AAC and 1024-sample
// frames at typical rates we get one ADTS frame per output AVPacket.
struct AudioOutFrame {
    std::vector<std::uint8_t> es;
    std::int64_t              pts_90khz = 0;
};

// AAC-in / AAC-out audio re-encoder. Owns one AAC decoder context, one
// libswresample context (for sample-rate / channel-layout adaptation between
// decoded PCM and the output AAC encoder), and one AAC encoder context. Output
// AAC frames are emitted in ADTS framing (a 7-byte header is prepended to each
// raw AAC packet) so downstream PES packetisation matches MPEG-TS stream_type
// 0x0F conventions.
//
// The encoder is opened lazily on the first decoded frame because AAC frame
// size (1024 samples for LC) and channel layout aren't known until the decoder
// has parsed at least one ADTS frame.
class AudioTranscoder {
public:
    using OutSink = std::function<void(AudioOutFrame&&)>;

    explicit AudioTranscoder(TranscodeCfg cfg);
    ~AudioTranscoder();

    AudioTranscoder(const AudioTranscoder&) = delete;
    AudioTranscoder& operator=(const AudioTranscoder&) = delete;

    // input_avcodec_id: AV_CODEC_ID_AAC (PMT 0x0F or 0x11), AV_CODEC_ID_MP3,
    // AV_CODEC_ID_AC3, etc. Returns false if the decoder cannot be opened.
    bool init(int input_avcodec_id);

    bool decoderOpen() const noexcept { return decoder_open_; }
    bool encoderOpen() const noexcept { return encoder_open_; }

    void feed(std::span<const std::uint8_t> es,
              std::optional<std::int64_t>   pts_90khz,
              const OutSink&                sink);

    void flush(const OutSink& sink);

    // Encode one frame_size (typically 1024 samples) of digital silence at the
    // supplied 90 kHz PTS. Used by the gateway during input audio starvation
    // to keep the audio ES continuous while loss_grace_ms hasn't expired.
    // No-op until encoderOpen() — the encoder needs sr/channel layout from
    // the first decoded input frame to bring up the resampler/AAC encoder.
    void emitSilence(std::int64_t pts_90khz, const OutSink& sink);

    std::uint64_t framesIn()         const noexcept { return frames_in_; }
    std::uint64_t framesOut()        const noexcept { return frames_out_; }
    std::uint64_t silenceFramesOut() const noexcept { return silence_frames_out_; }
    std::uint64_t decodeErrors()     const noexcept { return decode_errors_; }
    std::uint64_t encodeErrors()     const noexcept { return encode_errors_; }
    std::uint64_t resampleErrors()   const noexcept { return resample_errors_; }

    int outputSampleRate() const noexcept;
    int outputChannels()   const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    TranscodeCfg          cfg_;

    bool decoder_open_ = false;
    bool encoder_open_ = false;

    std::uint64_t frames_in_          = 0;
    std::uint64_t frames_out_         = 0;
    std::uint64_t silence_frames_out_ = 0;
    std::uint64_t decode_errors_      = 0;
    std::uint64_t encode_errors_      = 0;
    std::uint64_t resample_errors_    = 0;

    bool openEncoderForInput(int input_sample_rate, int input_channels);
    void drainDecoder(const OutSink& sink);
    void encodeFromFifo(const OutSink& sink, bool flush);
    void emitEncodedPacket(const std::uint8_t* data, int size, std::int64_t pts_90k,
                           const OutSink& sink);
};

// Build a 7-byte ADTS header for a raw AAC packet of `aac_payload_len` bytes.
// `profile_minus_one` is the AAC object type minus one (LC = 1, so pass 1 for
// LC). `sample_rate_idx` is the index into the AAC sample-rate table.
//
// Exposed for unit tests and for the gateway's silence-frame generator (step 7)
// which needs to wrap a hard-coded silence AAC bitstream in ADTS too.
void writeAdtsHeader(std::uint8_t* out_7,
                     std::uint8_t  profile_minus_one,
                     std::uint8_t  sample_rate_idx,
                     std::uint8_t  channel_cfg,
                     std::size_t   aac_payload_len) noexcept;

// Sample-rate index lookup. Returns 0xFF when the rate is not in the table.
std::uint8_t aacSampleRateIndex(int sample_rate) noexcept;

}  // namespace liveqx::gateway::transcode
