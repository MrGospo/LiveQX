#include "clips/MediaValidator.h"
#include "utils/Log.h"

#include <cmath>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}

ValidationResult MediaValidator::validate(const std::string& path) {
    ValidationResult r;

    // ── Open container ────────────────────────────────────────────────────────
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) != 0) {
        r.error = "cannot open file";
        return r;
    }

    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        avformat_close_input(&fmt);
        r.error = "cannot read stream info";
        return r;
    }

    // ── Scan streams ──────────────────────────────────────────────────────────
    int video_idx = -1;
    int audio_idx = -1;

    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        const auto t = fmt->streams[i]->codecpar->codec_type;
        if (video_idx < 0 && t == AVMEDIA_TYPE_VIDEO)
            video_idx = static_cast<int>(i);
        if (audio_idx < 0 && t == AVMEDIA_TYPE_AUDIO)
            audio_idx = static_cast<int>(i);
    }

    // ── Video stream required ─────────────────────────────────────────────────
    if (video_idx < 0) {
        avformat_close_input(&fmt);
        r.error = "no video stream";
        return r;
    }
    r.has_video = true;

    // ── Check video codec is supported ────────────────────────────────────────
    {
        const AVCodecParameters* par = fmt->streams[video_idx]->codecpar;
        const AVCodec* codec = avcodec_find_decoder(par->codec_id);
        if (!codec) {
            avformat_close_input(&fmt);
            r.error = "unsupported video codec: "
                      + std::string(avcodec_get_name(par->codec_id));
            return r;
        }
    }

    // ── Video duration ────────────────────────────────────────────────────────
    {
        const AVStream* st = fmt->streams[video_idx];
        if (st->duration != AV_NOPTS_VALUE && st->time_base.den > 0)
            r.video_dur_sec = static_cast<double>(st->duration)
                              * st->time_base.num / st->time_base.den;
        else if (fmt->duration > 0)
            r.video_dur_sec = static_cast<double>(fmt->duration) / AV_TIME_BASE;
    }

    // ── Audio stream (optional) ───────────────────────────────────────────────
    if (audio_idx >= 0) {
        const AVCodecParameters* par = fmt->streams[audio_idx]->codecpar;
        const AVCodec* codec = avcodec_find_decoder(par->codec_id);
        if (codec) {
            r.has_audio = true;

            const AVStream* st = fmt->streams[audio_idx];
            if (st->duration != AV_NOPTS_VALUE && st->time_base.den > 0)
                r.audio_dur_sec = static_cast<double>(st->duration)
                                  * st->time_base.num / st->time_base.den;
            else if (fmt->duration > 0)
                r.audio_dur_sec = static_cast<double>(fmt->duration) / AV_TIME_BASE;
        } else {
            LOG_INFO("MediaValidator: '{}' — audio codec {} not supported, audio disabled",
                     path, avcodec_get_name(par->codec_id));
        }
    } else {
        LOG_INFO("MediaValidator: '{}' — no audio stream", path);
    }

    // ── Duration mismatch check ───────────────────────────────────────────────
    if (r.has_audio && r.video_dur_sec > 0.0 && r.audio_dur_sec > 0.0) {
        r.duration_mismatch_sec = std::abs(r.video_dur_sec - r.audio_dur_sec);
        if (r.duration_mismatch_sec > 0.5)
            LOG_WARN("MediaValidator: '{}' — audio/video duration mismatch "
                     "(video={:.2f}s audio={:.2f}s diff={:.2f}s)",
                     path, r.video_dur_sec, r.audio_dur_sec, r.duration_mismatch_sec);
    }

    // ── First-frame IDR probe ─────────────────────────────────────────────────
    // Seamless loop swaps fmt_ctx and resumes decoding from the new context's
    // position 0. The decoder is flushed on CROSSFADE_POINT, so the first
    // emitted video frame must be a keyframe — otherwise P/B frames decode
    // against zero refs and produce visible artifacts. This is a soft warning:
    // we still load the clip (loops fall back to FLUSH path), but operators
    // get told the asset is loop-unfriendly.
    {
        AVPacket* pkt = av_packet_alloc();
        if (pkt) {
            constexpr int kMaxScan = 32; // headers + a few packets is plenty
            for (int i = 0; i < kMaxScan; ++i) {
                const int rc = av_read_frame(fmt, pkt);
                if (rc < 0) break;
                if (pkt->stream_index == video_idx) {
                    r.first_frame_is_idr = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
                    av_packet_unref(pkt);
                    break;
                }
                av_packet_unref(pkt);
            }
            av_packet_free(&pkt);
        }
        if (!r.first_frame_is_idr)
            LOG_WARN("MediaValidator: '{}' — first video frame is not a keyframe; "
                     "seamless loop will artifact, fallback path will be used",
                     path);
    }

    avformat_close_input(&fmt);
    r.valid = true;
    return r;
}
