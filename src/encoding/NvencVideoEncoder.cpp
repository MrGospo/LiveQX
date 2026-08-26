#include "encoding/NvencVideoEncoder.h"

#include <atomic>
#include <utility>

#include <spdlog/spdlog.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

namespace liveqx::encoding {

namespace {

struct CodecCtxDeleter   { void operator()(AVCodecContext*  p) const noexcept { if (p) avcodec_free_context(&p); } };
struct FrameDeleter      { void operator()(AVFrame*         p) const noexcept { if (p) av_frame_free(&p); } };
struct PacketDeleter     { void operator()(AVPacket*        p) const noexcept { if (p) av_packet_free(&p); } };
struct SwsDeleter        { void operator()(SwsContext*      p) const noexcept { if (p) sws_freeContext(p); } };
struct HwDeviceDeleter   { void operator()(AVBufferRef*     p) const noexcept { if (p) av_buffer_unref(&p); } };
struct HwFramesDeleter   { void operator()(AVBufferRef*     p) const noexcept { if (p) av_buffer_unref(&p); } };

using CodecCtxPtr = std::unique_ptr<AVCodecContext, CodecCtxDeleter>;
using FramePtr    = std::unique_ptr<AVFrame,        FrameDeleter>;
using PacketPtr   = std::unique_ptr<AVPacket,       PacketDeleter>;
using SwsPtr      = std::unique_ptr<SwsContext,     SwsDeleter>;
using HwDevicePtr = std::unique_ptr<AVBufferRef,    HwDeviceDeleter>;
using HwFramesPtr = std::unique_ptr<AVBufferRef,    HwFramesDeleter>;

}  // namespace

struct NvencVideoEncoder::Impl {
    Config                          cfg;
    std::shared_ptr<spdlog::logger> logger;

    CodecCtxPtr      ctx;
    FramePtr         frame_sw;       // NV12 staging in host memory
    FramePtr         frame_hw;       // CUDA hwframe used as send target
    PacketPtr        enc_pkt;
    SwsPtr           sws;
    HwDevicePtr      hw_device;
    HwFramesPtr      hw_frames;
    int64_t          pts        = 0;
    std::atomic<bool> force_idr{false};
    PacketCallback   packet_cb;

    spdlog::logger& lg() noexcept {
        return logger ? *logger : *spdlog::default_logger();
    }

#ifdef LIVEQX_HAVE_NVENC
    void drainPackets() {
        while (avcodec_receive_packet(ctx.get(), enc_pkt.get()) == 0) {
            if (packet_cb) packet_cb(enc_pkt.get());
            av_packet_unref(enc_pkt.get());
        }
    }
#endif
};

bool NvencVideoEncoder::isBuiltIn() noexcept {
#ifdef LIVEQX_HAVE_NVENC
    return true;
#else
    return false;
#endif
}

NvencVideoEncoder::NvencVideoEncoder(const Config& cfg,
                                     std::shared_ptr<spdlog::logger> logger)
    : impl_(std::make_unique<Impl>()) {
    impl_->cfg    = cfg;
    impl_->logger = std::move(logger);
}

NvencVideoEncoder::~NvencVideoEncoder() { close(); }

#ifdef LIVEQX_HAVE_NVENC

bool NvencVideoEncoder::open() {
    const Config& cfg = impl_->cfg;

    const AVCodec* vcodec = avcodec_find_encoder_by_name("h264_nvenc");
    if (!vcodec) {
        impl_->lg().error("NvencVideoEncoder: h264_nvenc codec not registered "
                          "(FFmpeg built without --enable-nvenc?)");
        return false;
    }

    // CUDA hwdevice — selects the physical GPU via gpu_index. Failure here
    // typically means no NVIDIA driver / no CUDA runtime; surface that to
    // the caller so EncoderFactory can fall through to the next backend.
    AVBufferRef* dev = nullptr;
    char gpu_id[16] = {0};
    std::snprintf(gpu_id, sizeof(gpu_id), "%d", cfg.gpu_index);
    if (av_hwdevice_ctx_create(&dev, AV_HWDEVICE_TYPE_CUDA, gpu_id, nullptr, 0) < 0) {
        impl_->lg().error("NvencVideoEncoder: av_hwdevice_ctx_create(CUDA, gpu={}) failed",
                          cfg.gpu_index);
        return false;
    }
    impl_->hw_device.reset(dev);

    // hwframes pool — software upload target uses NV12 (NVENC native).
    AVBufferRef* hwf = av_hwframe_ctx_alloc(impl_->hw_device.get());
    if (!hwf) {
        impl_->lg().error("NvencVideoEncoder: av_hwframe_ctx_alloc failed");
        return false;
    }
    impl_->hw_frames.reset(hwf);
    auto* hwfctx = reinterpret_cast<AVHWFramesContext*>(impl_->hw_frames->data);
    hwfctx->format    = AV_PIX_FMT_CUDA;
    hwfctx->sw_format = AV_PIX_FMT_NV12;
    hwfctx->width     = cfg.width;
    hwfctx->height    = cfg.height;
    hwfctx->initial_pool_size = 4;
    if (av_hwframe_ctx_init(impl_->hw_frames.get()) < 0) {
        impl_->lg().error("NvencVideoEncoder: av_hwframe_ctx_init failed");
        return false;
    }

    impl_->ctx.reset(avcodec_alloc_context3(vcodec));
    if (!impl_->ctx) return false;
    AVCodecContext* vc = impl_->ctx.get();
    vc->width        = cfg.width;
    vc->height       = cfg.height;
    vc->pix_fmt      = AV_PIX_FMT_CUDA;        // hwframe input
    vc->bit_rate     = cfg.bitrate;
    vc->time_base    = { 1, cfg.fps };
    vc->framerate    = { cfg.fps, 1 };
    vc->gop_size     = cfg.gop_size > 0 ? cfg.gop_size : cfg.fps;
    vc->max_b_frames = cfg.max_b_frames;
    if (cfg.global_header)
        vc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    vc->hw_frames_ctx = av_buffer_ref(impl_->hw_frames.get());

    AVDictionary* vopts = nullptr;
    // NVENC preset names differ from x264. Map x264's "ultrafast/veryfast/
    // medium/slow" onto NVENC's p1..p7 ladder; anything else passes through
    // verbatim so callers can request raw NVENC presets if they want.
    const char* preset_str = cfg.preset.c_str();
    if      (cfg.preset == "ultrafast") preset_str = "p1";
    else if (cfg.preset == "veryfast")  preset_str = "p3";
    else if (cfg.preset == "fast")      preset_str = "p4";
    else if (cfg.preset == "medium")    preset_str = "p5";
    else if (cfg.preset == "slow")      preset_str = "p6";
    else if (cfg.preset == "slower")    preset_str = "p7";
    av_dict_set(&vopts, "preset", preset_str, 0);
    if (cfg.max_b_frames == 0)
        av_dict_set(&vopts, "tune", "ll", 0);   // low-latency, NVENC equivalent of zerolatency
    av_dict_set(&vopts, "rc", "cbr", 0);
    // forced-idr=1 — without this h264_nvenc silently ignores
    // pict_type=AV_PICTURE_TYPE_I on the input frame, so forceKeyframe()
    // would be a no-op and downstream RTMP/HLS reconnects would never
    // recover their decoder. Required for production.
    av_dict_set(&vopts, "forced-idr", "1", 0);
    // repeat_headers=1 — emit SPS/PPS with every IDR (not just the first
    // packet). HLS segmenters and reconnecting muxers need fresh extradata
    // at every keyframe; otherwise a mid-stream subscriber sees a video
    // stream with no parameter sets and silently drops frames.
    av_dict_set(&vopts, "repeat_headers", "1", 0);
    const int vopen = avcodec_open2(vc, vcodec, &vopts);
    av_dict_free(&vopts);
    if (vopen < 0) {
        impl_->lg().error("NvencVideoEncoder: avcodec_open2 failed (driver/codec mismatch?)");
        return false;
    }

    // SWS BGRA→NV12 for CPU-side staging frame.
    impl_->sws.reset(sws_getContext(
        cfg.width, cfg.height, AV_PIX_FMT_RGBA,
        cfg.width, cfg.height, AV_PIX_FMT_NV12,
        SWS_BILINEAR, nullptr, nullptr, nullptr));
    if (!impl_->sws) {
        impl_->lg().error("NvencVideoEncoder: sws_getContext(BGRA→NV12) failed");
        return false;
    }

    impl_->frame_sw.reset(av_frame_alloc());
    if (!impl_->frame_sw) return false;
    impl_->frame_sw->format = AV_PIX_FMT_NV12;
    impl_->frame_sw->width  = cfg.width;
    impl_->frame_sw->height = cfg.height;
    if (av_frame_get_buffer(impl_->frame_sw.get(), 32) < 0) return false;

    impl_->frame_hw.reset(av_frame_alloc());
    if (!impl_->frame_hw) return false;
    if (av_hwframe_get_buffer(impl_->hw_frames.get(), impl_->frame_hw.get(), 0) < 0) {
        impl_->lg().error("NvencVideoEncoder: av_hwframe_get_buffer failed");
        return false;
    }

    impl_->enc_pkt.reset(av_packet_alloc());
    if (!impl_->enc_pkt) return false;

    impl_->pts = 0;
    return true;
}

void NvencVideoEncoder::close() {
    impl_->enc_pkt.reset();
    impl_->frame_hw.reset();
    impl_->frame_sw.reset();
    impl_->ctx.reset();
    impl_->sws.reset();
    impl_->hw_frames.reset();
    impl_->hw_device.reset();
    impl_->pts = 0;
}

void NvencVideoEncoder::pushFrame(const Frame& bgra) {
    if (!impl_->ctx || !impl_->sws || !impl_->frame_sw ||
        !impl_->frame_hw || !impl_->enc_pkt) return;
    if (!bgra.valid()) return;

    // 1. CPU side: BGRA → NV12 staging buffer.
    av_frame_make_writable(impl_->frame_sw.get());
    const uint8_t* src_data[4]   = { bgra.pixels(), nullptr, nullptr, nullptr };
    const int      src_stride[4] = { bgra.width * 4, 0, 0, 0 };
    sws_scale(impl_->sws.get(),
              src_data, src_stride, 0, bgra.height,
              impl_->frame_sw->data, impl_->frame_sw->linesize);

    // 2. GPU upload: frame_sw (host NV12) → frame_hw (CUDA).
    // av_hwframe_transfer_data drives the cudaMemcpy under the hood; if
    // it fails we drop the frame rather than crash — the encoder will
    // surface the underrun via the next drainPackets() call.
    if (av_hwframe_transfer_data(impl_->frame_hw.get(),
                                 impl_->frame_sw.get(), 0) < 0) {
        impl_->lg().warn("NvencVideoEncoder: av_hwframe_transfer_data failed; dropping frame");
        return;
    }

    if (impl_->force_idr.exchange(false, std::memory_order_relaxed)) {
        impl_->frame_hw->pict_type = AV_PICTURE_TYPE_I;
        impl_->frame_hw->flags    |= AV_FRAME_FLAG_KEY;
    } else {
        impl_->frame_hw->pict_type = AV_PICTURE_TYPE_NONE;
        impl_->frame_hw->flags    &= ~AV_FRAME_FLAG_KEY;
    }
    impl_->frame_hw->pts = impl_->pts++;

    if (avcodec_send_frame(impl_->ctx.get(), impl_->frame_hw.get()) == 0) {
        impl_->drainPackets();
    }
}

void NvencVideoEncoder::drain() {
    if (!impl_->ctx) return;
    avcodec_send_frame(impl_->ctx.get(), nullptr);
    impl_->drainPackets();
}

void NvencVideoEncoder::forceKeyframe() noexcept {
    impl_->force_idr.store(true, std::memory_order_relaxed);
}

void NvencVideoEncoder::setPacketCallback(PacketCallback cb) {
    impl_->packet_cb = std::move(cb);
}

bool NvencVideoEncoder::fillStreamParameters(AVCodecParameters* dst) const {
    if (!impl_->ctx || !dst) return false;
    return avcodec_parameters_from_context(dst, impl_->ctx.get()) >= 0;
}

AVRational NvencVideoEncoder::timeBase() const noexcept {
    return impl_->ctx ? impl_->ctx->time_base : AVRational{1, 1};
}

#else  // !LIVEQX_HAVE_NVENC — stub backend, fails gracefully.

bool NvencVideoEncoder::open() {
    impl_->lg().warn("NvencVideoEncoder: binary built without ENABLE_NVENC; "
                     "rebuild FFmpeg+LiveQX with NVENC support");
    return false;
}

void NvencVideoEncoder::close()                                    {}
void NvencVideoEncoder::pushFrame(const Frame&)                    {}
void NvencVideoEncoder::drain()                                    {}
void NvencVideoEncoder::forceKeyframe() noexcept                   {}
void NvencVideoEncoder::setPacketCallback(PacketCallback)          {}
bool NvencVideoEncoder::fillStreamParameters(AVCodecParameters*) const { return false; }
AVRational NvencVideoEncoder::timeBase() const noexcept            { return AVRational{1, 1}; }

#endif  // LIVEQX_HAVE_NVENC

}  // namespace liveqx::encoding
