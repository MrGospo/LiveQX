#include "gateway/transcode/FfmpegTranscodePipeline.h"

#include <algorithm>
#include <utility>

#include "gateway/ts/PesAssembler.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace liveqx::gateway::transcode {

using ts::PesPacket;
using ts::PesPacketizer;

namespace {

// PES stream_id values per ISO/IEC 13818-1 §2.4.3.7.
//   0xE0..0xEF — video streams (we use 0xE0)
//   0xC0..0xDF — audio streams (we use 0xC0)
constexpr std::uint8_t kVideoStreamId = 0xE0;
constexpr std::uint8_t kAudioStreamId = 0xC0;

// 25 fps default — 90000/25 = 3600 ticks per frame.
std::int64_t ticksPerVideoFrame90k(int fps) noexcept {
    return (fps > 0) ? 90000 / fps : 3600;
}

// AAC LC frame size at output sample rate (1024 samples) translated to 90k.
std::int64_t ticksPerAudioFrame90k(int sample_rate) noexcept {
    return (sample_rate > 0) ? 1024LL * 90000LL / sample_rate : 1920;
}

}  // namespace

// PesPacketizer holds a reference to the CC byte. We keep the packetizer
// instances behind a pimpl so the FfmpegTranscodePipeline header doesn't
// have to drag in PesAssembler.h transitively (it's already included for
// PesPacket — but the packetizer is impl detail).
struct FfmpegTranscodePipeline::Packetizers {
    PesPacketizer video;
    PesPacketizer audio;

    Packetizers(std::uint16_t v_pid, std::uint8_t& v_cc,
                std::uint16_t a_pid, std::uint8_t& a_cc,
                PesPacketizer::PacketSink v_sink,
                PesPacketizer::PacketSink a_sink) noexcept
        : video(v_pid, v_cc, std::move(v_sink))
        , audio(a_pid, a_cc, std::move(a_sink)) {}
};

// ── ctor / dtor ─────────────────────────────────────────────────────────────

FfmpegTranscodePipeline::FfmpegTranscodePipeline(TranscodeCfg cfg)
    : cfg_(std::move(cfg))
    , video_tc_(cfg_)
    , audio_tc_(cfg_) {
    auto v_sink = [this](const std::uint8_t* p, std::size_t n) {
        if (sink_ && n == 188) sink_(std::span<const std::uint8_t>(p, n));
    };
    auto a_sink = v_sink;  // identical wrapper
    packetizers_ = std::make_unique<Packetizers>(
        cfg_.video_pid, out_v_cc_,
        cfg_.audio_pid, out_a_cc_,
        std::move(v_sink), std::move(a_sink));
}

FfmpegTranscodePipeline::~FfmpegTranscodePipeline() = default;

// ── Stream-type → AV_CODEC_ID mapping ───────────────────────────────────────

int FfmpegTranscodePipeline::mapVideoStreamType(std::uint8_t st) noexcept {
    switch (st) {
        case 0x01:
        case 0x02: return AV_CODEC_ID_MPEG2VIDEO;
        case 0x10: return AV_CODEC_ID_MPEG4;
        case 0x1B: return AV_CODEC_ID_H264;
        case 0x24: return AV_CODEC_ID_HEVC;
        default:   return AV_CODEC_ID_NONE;
    }
}

int FfmpegTranscodePipeline::mapAudioStreamType(std::uint8_t st) noexcept {
    switch (st) {
        case 0x03:
        case 0x04: return AV_CODEC_ID_MP3;
        case 0x0F:
        case 0x11: return AV_CODEC_ID_AAC;
        case 0x81: return AV_CODEC_ID_AC3;
        case 0x06: return AV_CODEC_ID_AC3;  // private — best-effort guess
        default:   return AV_CODEC_ID_NONE;
    }
}

// ── ITranscodePipeline ──────────────────────────────────────────────────────

void FfmpegTranscodePipeline::setOutputSink(TsPacketSink sink) {
    sink_ = std::move(sink);
}

void FfmpegTranscodePipeline::onInputStreamTypes(std::uint8_t v_st,
                                                 std::uint8_t a_st) {
    if (v_st != curr_v_stream_type_) {
        curr_v_stream_type_ = v_st;
        if (v_st != 0) {
            const int id = mapVideoStreamType(v_st);
            if (id != AV_CODEC_ID_NONE) {
                // VideoTranscoder.init() opens a fresh decoder; if a previous
                // decoder is still open the call is a no-op (returns false).
                // For runtime stream-type changes the pipeline should ideally
                // be reconstructed; for now, first-call wins and subsequent
                // changes are ignored upstream by the gateway (input PMT
                // change → reset of PesAssembler resyncs PUSI but the
                // transcoder keeps its existing decoder).
                video_tc_.init(id);
            }
        }
    }
    if (a_st != curr_a_stream_type_) {
        curr_a_stream_type_ = a_st;
        if (a_st != 0) {
            const int id = mapAudioStreamType(a_st);
            if (id != AV_CODEC_ID_NONE) audio_tc_.init(id);
        }
    }
}

void FfmpegTranscodePipeline::feedVideoPes(PesPacket pes) {
    if (!video_tc_.decoderOpen()) return;
    ++v_input_pes_;
    last_v_input_ = std::chrono::steady_clock::now();

    if (v_state_ != VideoState::Live) {
        // Recovery — snap PTS to the resumed input.
        if (pes.pts_90khz.has_value())
            resetVideoStateOnRecovery(*pes.pts_90khz);
        else
            v_state_ = VideoState::Live;
    }

    video_tc_.feed(
        std::span<const std::uint8_t>(pes.es.data(), pes.es.size()),
        pes.pts_90khz, pes.dts_90khz,
        [this](VideoOutFrame&& out) { emitVideoPes(std::move(out)); });
}

void FfmpegTranscodePipeline::feedAudioPes(PesPacket pes) {
    if (!audio_tc_.decoderOpen()) return;
    ++a_input_pes_;
    last_a_input_ = std::chrono::steady_clock::now();

    if (a_state_ != AudioState::Live) {
        if (pes.pts_90khz.has_value())
            resetAudioStateOnRecovery(*pes.pts_90khz);
        else
            a_state_ = AudioState::Live;
    }

    audio_tc_.feed(
        std::span<const std::uint8_t>(pes.es.data(), pes.es.size()),
        pes.pts_90khz,
        [this](AudioOutFrame&& out) { emitAudioPes(std::move(out)); });
}

void FfmpegTranscodePipeline::tick(std::chrono::steady_clock::time_point now) {
    onVideoLossTickIfDue(now);
    onAudioLossTickIfDue(now);
}

void FfmpegTranscodePipeline::flush() {
    video_tc_.flush(
        [this](VideoOutFrame&& out) { emitVideoPes(std::move(out)); });
    audio_tc_.flush(
        [this](AudioOutFrame&& out) { emitAudioPes(std::move(out)); });
}

// ── Output: wrap encoded ES into PES → TS ───────────────────────────────────

void FfmpegTranscodePipeline::emitVideoPes(VideoOutFrame&& out) {
    if (!packetizers_) return;
    PesPacket pes;
    pes.stream_id = kVideoStreamId;
    pes.pts_90khz = out.pts_90khz;
    // For MPEG-TS H.264 video we always emit PTS+DTS so receivers don't have
    // to infer DTS = PTS from absence of the field. With max_b_frames=0 the
    // two are equal, but with reorder enabled the encoder gives us distinct
    // values and the PES carries them faithfully.
    pes.dts_90khz = out.dts_90khz;
    pes.es        = std::move(out.es);
    packetizers_->video.emit(pes);

    // Track the next-PTS counter so a subsequent loss tick advances from the
    // last live frame's PTS.
    next_v_pts_90k_ = out.pts_90khz + ticksPerVideoFrame90k(cfg_.video_fps);
    v_pts_seeded_   = true;
}

void FfmpegTranscodePipeline::emitAudioPes(AudioOutFrame&& out) {
    if (!packetizers_) return;
    PesPacket pes;
    pes.stream_id = kAudioStreamId;
    pes.pts_90khz = out.pts_90khz;
    pes.dts_90khz = std::nullopt;        // audio: no reordering, PTS-only
    pes.es        = std::move(out.es);
    packetizers_->audio.emit(pes);

    next_a_pts_90k_ = out.pts_90khz + ticksPerAudioFrame90k(cfg_.audio_sample_rate);
    a_pts_seeded_   = true;
}

// ── Loss FSM (commit 9b — wired here so the pipeline tick drives it) ────────

void FfmpegTranscodePipeline::onVideoLossTickIfDue(
        std::chrono::steady_clock::time_point now) {
    if (!cfg_.freeze_on_video_loss) return;
    if (!video_tc_.encoderOpen()) return;        // nothing to emit yet
    if (last_v_input_ == std::chrono::steady_clock::time_point{}) return;

    using namespace std::chrono;
    const auto idle_ms = duration_cast<milliseconds>(now - last_v_input_).count();
    // The encoder cadence is video_fps; if input arrives within one frame we
    // assume it's still alive. The loss threshold is one missed frame.
    const auto frame_period_ms = (cfg_.video_fps > 0) ? (1000 / cfg_.video_fps) : 40;
    if (idle_ms < frame_period_ms * 2) {
        // Still live.
        if (v_state_ != VideoState::Live) v_state_ = VideoState::Live;
        return;
    }

    // Decide target loss state.
    VideoState target = VideoState::Freeze;
    if (idle_ms >= static_cast<long long>(cfg_.loss_grace_ms)) {
        // Lazy-load the fallback image once.
        if (!fallback_loaded_attempted_) {
            fallback_loaded_attempted_ = true;
            if (!cfg_.fallback_logo_path.empty()) {
                fallback_loaded_ok_ =
                    video_tc_.loadFallbackImage(cfg_.fallback_logo_path);
            }
        }
        if (fallback_loaded_ok_ && video_tc_.hasFallback()) {
            target = VideoState::Fallback;
        }
    }
    if (target != v_state_) {
        v_state_ = target;
        forced_keyframe_pending_ = true;
        ++v_loss_transitions_;
        if (target == VideoState::Fallback) ++fallback_transitions_;
    }

    // Emit at fps cadence — drive next_v_emit_at_ off the wall clock.
    if (next_v_emit_at_ == std::chrono::steady_clock::time_point{}) {
        next_v_emit_at_ = last_v_input_ + milliseconds(frame_period_ms);
    }
    while (now >= next_v_emit_at_) {
        if (!v_pts_seeded_) {
            // No live frame ever observed — nothing to freeze. Skip.
            next_v_emit_at_ += milliseconds(frame_period_ms);
            continue;
        }
        const std::int64_t pts = next_v_pts_90k_;
        const bool key = forced_keyframe_pending_;
        forced_keyframe_pending_ = false;

        auto sink = [this](VideoOutFrame&& f) { emitVideoPes(std::move(f)); };
        if (v_state_ == VideoState::Fallback) {
            video_tc_.emitFallback(pts, key, sink);
        } else {
            video_tc_.emitFreeze(pts, key, sink);
        }
        // emitVideoPes already advanced next_v_pts_90k_; just bump the wall
        // clock for the next due-tick.
        next_v_emit_at_ += milliseconds(frame_period_ms);
    }
}

void FfmpegTranscodePipeline::onAudioLossTickIfDue(
        std::chrono::steady_clock::time_point now) {
    if (!cfg_.silence_on_audio_loss) return;
    if (!audio_tc_.encoderOpen()) return;
    if (last_a_input_ == std::chrono::steady_clock::time_point{}) return;

    using namespace std::chrono;
    const auto idle_ms = duration_cast<milliseconds>(now - last_a_input_).count();
    const auto frame_ms = (cfg_.audio_sample_rate > 0)
                          ? (1024 * 1000 / cfg_.audio_sample_rate)
                          : 21;  // ~21 ms at 48 kHz
    if (idle_ms < frame_ms * 4) {
        if (a_state_ != AudioState::Live) a_state_ = AudioState::Live;
        return;
    }

    if (a_state_ != AudioState::Silence) {
        a_state_ = AudioState::Silence;
        ++a_loss_transitions_;
    }

    if (next_a_emit_at_ == std::chrono::steady_clock::time_point{}) {
        next_a_emit_at_ = last_a_input_ + milliseconds(frame_ms);
    }
    while (now >= next_a_emit_at_) {
        if (!a_pts_seeded_) {
            next_a_emit_at_ += milliseconds(frame_ms);
            continue;
        }
        const std::int64_t pts = next_a_pts_90k_;
        auto sink = [this](AudioOutFrame&& f) { emitAudioPes(std::move(f)); };
        audio_tc_.emitSilence(pts, sink);
        next_a_emit_at_ += milliseconds(frame_ms);
    }
}

void FfmpegTranscodePipeline::resetVideoStateOnRecovery(
        std::int64_t resumed_pts_90k) {
    v_state_ = VideoState::Live;
    next_v_pts_90k_ = resumed_pts_90k;
    next_v_emit_at_ = {};
    forced_keyframe_pending_ = false;
}

void FfmpegTranscodePipeline::resetAudioStateOnRecovery(
        std::int64_t resumed_pts_90k) {
    a_state_ = AudioState::Live;
    next_a_pts_90k_ = resumed_pts_90k;
    next_a_emit_at_ = {};
}

// ── Stats ───────────────────────────────────────────────────────────────────

nlohmann::json FfmpegTranscodePipeline::statsJson() const {
    auto videoStateStr = [](VideoState s) {
        switch (s) {
            case VideoState::Live:     return "live";
            case VideoState::Freeze:   return "freeze";
            case VideoState::Fallback: return "fallback";
        }
        return "unknown";
    };
    auto audioStateStr = [](AudioState s) {
        switch (s) {
            case AudioState::Live:    return "live";
            case AudioState::Silence: return "silence";
        }
        return "unknown";
    };

    return nlohmann::json{
        {"video", {
            {"state",                videoStateStr(v_state_)},
            {"input_pes",            v_input_pes_},
            {"frames_in",            video_tc_.framesIn()},
            {"frames_out",           video_tc_.framesOut()},
            {"freeze_frames_out",    video_tc_.freezeFramesOut()},
            {"fallback_frames_out",  video_tc_.fallbackFramesOut()},
            {"decode_errors",        video_tc_.decodeErrors()},
            {"encode_errors",        video_tc_.encodeErrors()},
            {"loss_transitions",     v_loss_transitions_},
            {"fallback_transitions", fallback_transitions_},
            {"fallback_loaded",      fallback_loaded_ok_},
            {"output_width",         video_tc_.outputWidth()},
            {"output_height",        video_tc_.outputHeight()},
        }},
        {"audio", {
            {"state",                audioStateStr(a_state_)},
            {"input_pes",            a_input_pes_},
            {"frames_in",            audio_tc_.framesIn()},
            {"frames_out",           audio_tc_.framesOut()},
            {"silence_frames_out",   audio_tc_.silenceFramesOut()},
            {"decode_errors",        audio_tc_.decodeErrors()},
            {"encode_errors",        audio_tc_.encodeErrors()},
            {"loss_transitions",     a_loss_transitions_},
            {"output_sample_rate",   audio_tc_.outputSampleRate()},
            {"output_channels",      audio_tc_.outputChannels()},
        }},
    };
}

}  // namespace liveqx::gateway::transcode
