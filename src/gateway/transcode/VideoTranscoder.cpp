#include "gateway/transcode/VideoTranscoder.h"

#include <algorithm>
#include <cstring>
#include <utility>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

namespace liveqx::gateway::transcode {

namespace {

struct CodecCtxDeleter { void operator()(AVCodecContext* p) const noexcept { if (p) avcodec_free_context(&p); } };
struct FrameDeleter    { void operator()(AVFrame*         p) const noexcept { if (p) av_frame_free(&p); } };
struct PacketDeleter   { void operator()(AVPacket*        p) const noexcept { if (p) av_packet_free(&p); } };
struct SwsDeleter      { void operator()(SwsContext*      p) const noexcept { if (p) sws_freeContext(p); } };

using CodecCtxPtr = std::unique_ptr<AVCodecContext, CodecCtxDeleter>;
using FramePtr    = std::unique_ptr<AVFrame,         FrameDeleter>;
using PacketPtr   = std::unique_ptr<AVPacket,        PacketDeleter>;
using SwsPtr      = std::unique_ptr<SwsContext,      SwsDeleter>;

constexpr AVRational kTbTs90k = {1, 90000};

}  // namespace

struct VideoTranscoder::Impl {
    CodecCtxPtr dec_ctx;
    CodecCtxPtr enc_ctx;
    SwsPtr      sws;

    PacketPtr   in_pkt;          // ES → decoder
    FramePtr    dec_frame;       // decoder output (input pix_fmt)
    FramePtr    scale_frame;     // YUV420P @ encoder dims (after swscale)
    FramePtr    freeze_frame;    // ref-shared snapshot of last scale_frame for freeze
    FramePtr    fallback_frame;  // logo image scaled to encoder dims (YUV420P)
    PacketPtr   out_pkt;         // encoder output

    bool has_freeze    = false;
    bool has_fallback  = false;
    int input_pix_fmt  = -1;  // AV_PIX_FMT_*
    int input_width    = 0;
    int input_height   = 0;
    int output_width   = 0;
    int output_height  = 0;
};

VideoTranscoder::VideoTranscoder(TranscodeCfg cfg)
    : impl_(std::make_unique<Impl>()), cfg_(std::move(cfg)) {}

VideoTranscoder::~VideoTranscoder() = default;

int VideoTranscoder::outputWidth()  const noexcept {
    return impl_ ? impl_->output_width  : 0;
}
int VideoTranscoder::outputHeight() const noexcept {
    return impl_ ? impl_->output_height : 0;
}

bool VideoTranscoder::init(int input_avcodec_id) {
    if (decoder_open_) return true;

    const AVCodec* dec = avcodec_find_decoder(static_cast<AVCodecID>(input_avcodec_id));
    if (!dec) return false;

    impl_->dec_ctx.reset(avcodec_alloc_context3(dec));
    if (!impl_->dec_ctx) return false;

    impl_->dec_ctx->time_base = kTbTs90k;
    impl_->dec_ctx->pkt_timebase = kTbTs90k;

    if (avcodec_open2(impl_->dec_ctx.get(), dec, nullptr) < 0) {
        impl_->dec_ctx.reset();
        return false;
    }

    impl_->in_pkt.reset(av_packet_alloc());
    impl_->dec_frame.reset(av_frame_alloc());
    impl_->out_pkt.reset(av_packet_alloc());
    if (!impl_->in_pkt || !impl_->dec_frame || !impl_->out_pkt) {
        impl_->dec_ctx.reset();
        return false;
    }

    decoder_open_ = true;
    return true;
}

bool VideoTranscoder::openEncoderForFrameDims(int input_w, int input_h, int input_pix_fmt) {
    if (encoder_open_) return true;

    const AVCodec* enc = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!enc) return false;

    impl_->enc_ctx.reset(avcodec_alloc_context3(enc));
    if (!impl_->enc_ctx) return false;

    const int out_w = (cfg_.video_width  > 0) ? cfg_.video_width  : input_w;
    const int out_h = (cfg_.video_height > 0) ? cfg_.video_height : input_h;
    const int fps   = (cfg_.video_fps    > 0) ? cfg_.video_fps    : 25;

    AVCodecContext* ec = impl_->enc_ctx.get();
    ec->width        = out_w;
    ec->height       = out_h;
    ec->pix_fmt      = AV_PIX_FMT_YUV420P;
    ec->bit_rate     = static_cast<int64_t>(cfg_.video_bitrate_bps);
    ec->time_base    = kTbTs90k;        // 90 kHz pt-clock; encoder rescales internally
    ec->framerate    = AVRational{fps, 1};
    ec->gop_size     = std::max(1, fps);
    ec->max_b_frames = std::max(0, cfg_.video_max_b_frames);

    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "preset", cfg_.video_preset.empty() ? "medium" : cfg_.video_preset.c_str(), 0);
    if (cfg_.video_max_b_frames == 0)
        av_dict_set(&opts, "tune", "zerolatency", 0);

    const int rc = avcodec_open2(ec, enc, &opts);
    av_dict_free(&opts);
    if (rc < 0) {
        impl_->enc_ctx.reset();
        return false;
    }

    // Allocate the scaling target frame (encoder dims, YUV420P).
    impl_->scale_frame.reset(av_frame_alloc());
    if (!impl_->scale_frame) {
        impl_->enc_ctx.reset();
        return false;
    }
    impl_->scale_frame->format = AV_PIX_FMT_YUV420P;
    impl_->scale_frame->width  = out_w;
    impl_->scale_frame->height = out_h;
    if (av_frame_get_buffer(impl_->scale_frame.get(), 32) < 0) {
        impl_->scale_frame.reset();
        impl_->enc_ctx.reset();
        return false;
    }

    // Empty container; populated lazily on the first successful decode.
    impl_->freeze_frame.reset(av_frame_alloc());
    if (!impl_->freeze_frame) {
        impl_->scale_frame.reset();
        impl_->enc_ctx.reset();
        return false;
    }

    // Need swscale only when dims or pix_fmt differ. For pure passthrough
    // dims+pix_fmt we still go through scale_frame so the encoder always
    // sees an aligned, encoder-owned buffer (it's cheaper than negotiating
    // ref-counted handoff between two AVCodecContexts).
    impl_->sws.reset(sws_getContext(
        input_w, input_h, static_cast<AVPixelFormat>(input_pix_fmt),
        out_w,   out_h,   AV_PIX_FMT_YUV420P,
        SWS_BILINEAR, nullptr, nullptr, nullptr));
    if (!impl_->sws) {
        impl_->scale_frame.reset();
        impl_->enc_ctx.reset();
        return false;
    }

    impl_->input_pix_fmt = input_pix_fmt;
    impl_->input_width   = input_w;
    impl_->input_height  = input_h;
    impl_->output_width  = out_w;
    impl_->output_height = out_h;
    encoder_open_ = true;
    return true;
}

void VideoTranscoder::feed(std::span<const std::uint8_t> es,
                           std::optional<std::int64_t>   pts_90khz,
                           std::optional<std::int64_t>   dts_90khz,
                           const OutSink&                sink) {
    if (!decoder_open_) return;
    ++frames_in_;

    AVPacket* pkt = impl_->in_pkt.get();
    av_packet_unref(pkt);
    pkt->data = const_cast<std::uint8_t*>(es.data());
    pkt->size = static_cast<int>(es.size());
    pkt->pts  = pts_90khz.value_or(AV_NOPTS_VALUE);
    pkt->dts  = dts_90khz.value_or(AV_NOPTS_VALUE);
    pkt->time_base = kTbTs90k;

    const int rc = avcodec_send_packet(impl_->dec_ctx.get(), pkt);
    pkt->data = nullptr;   // we did not own the buffer; clear before free
    pkt->size = 0;
    if (rc < 0 && rc != AVERROR(EAGAIN)) {
        ++decode_errors_;
        return;
    }
    drainDecoder(sink);
}

void VideoTranscoder::drainDecoder(const OutSink& sink) {
    while (true) {
        const int rc = avcodec_receive_frame(impl_->dec_ctx.get(), impl_->dec_frame.get());
        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) return;
        if (rc < 0) {
            ++decode_errors_;
            return;
        }
        AVFrame* f = impl_->dec_frame.get();

        if (!encoder_open_) {
            if (!openEncoderForFrameDims(f->width, f->height, f->format)) {
                ++encode_errors_;
                av_frame_unref(f);
                return;
            }
        }

        // Scale into the encoder-target frame.
        if (av_frame_make_writable(impl_->scale_frame.get()) < 0) {
            ++scale_errors_;
            av_frame_unref(f);
            continue;
        }
        const int sh = sws_scale(impl_->sws.get(),
                                 f->data, f->linesize, 0, f->height,
                                 impl_->scale_frame->data, impl_->scale_frame->linesize);
        if (sh <= 0) {
            ++scale_errors_;
            av_frame_unref(f);
            continue;
        }
        impl_->scale_frame->pts = f->pts;

        // Snapshot the latest scaled picture for freeze-frame use. av_frame_ref
        // shares the backing planes with scale_frame; the next call to
        // av_frame_make_writable(scale_frame) above will copy-on-write so the
        // freeze cache stays intact while sws_scale rewrites scale_frame.
        av_frame_unref(impl_->freeze_frame.get());
        if (av_frame_ref(impl_->freeze_frame.get(), impl_->scale_frame.get()) >= 0) {
            impl_->has_freeze = true;
        }

        const int er = avcodec_send_frame(impl_->enc_ctx.get(), impl_->scale_frame.get());
        av_frame_unref(f);
        if (er < 0 && er != AVERROR(EAGAIN)) {
            ++encode_errors_;
            continue;
        }
        drainEncoder(sink);
    }
}

void VideoTranscoder::drainEncoder(const OutSink& sink) {
    while (true) {
        const int rc = avcodec_receive_packet(impl_->enc_ctx.get(), impl_->out_pkt.get());
        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) return;
        if (rc < 0) {
            ++encode_errors_;
            return;
        }
        AVPacket* p = impl_->out_pkt.get();

        VideoOutFrame out;
        out.es.assign(p->data, p->data + p->size);
        out.pts_90khz   = (p->pts != AV_NOPTS_VALUE) ? p->pts : 0;
        out.dts_90khz   = (p->dts != AV_NOPTS_VALUE) ? p->dts : out.pts_90khz;
        out.is_keyframe = (p->flags & AV_PKT_FLAG_KEY) != 0;
        ++frames_out_;
        if (sink) sink(std::move(out));
        av_packet_unref(p);
    }
}

bool VideoTranscoder::hasLastFrame() const noexcept {
    return impl_ && impl_->has_freeze;
}

bool VideoTranscoder::hasFallback() const noexcept {
    return impl_ && impl_->has_fallback;
}

namespace {

struct FormatCtxDeleter { void operator()(AVFormatContext* p) const noexcept { if (p) avformat_close_input(&p); } };
using FormatCtxPtr = std::unique_ptr<AVFormatContext, FormatCtxDeleter>;

}  // namespace

bool VideoTranscoder::loadFallbackImage(const std::string& path) {
    if (!encoder_open_) return false;
    if (path.empty()) return false;

    AVFormatContext* fmt_raw = nullptr;
    if (avformat_open_input(&fmt_raw, path.c_str(), nullptr, nullptr) < 0) return false;
    FormatCtxPtr fmt(fmt_raw);

    if (avformat_find_stream_info(fmt.get(), nullptr) < 0) return false;

    int video_stream = -1;
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream = static_cast<int>(i);
            break;
        }
    }
    if (video_stream < 0) return false;

    AVStream* st = fmt->streams[video_stream];
    const AVCodec* dec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!dec) return false;

    CodecCtxPtr dec_ctx(avcodec_alloc_context3(dec));
    if (!dec_ctx) return false;
    if (avcodec_parameters_to_context(dec_ctx.get(), st->codecpar) < 0) return false;
    if (avcodec_open2(dec_ctx.get(), dec, nullptr) < 0) return false;

    PacketPtr pkt(av_packet_alloc());
    FramePtr  img_frame(av_frame_alloc());
    if (!pkt || !img_frame) return false;

    bool got_frame = false;
    while (!got_frame && av_read_frame(fmt.get(), pkt.get()) >= 0) {
        if (pkt->stream_index != video_stream) {
            av_packet_unref(pkt.get());
            continue;
        }
        if (avcodec_send_packet(dec_ctx.get(), pkt.get()) >= 0) {
            const int rc = avcodec_receive_frame(dec_ctx.get(), img_frame.get());
            if (rc == 0) got_frame = true;
        }
        av_packet_unref(pkt.get());
    }
    if (!got_frame) {
        // Try draining with NULL packet for single-image containers that
        // need it (e.g. some PNGs).
        avcodec_send_packet(dec_ctx.get(), nullptr);
        if (avcodec_receive_frame(dec_ctx.get(), img_frame.get()) == 0) {
            got_frame = true;
        }
    }
    if (!got_frame) return false;

    // Allocate the cache frame (encoder dims, YUV420P).
    FramePtr cache(av_frame_alloc());
    if (!cache) return false;
    cache->format = AV_PIX_FMT_YUV420P;
    cache->width  = impl_->output_width;
    cache->height = impl_->output_height;
    if (av_frame_get_buffer(cache.get(), 32) < 0) return false;

    // Local one-shot sws — keep impl_->sws untouched so it can stay configured
    // for the ongoing input video stream.
    SwsPtr local_sws(sws_getContext(
        img_frame->width, img_frame->height, static_cast<AVPixelFormat>(img_frame->format),
        impl_->output_width, impl_->output_height, AV_PIX_FMT_YUV420P,
        SWS_BILINEAR, nullptr, nullptr, nullptr));
    if (!local_sws) return false;

    if (sws_scale(local_sws.get(),
                  img_frame->data, img_frame->linesize, 0, img_frame->height,
                  cache->data, cache->linesize) <= 0) {
        return false;
    }

    impl_->fallback_frame = std::move(cache);
    impl_->has_fallback = true;
    return true;
}

void VideoTranscoder::emitFallback(std::int64_t pts_90khz,
                                   bool         force_keyframe,
                                   const OutSink& sink) {
    if (!encoder_open_ || !impl_->has_fallback) return;

    AVFrame* fr = impl_->fallback_frame.get();
    fr->pts = pts_90khz;
    fr->pict_type = force_keyframe ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;

    const int er = avcodec_send_frame(impl_->enc_ctx.get(), fr);
    fr->pict_type = AV_PICTURE_TYPE_NONE;
    if (er < 0 && er != AVERROR(EAGAIN)) {
        ++encode_errors_;
        return;
    }
    ++fallback_frames_out_;
    drainEncoder(sink);
}

void VideoTranscoder::emitFreeze(std::int64_t pts_90khz,
                                 bool         force_keyframe,
                                 const OutSink& sink) {
    if (!encoder_open_ || !impl_->has_freeze) return;

    AVFrame* fr = impl_->freeze_frame.get();
    fr->pts = pts_90khz;
    // pict_type=AV_PICTURE_TYPE_I requests a keyframe; libx264 honours it.
    // (AVFrame::key_frame is the legacy field — deprecated since FFmpeg 6.)
    fr->pict_type = force_keyframe ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;

    const int er = avcodec_send_frame(impl_->enc_ctx.get(), fr);
    fr->pict_type = AV_PICTURE_TYPE_NONE;
    if (er < 0 && er != AVERROR(EAGAIN)) {
        ++encode_errors_;
        return;
    }
    ++freeze_frames_out_;

    // We can't tell freeze frames apart from regular ones at the encoder
    // output side (they're just additional access units). Reuse the regular
    // drain — the caller's stats counters (frames_out_) will tick for them.
    drainEncoder(sink);
}

void VideoTranscoder::flush(const OutSink& sink) {
    if (decoder_open_) {
        avcodec_send_packet(impl_->dec_ctx.get(), nullptr);
        drainDecoder(sink);
    }
    if (encoder_open_) {
        avcodec_send_frame(impl_->enc_ctx.get(), nullptr);
        drainEncoder(sink);
    }
}

}  // namespace liveqx::gateway::transcode
