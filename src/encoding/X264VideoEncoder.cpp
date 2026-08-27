#include "encoding/X264VideoEncoder.h"

#include <atomic>
#include <cstdio>
#include <utility>

#include <spdlog/spdlog.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libswscale/swscale.h>
}

namespace liveqx::encoding {

namespace {

// FFmpeg RAII deleters scoped to this translation unit. Other backends
// (NvencVideoEncoder etc., fix29 c6+) will mirror these inside their own
// .cpp; keeping them per-file avoids leaking ffmpeg headers via a shared
// include.
struct CodecCtxDeleter { void operator()(AVCodecContext* p) const noexcept { if (p) avcodec_free_context(&p); } };
struct FrameDeleter    { void operator()(AVFrame*         p) const noexcept { if (p) av_frame_free(&p); } };
struct PacketDeleter   { void operator()(AVPacket*        p) const noexcept { if (p) av_packet_free(&p); } };
struct SwsDeleter      { void operator()(SwsContext*      p) const noexcept { if (p) sws_freeContext(p); } };

using CodecCtxPtr = std::unique_ptr<AVCodecContext, CodecCtxDeleter>;
using FramePtr    = std::unique_ptr<AVFrame,         FrameDeleter>;
using PacketPtr   = std::unique_ptr<AVPacket,        PacketDeleter>;
using SwsPtr      = std::unique_ptr<SwsContext,      SwsDeleter>;

}  // namespace

struct X264VideoEncoder::Impl {
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

X264VideoEncoder::X264VideoEncoder(const Config& cfg,
                                   std::shared_ptr<spdlog::logger> logger)
    : impl_(std::make_unique<Impl>()) {
    impl_->cfg    = cfg;
    impl_->logger = std::move(logger);
}

X264VideoEncoder::~X264VideoEncoder() { close(); }

bool X264VideoEncoder::open() {
    const Config& cfg = impl_->cfg;

    const AVCodec* vcodec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!vcodec) {
        impl_->lg().error("X264VideoEncoder: H.264 encoder not found");
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
    // GOP length: caller-picked wins, else 1 s @ nominal fps.
    vc->gop_size     = cfg.gop_size > 0 ? cfg.gop_size : cfg.fps;
    vc->max_b_frames = cfg.max_b_frames;
    // Broadcast-grade CBR. rc_max_rate == rc_min_rate == rc_buffer_size ==
    // bit_rate is the textbook single-second CBR VBV: the decoder never
    // sees a bitrate spike above the declared value, and IPTV middleware
    // like Otrum / hospitality-TV decoders (LG Pro:Centric etc.) can rely
    // on the vbv envelope. Without this a scene cut easily peaks 2–3× the
    // nominal bitrate, blowing the decoder buffer and causing macroblocks
    // to "melt". VLC on a PC is tolerant thanks to its own client buffer.
    vc->rc_max_rate    = cfg.bitrate;
    vc->rc_min_rate    = cfg.bitrate;
    vc->rc_buffer_size = static_cast<int>(cfg.bitrate);
    // Closed-GOP so every GOP is independently decodable — required for
    // clean channel-tune-in on set-tops that latch on the next IDR.
    vc->flags |= AV_CODEC_FLAG_CLOSED_GOP;
    if (cfg.global_header)
        vc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    AVDictionary* vopts = nullptr;
    av_dict_set(&vopts, "preset", cfg.preset.c_str(), 0);
    // profile/level — leave unset for "auto" (libx264 picks the tightest
    // profile the params allow). When forced, decoder interop matters more
    // than compression; e.g. hospitality set-tops that lock to Main@3.1.
    if (!cfg.h264_profile.empty())
        av_dict_set(&vopts, "profile", cfg.h264_profile.c_str(), 0);
    if (cfg.h264_level > 0) {
        char lvl[16];
        if (cfg.h264_level % 10 == 0)
            std::snprintf(lvl, sizeof(lvl), "%d", cfg.h264_level / 10);
        else
            std::snprintf(lvl, sizeof(lvl), "%d.%d",
                          cfg.h264_level / 10, cfg.h264_level % 10);
        av_dict_set(&vopts, "level", lvl, 0);
    }
    // tune=zerolatency forces B-frames to 0 inside libx264 regardless of
    // what we set on the AVCodecContext. Only apply it when the caller
    // really wants the lowest-latency live profile (max_b == 0).
    if (cfg.max_b_frames == 0)
        av_dict_set(&vopts, "tune", "zerolatency", 0);
    // x264-params is the only way to reach these libx264 internals — none
    // of them are surfaced as AVCodecContext fields.
    //   nal-hrd=cbr     — signal CBR in the HRD parameters so downstream
    //                     decoders trust the vbv envelope. Mandatory for
    //                     spec-compliant CBR.
    //   force-cfr=1     — refuse to drop or duplicate frames; keeps output
    //                     truly constant-framerate for PCR stability.
    //   aud=1           — emit Access Unit Delimiter NALs. Many
    //                     hospitality-TV decoders and IPTV middleware
    //                     require AUDs to find frame boundaries reliably.
    //   sc_threshold=0  — disable scene-cut IDR insertion. Scene cuts
    //                     would sit outside the fixed GOP grid and blow
    //                     past vbv-maxrate on random frames, defeating
    //                     the CBR envelope above.
    av_dict_set(&vopts,
                "x264-params",
                "nal-hrd=cbr:force-cfr=1:aud=1:sc_threshold=0",
                0);
    const int vopen = avcodec_open2(vc, vcodec, &vopts);
    av_dict_free(&vopts);
    if (vopen < 0) {
        impl_->lg().error("X264VideoEncoder: avcodec_open2 failed");
        return false;
    }

    impl_->sws.reset(sws_getContext(
        cfg.width, cfg.height, AV_PIX_FMT_RGBA,
        cfg.width, cfg.height, AV_PIX_FMT_YUV420P,
        SWS_BILINEAR, nullptr, nullptr, nullptr));
    if (!impl_->sws) {
        impl_->lg().error("X264VideoEncoder: sws_getContext failed");
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

void X264VideoEncoder::close() {
    impl_->enc_pkt.reset();
    impl_->frame.reset();
    impl_->ctx.reset();
    impl_->sws.reset();
    impl_->pts = 0;
}

void X264VideoEncoder::pushFrame(const Frame& bgra) {
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

void X264VideoEncoder::drain() {
    if (!impl_->ctx) return;
    avcodec_send_frame(impl_->ctx.get(), nullptr);
    impl_->drainPackets();
}

void X264VideoEncoder::forceKeyframe() noexcept {
    impl_->force_idr.store(true, std::memory_order_relaxed);
}

void X264VideoEncoder::setPacketCallback(PacketCallback cb) {
    impl_->packet_cb = std::move(cb);
}

bool X264VideoEncoder::fillStreamParameters(AVCodecParameters* dst) const {
    if (!impl_->ctx || !dst) return false;
    return avcodec_parameters_from_context(dst, impl_->ctx.get()) >= 0;
}

AVRational X264VideoEncoder::timeBase() const noexcept {
    return impl_->ctx ? impl_->ctx->time_base : AVRational{1, 1};
}

int X264VideoEncoder::effectiveGopSize() const noexcept {
    return impl_->ctx ? impl_->ctx->gop_size : -1;
}

namespace {
// Locate the SPS NAL (0x67, nal_ref_idc=3, nal_unit_type=7) in an annex-B
// byte stream and return a pointer to the first payload byte (profile_idc).
// Returns nullptr if not found. libx264 emits extradata as annex-B: one or
// more start-code + NAL runs.
const uint8_t* findSpsPayload(const uint8_t* buf, int size) noexcept {
    if (!buf || size < 5) return nullptr;
    for (int i = 0; i + 4 < size; ++i) {
        const bool start3 = (buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 1);
        const bool start4 = (i + 4 < size && buf[i] == 0 && buf[i + 1] == 0
                             && buf[i + 2] == 0 && buf[i + 3] == 1);
        int hdr_off = start4 ? i + 4 : (start3 ? i + 3 : -1);
        if (hdr_off < 0) continue;
        if (hdr_off + 4 >= size) return nullptr;
        const uint8_t nal_header = buf[hdr_off];
        if ((nal_header & 0x1F) == 7)   // SPS
            return &buf[hdr_off + 1];
    }
    return nullptr;
}
}  // namespace

int X264VideoEncoder::effectiveProfileIdc() const noexcept {
    if (!impl_->ctx || !impl_->ctx->extradata) return -1;
    const uint8_t* sps = findSpsPayload(impl_->ctx->extradata,
                                        impl_->ctx->extradata_size);
    return sps ? sps[0] : -1;
}

int X264VideoEncoder::effectiveLevelIdc() const noexcept {
    if (!impl_->ctx || !impl_->ctx->extradata) return -1;
    const uint8_t* sps = findSpsPayload(impl_->ctx->extradata,
                                        impl_->ctx->extradata_size);
    // SPS byte layout: profile_idc, constraint_set_flags, level_idc, ...
    return sps ? sps[2] : -1;
}

}  // namespace liveqx::encoding
