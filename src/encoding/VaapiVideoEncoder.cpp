#include "encoding/VaapiVideoEncoder.h"

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

#ifdef LIVEQX_HAVE_VAAPI
// h264_vaapi has no string preset — it uses an integer "quality" in 1..7
// (lower = slower = better). Map x264 vocabulary onto that range so
// callers can keep using the same preset names across backends.
const char* mapPresetToQuality(const std::string& preset) {
    if (preset == "ultrafast" || preset == "superfast") return "7";
    if (preset == "veryfast")                           return "6";
    if (preset == "faster")                             return "5";
    if (preset == "fast")                               return "4";
    if (preset == "medium")                             return "4";
    if (preset == "slow")                               return "3";
    if (preset == "slower")                             return "2";
    if (preset == "veryslow" || preset == "placebo")    return "1";
    return "4";  // medium-ish default
}
#endif

}  // namespace

struct VaapiVideoEncoder::Impl {
    Config                          cfg;
    std::shared_ptr<spdlog::logger> logger;

    CodecCtxPtr      ctx;
    FramePtr         frame_sw;
    FramePtr         frame_hw;
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

#ifdef LIVEQX_HAVE_VAAPI
    void drainPackets() {
        while (avcodec_receive_packet(ctx.get(), enc_pkt.get()) == 0) {
            if (packet_cb) packet_cb(enc_pkt.get());
            av_packet_unref(enc_pkt.get());
        }
    }
#endif
};

bool VaapiVideoEncoder::isBuiltIn() noexcept {
#ifdef LIVEQX_HAVE_VAAPI
    return true;
#else
    return false;
#endif
}

VaapiVideoEncoder::VaapiVideoEncoder(const Config& cfg,
                                     std::shared_ptr<spdlog::logger> logger)
    : impl_(std::make_unique<Impl>()) {
    impl_->cfg    = cfg;
    impl_->logger = std::move(logger);
}

VaapiVideoEncoder::~VaapiVideoEncoder() { close(); }

#ifdef LIVEQX_HAVE_VAAPI

bool VaapiVideoEncoder::open() {
    const Config& cfg = impl_->cfg;

    const AVCodec* vcodec = avcodec_find_encoder_by_name("h264_vaapi");
    if (!vcodec) {
        impl_->lg().error("VaapiVideoEncoder: h264_vaapi codec not registered "
                          "(FFmpeg built without --enable-vaapi?)");
        return false;
    }

    AVBufferRef* dev = nullptr;
    char dev_path[64] = {0};
    std::snprintf(dev_path, sizeof(dev_path), "/dev/dri/renderD%d",
                  128 + cfg.gpu_index);
    if (av_hwdevice_ctx_create(&dev, AV_HWDEVICE_TYPE_VAAPI, dev_path,
                               nullptr, 0) < 0) {
        impl_->lg().error("VaapiVideoEncoder: av_hwdevice_ctx_create(VAAPI, \"{}\") failed",
                          dev_path);
        return false;
    }
    impl_->hw_device.reset(dev);

    AVBufferRef* hwf = av_hwframe_ctx_alloc(impl_->hw_device.get());
    if (!hwf) {
        impl_->lg().error("VaapiVideoEncoder: av_hwframe_ctx_alloc failed");
        return false;
    }
    impl_->hw_frames.reset(hwf);
    auto* hwfctx = reinterpret_cast<AVHWFramesContext*>(impl_->hw_frames->data);
    hwfctx->format    = AV_PIX_FMT_VAAPI;
    hwfctx->sw_format = AV_PIX_FMT_NV12;
    hwfctx->width     = cfg.width;
    hwfctx->height    = cfg.height;
    hwfctx->initial_pool_size = 4;
    if (av_hwframe_ctx_init(impl_->hw_frames.get()) < 0) {
        impl_->lg().error("VaapiVideoEncoder: av_hwframe_ctx_init failed");
        return false;
    }

    impl_->ctx.reset(avcodec_alloc_context3(vcodec));
    if (!impl_->ctx) return false;
    AVCodecContext* vc = impl_->ctx.get();
    vc->width        = cfg.width;
    vc->height       = cfg.height;
    vc->pix_fmt      = AV_PIX_FMT_VAAPI;
    vc->bit_rate     = cfg.bitrate;
    vc->time_base    = { 1, cfg.fps };
    vc->framerate    = { cfg.fps, 1 };
    vc->gop_size     = cfg.gop_size > 0 ? cfg.gop_size : cfg.fps;
    vc->max_b_frames = cfg.max_b_frames;
    if (cfg.global_header)
        vc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    vc->hw_frames_ctx = av_buffer_ref(impl_->hw_frames.get());

    AVDictionary* vopts = nullptr;
    av_dict_set(&vopts, "quality", mapPresetToQuality(cfg.preset), 0);
    av_dict_set(&vopts, "rc_mode", "CBR", 0);
    // Same forced-IDR caveat as NVENC/QSV — without this h264_vaapi may
    // ignore pict_type=I and emit a normal P-frame.
    av_dict_set(&vopts, "forced_idr", "1", 0);
    const int vopen = avcodec_open2(vc, vcodec, &vopts);
    av_dict_free(&vopts);
    if (vopen < 0) {
        impl_->lg().error("VaapiVideoEncoder: avcodec_open2 failed (driver/codec mismatch?)");
        return false;
    }

    impl_->sws.reset(sws_getContext(
        cfg.width, cfg.height, AV_PIX_FMT_RGBA,
        cfg.width, cfg.height, AV_PIX_FMT_NV12,
        SWS_BILINEAR, nullptr, nullptr, nullptr));
    if (!impl_->sws) {
        impl_->lg().error("VaapiVideoEncoder: sws_getContext(BGRA→NV12) failed");
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
        impl_->lg().error("VaapiVideoEncoder: av_hwframe_get_buffer failed");
        return false;
    }

    impl_->enc_pkt.reset(av_packet_alloc());
    if (!impl_->enc_pkt) return false;

    impl_->pts = 0;
    return true;
}

void VaapiVideoEncoder::close() {
    impl_->enc_pkt.reset();
    impl_->frame_hw.reset();
    impl_->frame_sw.reset();
    impl_->ctx.reset();
    impl_->sws.reset();
    impl_->hw_frames.reset();
    impl_->hw_device.reset();
    impl_->pts = 0;
}

void VaapiVideoEncoder::pushFrame(const Frame& bgra) {
    if (!impl_->ctx || !impl_->sws || !impl_->frame_sw ||
        !impl_->frame_hw || !impl_->enc_pkt) return;
    if (!bgra.valid()) return;

    av_frame_make_writable(impl_->frame_sw.get());
    const uint8_t* src_data[4]   = { bgra.pixels(), nullptr, nullptr, nullptr };
    const int      src_stride[4] = { bgra.width * 4, 0, 0, 0 };
    sws_scale(impl_->sws.get(),
              src_data, src_stride, 0, bgra.height,
              impl_->frame_sw->data, impl_->frame_sw->linesize);

    if (av_hwframe_transfer_data(impl_->frame_hw.get(),
                                 impl_->frame_sw.get(), 0) < 0) {
        impl_->lg().warn("VaapiVideoEncoder: av_hwframe_transfer_data failed; dropping frame");
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

void VaapiVideoEncoder::drain() {
    if (!impl_->ctx) return;
    avcodec_send_frame(impl_->ctx.get(), nullptr);
    impl_->drainPackets();
}

void VaapiVideoEncoder::forceKeyframe() noexcept {
    impl_->force_idr.store(true, std::memory_order_relaxed);
}

void VaapiVideoEncoder::setPacketCallback(PacketCallback cb) {
    impl_->packet_cb = std::move(cb);
}

bool VaapiVideoEncoder::fillStreamParameters(AVCodecParameters* dst) const {
    if (!impl_->ctx || !dst) return false;
    return avcodec_parameters_from_context(dst, impl_->ctx.get()) >= 0;
}

AVRational VaapiVideoEncoder::timeBase() const noexcept {
    return impl_->ctx ? impl_->ctx->time_base : AVRational{1, 1};
}

#else  // !LIVEQX_HAVE_VAAPI

bool VaapiVideoEncoder::open() {
    impl_->lg().warn("VaapiVideoEncoder: binary built without ENABLE_VAAPI; "
                     "rebuild FFmpeg+LiveQX with VAAPI support");
    return false;
}

void VaapiVideoEncoder::close()                                    {}
void VaapiVideoEncoder::pushFrame(const Frame&)                    {}
void VaapiVideoEncoder::drain()                                    {}
void VaapiVideoEncoder::forceKeyframe() noexcept                   {}
void VaapiVideoEncoder::setPacketCallback(PacketCallback)          {}
bool VaapiVideoEncoder::fillStreamParameters(AVCodecParameters*) const { return false; }
AVRational VaapiVideoEncoder::timeBase() const noexcept            { return AVRational{1, 1}; }

#endif  // LIVEQX_HAVE_VAAPI

}  // namespace liveqx::encoding
