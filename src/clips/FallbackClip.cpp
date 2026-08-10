#include "clips/FallbackClip.h"
#include <algorithm>
#include <limits>
#include <stdexcept>
#include "utils/Log.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

namespace {
struct FmtCtxDeleter  { void operator()(AVFormatContext* p) const noexcept { if (p) avformat_close_input(&p); } };
struct CodecCtxDeleter { void operator()(AVCodecContext* p) const noexcept { if (p) avcodec_free_context(&p); } };
struct AvFrameDeleter  { void operator()(AVFrame*        p) const noexcept { if (p) av_frame_free(&p); } };
struct AvPacketDeleter { void operator()(AVPacket*       p) const noexcept { if (p) av_packet_free(&p); } };
struct SwsCtxDeleter   { void operator()(SwsContext*     p) const noexcept { if (p) sws_freeContext(p); } };
using FmtCtxPtr   = std::unique_ptr<AVFormatContext, FmtCtxDeleter>;
using CodecCtxPtr = std::unique_ptr<AVCodecContext,  CodecCtxDeleter>;
using AvFramePtr  = std::unique_ptr<AVFrame,         AvFrameDeleter>;
using AvPacketPtr = std::unique_ptr<AVPacket,        AvPacketDeleter>;
using SwsCtxPtr   = std::unique_ptr<SwsContext,      SwsCtxDeleter>;
} // namespace

FallbackClip::FallbackClip(const std::string& image_path,
                            int out_width, int out_height)
    : image_path_(image_path), out_width_(out_width), out_height_(out_height) {}

void FallbackClip::prepare() {
    if (image_path_.empty()) {
        // No image configured — use solid black frame.
        const size_t sz = static_cast<size_t>(out_width_) * out_height_ * 4;
        auto buf = std::make_shared<uint8_t[]>(sz);
        std::fill_n(buf.get(), sz, uint8_t{0});
        cached_frame_.data   = std::move(buf);
        cached_frame_.width  = out_width_;
        cached_frame_.height = out_height_;
        cached_frame_.pts    = 0;
        LOG_WARN("FallbackClip: no image path — using black frame");
        return;
    }

    AVFormatContext* raw = nullptr;
    if (avformat_open_input(&raw, image_path_.c_str(), nullptr, nullptr) != 0)
        throw std::runtime_error("FallbackClip: cannot open '" + image_path_ + "'");
    FmtCtxPtr fmt(raw);

    if (avformat_find_stream_info(fmt.get(), nullptr) < 0)
        throw std::runtime_error("FallbackClip: cannot read stream info: " + image_path_);

    int stream_idx = -1;
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            stream_idx = static_cast<int>(i);
            break;
        }
    }
    if (stream_idx < 0)
        throw std::runtime_error("FallbackClip: no video stream in " + image_path_);

    const AVCodecParameters* par = fmt->streams[stream_idx]->codecpar;
    const AVCodec* codec = avcodec_find_decoder(par->codec_id);
    if (!codec)
        throw std::runtime_error("FallbackClip: unsupported codec in " + image_path_);

    CodecCtxPtr ctx(avcodec_alloc_context3(codec));
    if (!ctx || avcodec_parameters_to_context(ctx.get(), par) < 0
             || avcodec_open2(ctx.get(), codec, nullptr) < 0)
        throw std::runtime_error("FallbackClip: codec open failed: " + image_path_);

    AvPacketPtr pkt(av_packet_alloc());
    AvFramePtr  av_frm(av_frame_alloc());
    if (!pkt || !av_frm)
        throw std::runtime_error("FallbackClip: allocation failed");

    bool got_frame = false;
    while (!got_frame && av_read_frame(fmt.get(), pkt.get()) >= 0) {
        if (pkt->stream_index == stream_idx) {
            if (avcodec_send_packet(ctx.get(), pkt.get()) == 0 &&
                avcodec_receive_frame(ctx.get(), av_frm.get()) == 0)
                got_frame = true;
        }
        av_packet_unref(pkt.get());
    }
    if (!got_frame)
        throw std::runtime_error("FallbackClip: could not decode frame from " + image_path_);

    const int src_w   = ctx->width;
    const int src_h   = ctx->height;
    const auto src_fmt = static_cast<AVPixelFormat>(av_frm->format);

    SwsCtxPtr sws(sws_getContext(src_w, src_h, src_fmt,
                                 out_width_, out_height_, AV_PIX_FMT_RGBA,
                                 SWS_BILINEAR, nullptr, nullptr, nullptr));
    if (!sws)
        throw std::runtime_error("FallbackClip: sws_getContext failed: " + image_path_);

    const size_t sz = static_cast<size_t>(out_width_) * out_height_ * 4;
    auto buf = std::make_shared<uint8_t[]>(sz);

    uint8_t* dst_data[4]   = { buf.get(), nullptr, nullptr, nullptr };
    int      dst_stride[4] = { out_width_ * 4, 0, 0, 0 };
    sws_scale(sws.get(), av_frm->data, av_frm->linesize, 0, src_h, dst_data, dst_stride);

    cached_frame_.data   = std::move(buf);
    cached_frame_.width  = out_width_;
    cached_frame_.height = out_height_;
    cached_frame_.pts    = 0;

    LOG_INFO("FallbackClip: decoded {}x{} → {}x{} RGBA [{}]",
             src_w, src_h, out_width_, out_height_, image_path_);
}

void FallbackClip::release() {}

Frame FallbackClip::getFrame() { return cached_frame_; }

AudioFrame FallbackClip::getAudio(int num_samples) {
    AudioFrame silence;
    silence.num_samples = num_samples;
    silence.samples.assign(static_cast<size_t>(num_samples) * silence.channels, 0.0f);
    silence.valid = true;
    return silence;
}

double FallbackClip::getDuration() const {
    return std::numeric_limits<double>::infinity();
}

bool FallbackClip::hasAudio()   const { return false; }
bool FallbackClip::isPrepared() const { return cached_frame_.valid(); }

Frame FallbackClip::getTailFrame() { return cached_frame_; }

AudioFrame FallbackClip::getTailAudio(int num_samples) {
    return getAudio(num_samples);
}

void FallbackClip::reset() {}
