#include "preview/PreviewEncoder.h"

#include <cstring>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include "utils/Log.h"

namespace liveqx::preview {

namespace {

struct CodecCtxDeleter { void operator()(AVCodecContext* p) const noexcept { if (p) avcodec_free_context(&p); } };
struct FrameDeleter    { void operator()(AVFrame*         p) const noexcept { if (p) av_frame_free(&p); } };
struct PacketDeleter   { void operator()(AVPacket*        p) const noexcept { if (p) av_packet_free(&p); } };
struct SwsDeleter      { void operator()(SwsContext*      p) const noexcept { if (p) sws_freeContext(p); } };

using CodecCtxPtr = std::unique_ptr<AVCodecContext, CodecCtxDeleter>;
using FramePtr    = std::unique_ptr<AVFrame,        FrameDeleter>;
using PacketPtr   = std::unique_ptr<AVPacket,       PacketDeleter>;
using SwsPtr      = std::unique_ptr<SwsContext,     SwsDeleter>;

// Detect Annex-B keyframe by walking start codes and looking for IDR
// NAL (type=5) or SPS/PPS (type=7/8) — the latter pair is always
// produced alongside IDR by libx264 on a keyframe boundary.
bool packetIsKeyframe(const std::uint8_t* data, std::size_t size) noexcept {
    std::size_t i = 0;
    while (i + 4 <= size) {
        // start code: 00 00 00 01 OR 00 00 01
        bool sc4 = data[i] == 0 && data[i+1] == 0 && data[i+2] == 0 && data[i+3] == 1;
        bool sc3 = data[i] == 0 && data[i+1] == 0 && data[i+2] == 1;
        if (sc4) {
            i += 4;
        } else if (sc3) {
            i += 3;
        } else {
            ++i;
            continue;
        }
        if (i >= size) break;
        const std::uint8_t nal_type = data[i] & 0x1F;
        if (nal_type == 5 /*IDR*/ || nal_type == 7 /*SPS*/ || nal_type == 8 /*PPS*/)
            return true;
        ++i;
    }
    return false;
}

}  // namespace

struct PreviewEncoder::Impl {
    CodecCtxPtr ctx;
    FramePtr    frame_yuv;
    PacketPtr   pkt;
    SwsPtr      sws;
    int         scaler_src_w = 0;
    int         scaler_src_h = 0;
    std::int64_t pts_counter = 0;
};

PreviewEncoder::PreviewEncoder(Config cfg)
    : impl_(std::make_unique<Impl>()), cfg_(cfg) {}

PreviewEncoder::~PreviewEncoder() {
    if (running_) stop();
}

bool PreviewEncoder::start(NalCallback cb) {
    if (running_) return false;
    cb_ = std::move(cb);

    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!codec) {
        LOG_ERROR("PreviewEncoder: H.264 encoder not found in this FFmpeg build");
        return false;
    }

    impl_->ctx.reset(avcodec_alloc_context3(codec));
    auto* c = impl_->ctx.get();
    if (!c) return false;

    c->width      = cfg_.width;
    c->height     = cfg_.height;
    c->time_base  = AVRational{1, cfg_.fps};
    c->framerate  = AVRational{cfg_.fps, 1};
    c->pix_fmt    = AV_PIX_FMT_YUV420P;
    c->gop_size   = cfg_.gop;
    c->max_b_frames = 0;
    c->bit_rate   = cfg_.bitrate_bps;
    c->rc_min_rate = cfg_.bitrate_bps;
    c->rc_max_rate = cfg_.bitrate_bps;
    c->rc_buffer_size = cfg_.bitrate_bps;  // 1s window

    // libx264 tuning — ultrafast/zerolatency keeps CPU minimal and avoids
    // lookahead delay; baseline profile maximises browser compatibility.
    av_opt_set(c->priv_data, "preset",  "ultrafast",   0);
    av_opt_set(c->priv_data, "tune",    "zerolatency", 0);
    av_opt_set(c->priv_data, "profile", "baseline",    0);
    // x264 emits SPS/PPS once per IDR — the WebRTC packetizer expects
    // them inline so the receiver can re-key without out-of-band SDP.
    av_opt_set(c->priv_data, "x264-params",
               "repeat-headers=1:annexb=1", 0);

    if (avcodec_open2(c, codec, nullptr) < 0) {
        LOG_ERROR("PreviewEncoder: avcodec_open2 failed");
        impl_->ctx.reset();
        return false;
    }

    impl_->frame_yuv.reset(av_frame_alloc());
    impl_->frame_yuv->format = AV_PIX_FMT_YUV420P;
    impl_->frame_yuv->width  = cfg_.width;
    impl_->frame_yuv->height = cfg_.height;
    if (av_frame_get_buffer(impl_->frame_yuv.get(), 32) < 0) {
        LOG_ERROR("PreviewEncoder: av_frame_get_buffer failed");
        return false;
    }
    impl_->pkt.reset(av_packet_alloc());

    running_ = true;
    return true;
}

void PreviewEncoder::stop() {
    if (!running_) return;
    running_ = false;
    if (auto* c = impl_->ctx.get()) {
        // Flush — send NULL frame, drain remaining packets.
        avcodec_send_frame(c, nullptr);
        for (;;) {
            int r = avcodec_receive_packet(c, impl_->pkt.get());
            if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) break;
            if (r < 0) break;
            if (cb_ && impl_->pkt->size > 0) {
                cb_(impl_->pkt->data, impl_->pkt->size,
                    packetIsKeyframe(impl_->pkt->data, impl_->pkt->size),
                    impl_->pkt->pts);
                bytes_emitted_ += impl_->pkt->size;
            }
            av_packet_unref(impl_->pkt.get());
        }
    }
    impl_->sws.reset();
    impl_->frame_yuv.reset();
    impl_->pkt.reset();
    impl_->ctx.reset();
}

bool PreviewEncoder::encode(const Frame& src) {
    if (!running_ || !src.valid()) return false;
    auto* c = impl_->ctx.get();
    if (!c) return false;

    // (Re)initialise the scaler when the source resolution changes
    // (e.g. channel was reconfigured between frames).
    if (!impl_->sws ||
        impl_->scaler_src_w != src.width ||
        impl_->scaler_src_h != src.height) {
        impl_->sws.reset(sws_getContext(
            src.width, src.height, AV_PIX_FMT_RGBA,
            cfg_.width, cfg_.height, AV_PIX_FMT_YUV420P,
            SWS_BILINEAR, nullptr, nullptr, nullptr));
        if (!impl_->sws) {
            LOG_ERROR("PreviewEncoder: sws_getContext failed");
            return false;
        }
        impl_->scaler_src_w = src.width;
        impl_->scaler_src_h = src.height;
    }

    if (av_frame_make_writable(impl_->frame_yuv.get()) < 0) return false;

    const std::uint8_t* src_planes[1]   = { src.pixels() };
    int                 src_stride[1]   = { src.width * 4 };
    sws_scale(impl_->sws.get(), src_planes, src_stride, 0, src.height,
              impl_->frame_yuv->data, impl_->frame_yuv->linesize);

    impl_->frame_yuv->pts = impl_->pts_counter++;

    // forceKeyframe() запрашивает немедленный IDR — нужно для двух кейсов:
    //   (а) первый кадр после Connected, чтобы браузер сразу мог декодировать
    //       вместо ожидания планового IDR через gop=60 (~2.4s @ 25fps);
    //   (б) RTCP PLI/FIR от получателя при потере пакетов.
    // exchange сбрасывает флаг под себя — повторный encode() идёт обычным
    // P-frame'ом. AV_PICTURE_TYPE_NONE отдаём в default-кейсе явно, иначе
    // libx264 может «застрять» в I-режиме на тот же AVFrame между вызовами.
    if (force_idr_.exchange(false, std::memory_order_acq_rel)) {
        impl_->frame_yuv->pict_type = AV_PICTURE_TYPE_I;
        impl_->frame_yuv->flags |= AV_FRAME_FLAG_KEY;
    } else {
        impl_->frame_yuv->pict_type = AV_PICTURE_TYPE_NONE;
        impl_->frame_yuv->flags &= ~AV_FRAME_FLAG_KEY;
    }

    if (avcodec_send_frame(c, impl_->frame_yuv.get()) < 0) return false;

    for (;;) {
        int r = avcodec_receive_packet(c, impl_->pkt.get());
        if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) break;
        if (r < 0) return false;
        if (cb_ && impl_->pkt->size > 0) {
            cb_(impl_->pkt->data,
                static_cast<std::size_t>(impl_->pkt->size),
                packetIsKeyframe(impl_->pkt->data, impl_->pkt->size),
                src.pts);
            bytes_emitted_ += impl_->pkt->size;
        }
        av_packet_unref(impl_->pkt.get());
    }
    ++frames_encoded_;
    return true;
}

}  // namespace liveqx::preview
