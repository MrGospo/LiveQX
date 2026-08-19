#include "encoding/Encoder.h"
#include "encoding/EncoderFactory.h"
#include "encoding/IVideoEncoder.h"
#include "utils/Log.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/samplefmt.h>
#include <libswscale/swscale.h>
}

using liveqx::encoding::IVideoEncoder;

// ─── RAII helpers ─────────────────────────────────────────────────────────────

namespace {
struct CodecCtxDeleter { void operator()(AVCodecContext* p) const noexcept { if (p) avcodec_free_context(&p); } };
struct FrameDeleter    { void operator()(AVFrame*         p) const noexcept { if (p) av_frame_free(&p); } };
struct PacketDeleter   { void operator()(AVPacket*        p) const noexcept { if (p) av_packet_free(&p); } };
struct SwsDeleter      { void operator()(SwsContext*      p) const noexcept { if (p) sws_freeContext(p); } };

using CodecCtxPtr = std::unique_ptr<AVCodecContext, CodecCtxDeleter>;
using FramePtr    = std::unique_ptr<AVFrame,         FrameDeleter>;
using PacketPtr   = std::unique_ptr<AVPacket,        PacketDeleter>;
using SwsPtr      = std::unique_ptr<SwsContext,      SwsDeleter>;

}  // namespace

// ─── Impl ─────────────────────────────────────────────────────────────────────

struct Encoder::Impl {
    /* Video — delegated to backend */
    std::unique_ptr<IVideoEncoder> video;
    int                            video_sidx = -1;
    int64_t                        last_video_pts = 0;  // tagged onto AVIO Packet

    /* Audio (kept here — single AAC path for all backends) */
    CodecCtxPtr        audio_ctx;
    FramePtr           audio_frame;
    int64_t            audio_pts        = 0;
    int                audio_sidx       = -1;
    int                audio_frame_size = 1024;
    std::vector<float> audio_buf;
    size_t             audio_buf_pos    = 0;

    /* Mux */
    AVFormatContext* fmt_ctx  = nullptr;
    AVIOContext*     avio_ctx = nullptr;
    uint8_t*         avio_buf = nullptr;

    /* Reused for audio packet receive */
    PacketPtr enc_pkt;

    /* Output callback fed by AVIO write hook */
    Encoder::PacketCallback on_packet;

    /* Pre-encode taps (NDI raw-frame outputs and similar). Mutated only
       between pushFrame() invocations; the render thread reads without
       a lock. Updates from the API thread happen during build/stop where
       no pushFrame is in flight. */
    std::vector<Encoder::RawFrameCallback> raw_frame_cbs;

    /* Set true only after avformat_write_header succeeds. Gates trailer. */
    bool header_written = false;

    void reset() noexcept {
        if (fmt_ctx)  { avformat_free_context(fmt_ctx); fmt_ctx = nullptr; }
        if (avio_ctx) { avio_context_free(&avio_ctx);   avio_ctx = nullptr; }
        if (avio_buf) { av_free(avio_buf);              avio_buf = nullptr; }

        enc_pkt.reset();
        audio_frame.reset();
        audio_ctx.reset();
        if (video) { video->close(); video.reset(); }

        last_video_pts = 0;
        audio_pts = 0;
        video_sidx = audio_sidx = -1;
        audio_buf.clear();
        audio_buf_pos = 0;
        header_written = false;
    }
};

// ─── AVIO write callback ──────────────────────────────────────────────────────

int avioWriteCallback(void* opaque, const uint8_t* buf, int size) {
    auto* impl = static_cast<Encoder::Impl*>(opaque);
    if (impl->on_packet && size > 0) {
        Packet pkt;
        pkt.data.assign(buf, buf + size);
        pkt.pts      = impl->last_video_pts;
        pkt.is_video = true;
        impl->on_packet(pkt);
    }
    return size;
}

// ─── Encoder ─────────────────────────────────────────────────────────────────

Encoder::Encoder(const Config& cfg)
    : impl_(std::make_unique<Impl>()), cfg_(cfg) {}

Encoder::~Encoder() { close(); }

spdlog::logger& Encoder::lg() noexcept {
    return logger_ ? *logger_ : *spdlog::default_logger();
}

void Encoder::onPacket(PacketCallback cb) { impl_->on_packet = std::move(cb); }

void Encoder::addRawFrameCallback(RawFrameCallback cb) {
    if (cb) impl_->raw_frame_cbs.push_back(std::move(cb));
}

void Encoder::clearRawFrameCallbacks() {
    impl_->raw_frame_cbs.clear();
}

bool Encoder::open() {
    close(); // idempotent

    // Look up the muxer's flags WITHOUT allocating an AVFormatContext, so
    // we know whether AV_CODEC_FLAG_GLOBAL_HEADER is required before we
    // open the codec. Doing it the other way around (allocating fmt_ctx
    // first) means a partially-opened encoder leaves close() with a
    // non-null fmt_ctx and no avformat_write_header — which corrupts the
    // av_write_trailer path on the destructor.
    const AVOutputFormat* of = av_guess_format("mpegts", nullptr, nullptr);
    const bool needs_global_header = of && (of->flags & AVFMT_GLOBALHEADER);

    // ── Video backend (factory-dispatched; default x264) ─────────────────
    IVideoEncoder::Config vcfg;
    vcfg.width         = cfg_.width;
    vcfg.height        = cfg_.height;
    vcfg.fps           = cfg_.fps;
    vcfg.bitrate       = cfg_.video_bitrate;
    vcfg.preset        = cfg_.preset;
    vcfg.max_b_frames  = cfg_.max_b_frames;
    vcfg.global_header = needs_global_header;
    vcfg.gpu_index     = cfg_.gpu_index;

    impl_->video = liveqx::encoding::pickVideoEncoder(
        cfg_.encoder_mode, cfg_.video_codec, vcfg, logger_);
    if (!impl_->video) {
        lg().error("Encoder: pickVideoEncoder(mode=\"{}\", codec=\"{}\") failed",
                   cfg_.encoder_mode, cfg_.video_codec);
        return false;
    }

    // ── Audio encoder: AAC (best-effort) ─────────────────────────────────
    const AVCodec* acodec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (acodec) {
        impl_->audio_ctx.reset(avcodec_alloc_context3(acodec));
        if (impl_->audio_ctx) {
            AVCodecContext* ac  = impl_->audio_ctx.get();
            ac->sample_rate     = cfg_.sample_rate;
            av_channel_layout_default(&ac->ch_layout, 2);
            ac->sample_fmt      = AV_SAMPLE_FMT_FLTP;
            ac->bit_rate        = cfg_.audio_bitrate;
            ac->time_base       = { 1, cfg_.sample_rate };
            if (needs_global_header)
                ac->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

            if (avcodec_open2(ac, acodec, nullptr) < 0) {
                impl_->audio_ctx.reset();
            } else {
                impl_->audio_frame_size = ac->frame_size > 0 ? ac->frame_size : 1024;
            }
        }
    }

    if (impl_->audio_ctx) {
        impl_->audio_frame.reset(av_frame_alloc());
        if (impl_->audio_frame) {
            AVFrame* af    = impl_->audio_frame.get();
            af->format     = AV_SAMPLE_FMT_FLTP;
            av_channel_layout_default(&af->ch_layout, 2);
            af->sample_rate= cfg_.sample_rate;
            af->nb_samples = impl_->audio_frame_size;
            av_frame_get_buffer(af, 0);
        }
    }

    impl_->enc_pkt.reset(av_packet_alloc());
    if (!impl_->enc_pkt) return false;

    // ── Custom AVIO + MPEG-TS muxer ──────────────────────────────────────
    constexpr int kAvioBufSize = 64 * 1024;
    impl_->avio_buf = static_cast<uint8_t*>(av_malloc(kAvioBufSize));
    if (!impl_->avio_buf) return false;

    impl_->avio_ctx = avio_alloc_context(
        impl_->avio_buf, kAvioBufSize,
        /*write_flag=*/1,
        impl_.get(),
        /*read=*/nullptr,
        &avioWriteCallback,
        /*seek=*/nullptr);
    if (!impl_->avio_ctx) { av_free(impl_->avio_buf); impl_->avio_buf = nullptr; return false; }

    if (avformat_alloc_output_context2(&impl_->fmt_ctx, nullptr, "mpegts", nullptr) < 0
        || !impl_->fmt_ctx) {
        lg().error("Encoder: could not allocate MPEG-TS mux context");
        return false;
    }
    impl_->fmt_ctx->pb    = impl_->avio_ctx;
    impl_->fmt_ctx->flags |= AVFMT_FLAG_CUSTOM_IO;

    AVStream* vs = avformat_new_stream(impl_->fmt_ctx, nullptr);
    if (!vs) return false;
    vs->id        = 0;
    vs->time_base = impl_->video->timeBase();
    if (!impl_->video->fillStreamParameters(vs->codecpar)) return false;
    impl_->video_sidx = vs->index;

    if (impl_->audio_ctx) {
        AVStream* as = avformat_new_stream(impl_->fmt_ctx, nullptr);
        if (as) {
            as->id        = 1;
            as->time_base = impl_->audio_ctx->time_base;
            avcodec_parameters_from_context(as->codecpar, impl_->audio_ctx.get());
            impl_->audio_sidx = as->index;
        }
    }

    // Muxer options for the MPEG-TS output. Historically we passed nullptr
    // here, which meant the stream had no SDT (Otrum, LG Pro:Centric and
    // other hospitality/IPTV middleware reject or mis-identify such
    // streams) and default service_id/name/etc. Now:
    //   mpegts_flags = system_b | resend_headers
    //     system_b        — emit SDT (Service Description Table). Without
    //                       SDT strict middleware can't map the program to
    //                       a channel entry.
    //     resend_headers  — repeat PAT/PMT immediately after every keyframe
    //                       so a decoder that joined mid-GOP resyncs on the
    //                       next I-frame instead of waiting for the next
    //                       PAT interval.
    //   service_id / service_name / service_provider / TSID / ONID are all
    //   configurable per channel — critical when multiple channels share
    //   the same multicast subnet: identical service_id causes middleware
    //   to collide programs together.
    //   muxrate > 0 turns on FFmpeg's null-packet stuffing so the UDP
    //   stream is truly CBR. Leave at 0 for VBR (backwards compatible).
    //   {sdt,pat}_period accept a floating-point value in seconds.
    AVDictionary* muxopts = nullptr;
    av_dict_set(&muxopts, "mpegts_flags", "+system_b+resend_headers", 0);
    av_dict_set(&muxopts, "service_provider", cfg_.service_provider.c_str(), 0);
    av_dict_set(&muxopts, "service_name", cfg_.service_name.c_str(), 0);
    av_dict_set_int(&muxopts, "mpegts_service_id", cfg_.service_id, 0);
    av_dict_set_int(&muxopts, "mpegts_transport_stream_id", cfg_.transport_stream_id, 0);
    av_dict_set_int(&muxopts, "mpegts_original_network_id", cfg_.original_network_id, 0);
    if (cfg_.mux_rate > 0)
        av_dict_set_int(&muxopts, "muxrate", cfg_.mux_rate, 0);
    if (cfg_.sdt_period_ms > 0) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.3f", cfg_.sdt_period_ms / 1000.0);
        av_dict_set(&muxopts, "sdt_period", buf, 0);
    }
    if (cfg_.pat_period_ms > 0) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.3f", cfg_.pat_period_ms / 1000.0);
        av_dict_set(&muxopts, "pat_period", buf, 0);
    }

    const int wh_rc = avformat_write_header(impl_->fmt_ctx, &muxopts);
    av_dict_free(&muxopts);
    if (wh_rc < 0) {
        lg().error("Encoder: avformat_write_header failed");
        return false;
    }
    impl_->header_written = true;

    // Backend → muxer wiring (set after write_header so muxer time_base is
    // finalized and av_interleaved_write_frame is safe to call from the
    // backend's drain loop).
    Impl* impl = impl_.get();
    const AVRational vtb = impl_->video->timeBase();
    const AVRational mtb = impl_->fmt_ctx->streams[impl_->video_sidx]->time_base;
    impl_->video->setPacketCallback([impl, vtb, mtb](AVPacket* pkt) {
        pkt->stream_index = impl->video_sidx;
        av_packet_rescale_ts(pkt, vtb, mtb);
        impl->last_video_pts = pkt->pts;
        av_interleaved_write_frame(impl->fmt_ctx, pkt);
    });

    lg().info("Encoder: opened {}×{} @{}fps, video={}, preset={}",
              cfg_.width, cfg_.height, cfg_.fps,
              impl_->video->name(), cfg_.preset);
    return true;
}

void Encoder::close() {
    // Flush + trailer only when avformat_write_header succeeded.
    // Otherwise we still fall through to reset() to clean partial state.
    if (impl_->header_written && impl_->fmt_ctx) {
        if (impl_->video) impl_->video->drain();

        if (impl_->audio_ctx && impl_->audio_sidx >= 0) {
            avcodec_send_frame(impl_->audio_ctx.get(), nullptr);
            while (avcodec_receive_packet(impl_->audio_ctx.get(), impl_->enc_pkt.get()) == 0) {
                impl_->enc_pkt->stream_index = impl_->audio_sidx;
                av_packet_rescale_ts(impl_->enc_pkt.get(),
                    impl_->audio_ctx->time_base,
                    impl_->fmt_ctx->streams[impl_->audio_sidx]->time_base);
                av_interleaved_write_frame(impl_->fmt_ctx, impl_->enc_pkt.get());
                av_packet_unref(impl_->enc_pkt.get());
            }
        }

        av_write_trailer(impl_->fmt_ctx);
    }
    impl_->reset();
}

// ─── Reset on reconnect ───────────────────────────────────────────────────────

void Encoder::resetOnReconnect() noexcept {
    pts_reset_pending_.store(true, std::memory_order_release);
    force_idr_.store(true, std::memory_order_relaxed);
}

// ─── Push frame ───────────────────────────────────────────────────────────────

void Encoder::pushFrame(const Frame& video, const AudioFrame& audio) {
    if (!impl_->fmt_ctx || !impl_->video) return;

    // On reconnect: flush the audio accumulator so the new client starts on a
    // clean AAC frame boundary. PTS continues monotonically — the muxer
    // requires it.
    if (pts_reset_pending_.exchange(false, std::memory_order_acquire)) {
        impl_->audio_buf.clear();
        impl_->audio_buf_pos = 0;
        lg().info("Encoder: audio accumulator flushed for new client");
    }

    // Translate the facade's force_idr flag into a backend-level keyframe
    // request before we hand off the frame.
    if (force_idr_.exchange(false, std::memory_order_relaxed)) {
        impl_->video->forceKeyframe();
    }

    // Pre-encode tap (NDI raw-frame outputs etc). Fires before we hand the
    // Frame to the encoder so the buffer is still RGBA. Exceptions out of a
    // tap must not abort the render thread.
    if (!impl_->raw_frame_cbs.empty() && video.valid()) {
        for (auto& cb : impl_->raw_frame_cbs) {
            try { cb(video); }
            catch (const std::exception& e) {
                lg().warn("Encoder: raw-frame tap threw: {}", e.what());
            } catch (...) {
                lg().warn("Encoder: raw-frame tap threw non-std exception");
            }
        }
    }

    impl_->video->pushFrame(video);

    // ── Buffer + encode audio (unchanged from pre-refactor) ──────────────────
    if (impl_->audio_ctx && impl_->audio_sidx >= 0 && audio.valid
        && !audio.samples.empty()) {
        impl_->audio_buf.insert(impl_->audio_buf.end(),
                                audio.samples.begin(), audio.samples.end());

        const int chunk = impl_->audio_frame_size * 2;
        while (static_cast<int>(impl_->audio_buf.size() - impl_->audio_buf_pos) >= chunk) {
            av_frame_make_writable(impl_->audio_frame.get());

            float* ch0 = reinterpret_cast<float*>(impl_->audio_frame->data[0]);
            float* ch1 = reinterpret_cast<float*>(impl_->audio_frame->data[1]);
            for (int i = 0; i < impl_->audio_frame_size; ++i) {
                ch0[i] = impl_->audio_buf[impl_->audio_buf_pos + i * 2];
                ch1[i] = impl_->audio_buf[impl_->audio_buf_pos + i * 2 + 1];
            }
            impl_->audio_buf_pos += static_cast<size_t>(chunk);
            if (impl_->audio_buf_pos > 8192) {
                impl_->audio_buf.erase(impl_->audio_buf.begin(),
                                       impl_->audio_buf.begin() + impl_->audio_buf_pos);
                impl_->audio_buf_pos = 0;
            }

            impl_->audio_frame->pts = impl_->audio_pts;
            impl_->audio_pts += impl_->audio_frame_size;

            if (avcodec_send_frame(impl_->audio_ctx.get(),
                                   impl_->audio_frame.get()) == 0) {
                while (avcodec_receive_packet(impl_->audio_ctx.get(),
                                              impl_->enc_pkt.get()) == 0) {
                    AVPacket* pkt = impl_->enc_pkt.get();
                    pkt->stream_index = impl_->audio_sidx;
                    av_packet_rescale_ts(pkt,
                        impl_->audio_ctx->time_base,
                        impl_->fmt_ctx->streams[impl_->audio_sidx]->time_base);
                    av_interleaved_write_frame(impl_->fmt_ctx, pkt);
                    av_packet_unref(pkt);
                }
            }
        }
    }
}
