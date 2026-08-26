#include "encoding/Mpeg2VideoEncoder.h"

#include <algorithm>
#include <atomic>
#include <utility>

#include <spdlog/spdlog.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

namespace liveqx::encoding {

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

struct Mpeg2VideoEncoder::Impl {
    Config                          cfg;
    std::shared_ptr<spdlog::logger> logger;

    CodecCtxPtr      ctx;
    FramePtr         frame;
    PacketPtr        enc_pkt;
    SwsPtr           sws;
    int64_t          pts        = 0;
    std::atomic<bool> force_idr{false};
    PacketCallback   packet_cb;

    spdlog::logger& lg() noexcept {
        return logger ? *logger : *spdlog::default_logger();
    }

    void drainPackets() {
        while (avcodec_receive_packet(ctx.get(), enc_pkt.get()) == 0) {
            if (packet_cb) packet_cb(enc_pkt.get());
            av_packet_unref(enc_pkt.get());
        }
    }
};

Mpeg2VideoEncoder::Mpeg2VideoEncoder(const Config& cfg,
                                     std::shared_ptr<spdlog::logger> logger)
    : impl_(std::make_unique<Impl>()) {
    impl_->cfg    = cfg;
    impl_->logger = std::move(logger);
}

Mpeg2VideoEncoder::~Mpeg2VideoEncoder() { close(); }

bool Mpeg2VideoEncoder::open() {
    const Config& cfg = impl_->cfg;

    const AVCodec* vcodec = avcodec_find_encoder(AV_CODEC_ID_MPEG2VIDEO);
    if (!vcodec) {
        impl_->lg().error("Mpeg2VideoEncoder: MPEG-2 encoder not found");
        return false;
    }

    impl_->ctx.reset(avcodec_alloc_context3(vcodec));
    if (!impl_->ctx) return false;

    AVCodecContext* vc = impl_->ctx.get();
    vc->width        = cfg.width;
    vc->height       = cfg.height;
    vc->pix_fmt      = AV_PIX_FMT_YUV420P;
    vc->bit_rate     = cfg.bitrate;
    vc->time_base    = { 1, cfg.fps };
    vc->framerate    = { cfg.fps, 1 };
    // Broadcast MPEG-2 GOP is traditionally 12–15 frames (~0.5 s @ 25 fps).
    // Caller-picked wins; auto default keeps the DVB set-top cadence.
    vc->gop_size     = cfg.gop_size > 0
                         ? cfg.gop_size
                         : (cfg.fps > 0 ? std::max(cfg.fps / 2, 6) : 12);
    // Honor the caller's B-frame count verbatim (matches X264/VAAPI/QSV/NVENC
    // encoders in this project). The DVB "IBBPBBP" cadence uses 2 B-frames —
    // that hint lives in the UI, not as a silent backend clamp.
    vc->max_b_frames = cfg.max_b_frames;
    // CBR envelope for broadcast set-tops: rc_max == rc_min == bit_rate,
    // with a VBV buffer of ~half a second (the DVB reference figure).
    vc->rc_max_rate    = cfg.bitrate;
    vc->rc_min_rate    = cfg.bitrate;
    vc->rc_buffer_size = static_cast<int>(cfg.bitrate / 2);
    // NOTE: AV_CODEC_FLAG_CLOSED_GOP is not implemented for mpeg2video in
    // FFmpeg 7.1 (avcodec_open2 returns AVERROR_PATCHWELCOME). Fixed GOP
    // cadence is achieved by holding gop_size constant and disabling
    // scene-change I-frame insertion below.
    av_opt_set(vc->priv_data, "sc_threshold", "1000000000", 0);
    if (cfg.global_header)
        vc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    // MPEG-2 doesn't take an x264-style preset dictionary. All rate control
    // knobs are on the AVCodecContext itself, so avcodec_open2 gets an empty
    // options bag.
    const int vopen = avcodec_open2(vc, vcodec, nullptr);
    if (vopen < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE] = {};
        av_strerror(vopen, errbuf, sizeof(errbuf));
        impl_->lg().error("Mpeg2VideoEncoder: avcodec_open2 failed: {}", errbuf);
        return false;
    }

    impl_->sws.reset(sws_getContext(
        cfg.width, cfg.height, AV_PIX_FMT_RGBA,
        cfg.width, cfg.height, AV_PIX_FMT_YUV420P,
        SWS_BILINEAR, nullptr, nullptr, nullptr));
    if (!impl_->sws) {
        impl_->lg().error("Mpeg2VideoEncoder: sws_getContext failed");
        return false;
    }

    impl_->frame.reset(av_frame_alloc());
    if (!impl_->frame) return false;
    impl_->frame->format = AV_PIX_FMT_YUV420P;
    impl_->frame->width  = cfg.width;
    impl_->frame->height = cfg.height;
    if (av_frame_get_buffer(impl_->frame.get(), 32) < 0) return false;

    impl_->enc_pkt.reset(av_packet_alloc());
    if (!impl_->enc_pkt) return false;

    impl_->pts = 0;
    return true;
}

void Mpeg2VideoEncoder::close() {
    impl_->enc_pkt.reset();
    impl_->frame.reset();
    impl_->ctx.reset();
    impl_->sws.reset();
    impl_->pts = 0;
}

void Mpeg2VideoEncoder::pushFrame(const Frame& bgra) {
    if (!impl_->ctx || !impl_->sws || !impl_->frame || !impl_->enc_pkt) return;
    if (!bgra.valid()) return;

    av_frame_make_writable(impl_->frame.get());

    const uint8_t* src_data[4]   = { bgra.pixels(), nullptr, nullptr, nullptr };
    const int      src_stride[4] = { bgra.width * 4, 0, 0, 0 };
    sws_scale(impl_->sws.get(),
              src_data, src_stride, 0, bgra.height,
              impl_->frame->data, impl_->frame->linesize);

    if (impl_->force_idr.exchange(false, std::memory_order_relaxed)) {
        impl_->frame->pict_type = AV_PICTURE_TYPE_I;
        impl_->frame->flags    |= AV_FRAME_FLAG_KEY;
    } else {
        impl_->frame->pict_type = AV_PICTURE_TYPE_NONE;
        impl_->frame->flags    &= ~AV_FRAME_FLAG_KEY;
    }
    impl_->frame->pts = impl_->pts++;

    if (avcodec_send_frame(impl_->ctx.get(), impl_->frame.get()) == 0) {
        impl_->drainPackets();
    }
}

void Mpeg2VideoEncoder::drain() {
    if (!impl_->ctx) return;
    avcodec_send_frame(impl_->ctx.get(), nullptr);
    impl_->drainPackets();
}

void Mpeg2VideoEncoder::forceKeyframe() noexcept {
    impl_->force_idr.store(true, std::memory_order_relaxed);
}

void Mpeg2VideoEncoder::setPacketCallback(PacketCallback cb) {
    impl_->packet_cb = std::move(cb);
}

bool Mpeg2VideoEncoder::fillStreamParameters(AVCodecParameters* dst) const {
    if (!impl_->ctx || !dst) return false;
    return avcodec_parameters_from_context(dst, impl_->ctx.get()) >= 0;
}

AVRational Mpeg2VideoEncoder::timeBase() const noexcept {
    return impl_->ctx ? impl_->ctx->time_base : AVRational{1, 1};
}

int Mpeg2VideoEncoder::effectiveMaxBFrames() const noexcept {
    return impl_->ctx ? impl_->ctx->max_b_frames : -1;
}

int Mpeg2VideoEncoder::effectiveGopSize() const noexcept {
    return impl_->ctx ? impl_->ctx->gop_size : -1;
}

}  // namespace liveqx::encoding
