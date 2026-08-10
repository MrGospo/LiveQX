#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>

#include "gateway/GatewayCfg.h"
#include "gateway/transcode/AudioTranscoder.h"
#include "gateway/transcode/ITranscodePipeline.h"
#include "gateway/transcode/VideoTranscoder.h"
#include "gateway/ts/PesAssembler.h"

namespace liveqx::gateway::transcode {

// fix40 A6 step 9b — concrete ITranscodePipeline that uses libavcodec
// (VideoTranscoder + AudioTranscoder) for decode→re-encode and
// gateway::ts::PesPacketizer for the inverse PES→TS pack.
//
// Loss FSM (commit 9b2) lives here too: video state machine moves
// Live → Freeze (after first missed input frame) → Fallback (after
// loss_grace_ms with input still down) → Live (on first arriving frame).
// Audio is simpler: Live → Silence → Live.
//
// All work is single-threaded; the gateway's IO thread drives feed*() and
// tick() in sequence and the pipeline never spawns threads of its own.
class FfmpegTranscodePipeline final : public ITranscodePipeline {
public:
    explicit FfmpegTranscodePipeline(TranscodeCfg cfg);
    ~FfmpegTranscodePipeline() override;

    FfmpegTranscodePipeline(const FfmpegTranscodePipeline&)            = delete;
    FfmpegTranscodePipeline& operator=(const FfmpegTranscodePipeline&) = delete;

    void onInputStreamTypes(std::uint8_t v_st, std::uint8_t a_st) override;
    void feedVideoPes(ts::PesPacket pes) override;
    void feedAudioPes(ts::PesPacket pes) override;
    void tick(std::chrono::steady_clock::time_point now) override;
    void flush() override;
    void setOutputSink(TsPacketSink sink) override;
    nlohmann::json statsJson() const override;

    // ── exposed for itests ────────────────────────────────────────────────
    bool videoDecoderOpen() const noexcept { return video_tc_.decoderOpen(); }
    bool audioDecoderOpen() const noexcept { return audio_tc_.decoderOpen(); }

    // Map a PMT stream_type (ISO/IEC 13818-1 Table 2-34) to a libavcodec
    // AV_CODEC_ID_*. Returns 0 (AV_CODEC_ID_NONE) for unsupported / 0.
    static int mapVideoStreamType(std::uint8_t st) noexcept;
    static int mapAudioStreamType(std::uint8_t st) noexcept;

private:
    enum class VideoState : std::uint8_t { Live, Freeze, Fallback };
    enum class AudioState : std::uint8_t { Live, Silence };

    void emitVideoPes(VideoOutFrame&& out);
    void emitAudioPes(AudioOutFrame&& out);
    void onVideoLossTickIfDue(std::chrono::steady_clock::time_point now);
    void onAudioLossTickIfDue(std::chrono::steady_clock::time_point now);
    void resetVideoStateOnRecovery(std::int64_t resumed_pts_90k);
    void resetAudioStateOnRecovery(std::int64_t resumed_pts_90k);

    TranscodeCfg                    cfg_;
    TsPacketSink                    sink_;

    VideoTranscoder                 video_tc_;
    AudioTranscoder                 audio_tc_;

    std::uint8_t                    curr_v_stream_type_ = 0;
    std::uint8_t                    curr_a_stream_type_ = 0;

    // Per-output-PID CC counters owned by the pipeline. The TS packets we
    // emit are passed through TranscodeGateway::appendPreparedPacket() —
    // the gateway no longer rewrites the CC field on these packets.
    std::uint8_t                    out_v_cc_ = 0;
    std::uint8_t                    out_a_cc_ = 0;

    // PesPacketizer holds a uint8_t& reference; declared after CC counters
    // and constructed in the initializer list (see .cpp).
    struct Packetizers;
    std::unique_ptr<Packetizers>    packetizers_;

    // ── Loss FSM ──────────────────────────────────────────────────────────
    VideoState                      v_state_ = VideoState::Live;
    AudioState                      a_state_ = AudioState::Live;
    std::chrono::steady_clock::time_point last_v_input_{};
    std::chrono::steady_clock::time_point last_a_input_{};
    std::chrono::steady_clock::time_point loss_started_v_{};

    // The next PTS to assign to a synthesised frame during loss. Advanced by
    // the cadence (90000/fps for V, 1024*90000/sr for A). When live input
    // resumes, both are snapped to whatever PTS the new frame carries.
    std::int64_t                    next_v_pts_90k_ = 0;
    std::int64_t                    next_a_pts_90k_ = 0;
    bool                            v_pts_seeded_   = false;
    bool                            a_pts_seeded_   = false;

    // Cadence reference for loss-mode emission. `next_*_emit_at_` is when
    // the next freeze/silence frame is due.
    std::chrono::steady_clock::time_point next_v_emit_at_{};
    std::chrono::steady_clock::time_point next_a_emit_at_{};

    bool                            fallback_loaded_attempted_ = false;
    bool                            fallback_loaded_ok_        = false;
    bool                            forced_keyframe_pending_   = false;

    // Stats
    std::uint64_t                   v_input_pes_           = 0;
    std::uint64_t                   a_input_pes_           = 0;
    std::uint64_t                   v_loss_transitions_    = 0;
    std::uint64_t                   a_loss_transitions_    = 0;
    std::uint64_t                   fallback_transitions_  = 0;
};

}  // namespace liveqx::gateway::transcode
