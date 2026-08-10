#include "audio/AudioDecoder.h"
#include "utils/Log.h"

#include <stdexcept>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}

namespace {

struct FmtCtxDeleter   { void operator()(AVFormatContext* p) const noexcept { if (p) avformat_close_input(&p); } };
struct CodecCtxDeleter { void operator()(AVCodecContext*  p) const noexcept { if (p) avcodec_free_context(&p); } };
struct SwrCtxDeleter   { void operator()(SwrContext*      p) const noexcept { if (p) swr_free(&p); } };
struct AvFrameDeleter  { void operator()(AVFrame*         p) const noexcept { if (p) av_frame_free(&p); } };
struct AvPacketDeleter { void operator()(AVPacket*        p) const noexcept { if (p) av_packet_free(&p); } };

using FmtCtxPtr   = std::unique_ptr<AVFormatContext, FmtCtxDeleter>;
using CodecCtxPtr = std::unique_ptr<AVCodecContext,  CodecCtxDeleter>;
using SwrCtxPtr   = std::unique_ptr<SwrContext,      SwrCtxDeleter>;
using AvFramePtr  = std::unique_ptr<AVFrame,         AvFrameDeleter>;
using AvPacketPtr = std::unique_ptr<AVPacket,        AvPacketDeleter>;

} // namespace

// ─── Impl ─────────────────────────────────────────────────────────────────────

struct AudioDecoder::Impl {
    std::string path;
    bool at_end = false;

    FmtCtxPtr   fmt_ctx;
    CodecCtxPtr audio_ctx;
    SwrCtxPtr   swr_ctx;
    int         audio_stream_idx = -1;
    AVRational  audio_tb{};

    AvFramePtr  frm{av_frame_alloc()};
    AvPacketPtr pkt{av_packet_alloc()};

    static constexpr int kOutRate = 48000;
    static constexpr int kOutCh   = 2;
};

// ─── AudioDecoder ─────────────────────────────────────────────────────────────

AudioDecoder::AudioDecoder(const std::string& path)
    : impl_(std::make_unique<Impl>()) {
    impl_->path = path;
}

AudioDecoder::~AudioDecoder() { close(); }

bool AudioDecoder::open() {
    auto impl = std::make_unique<Impl>();
    impl->path = impl_->path;
    impl->frm.reset(av_frame_alloc());
    impl->pkt.reset(av_packet_alloc());
    if (!impl->frm || !impl->pkt) return false;

    // ── Open container ────────────────────────────────────────────────────────
    {
        AVFormatContext* raw = nullptr;
        if (avformat_open_input(&raw, impl->path.c_str(), nullptr, nullptr) < 0)
            return false;
        impl->fmt_ctx.reset(raw);
    }
    if (avformat_find_stream_info(impl->fmt_ctx.get(), nullptr) < 0)
        return false;

    // ── Find first audio stream ───────────────────────────────────────────────
    for (unsigned i = 0; i < impl->fmt_ctx->nb_streams; ++i) {
        if (impl->fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            impl->audio_stream_idx = static_cast<int>(i);
            break;
        }
    }
    if (impl->audio_stream_idx < 0) return false;

    // ── Open codec ────────────────────────────────────────────────────────────
    const AVStream*          st  = impl->fmt_ctx->streams[impl->audio_stream_idx];
    const AVCodecParameters* par = st->codecpar;
    impl->audio_tb = st->time_base;

    const AVCodec* codec = avcodec_find_decoder(par->codec_id);
    if (!codec) return false;

    CodecCtxPtr ctx{avcodec_alloc_context3(codec)};
    if (!ctx || avcodec_parameters_to_context(ctx.get(), par) < 0) return false;
    if (avcodec_open2(ctx.get(), codec, nullptr) < 0) return false;

    // ── Init SWR: any input format → 48kHz stereo float ──────────────────────
    AVChannelLayout in_layout{};
    if (ctx->ch_layout.nb_channels > 0)
        in_layout = ctx->ch_layout;
    else
        av_channel_layout_default(&in_layout, par->ch_layout.nb_channels > 0
                                              ? par->ch_layout.nb_channels : 1);

    AVChannelLayout out_layout = AV_CHANNEL_LAYOUT_STEREO;
    SwrContext* swr_raw = nullptr;
    if (swr_alloc_set_opts2(&swr_raw,
            &out_layout, AV_SAMPLE_FMT_FLT, Impl::kOutRate,
            &in_layout,  ctx->sample_fmt,   ctx->sample_rate,
            0, nullptr) < 0 || !swr_raw)
        return false;
    if (swr_init(swr_raw) < 0) { swr_free(&swr_raw); return false; }

    impl->swr_ctx.reset(swr_raw);
    impl->audio_ctx = std::move(ctx);
    impl_ = std::move(impl);
    return true;
}

void AudioDecoder::close() {
    impl_->swr_ctx.reset();
    impl_->audio_ctx.reset();
    impl_->fmt_ctx.reset();
    impl_->at_end = false;
}

AudioFrame AudioDecoder::decodeNext() {
    if (impl_->at_end || !impl_->audio_ctx) return {};

    auto& i = *impl_;

    while (true) {
        int ret = avcodec_receive_frame(i.audio_ctx.get(), i.frm.get());

        if (ret == 0) {
            const int max_out = swr_get_out_samples(i.swr_ctx.get(), i.frm->nb_samples) + 256;
            AudioFrame af;
            af.samples.resize(static_cast<size_t>(max_out) * Impl::kOutCh);

            uint8_t* out_ptr = reinterpret_cast<uint8_t*>(af.samples.data());
            int out_samples  = swr_convert(i.swr_ctx.get(),
                &out_ptr, max_out,
                const_cast<const uint8_t**>(i.frm->data), i.frm->nb_samples);

            int64_t pts_us = 0;
            if (i.frm->pts != AV_NOPTS_VALUE && i.audio_tb.den > 0)
                pts_us = static_cast<int64_t>(
                    static_cast<double>(i.frm->pts) * i.audio_tb.num
                    * AV_TIME_BASE / i.audio_tb.den);

            av_frame_unref(i.frm.get());
            if (out_samples <= 0) continue;

            af.samples.resize(static_cast<size_t>(out_samples) * Impl::kOutCh);
            af.num_samples = out_samples;
            af.sample_rate = Impl::kOutRate;
            af.channels    = Impl::kOutCh;
            af.pts         = pts_us;
            af.valid       = true;
            return af;
        }

        if (ret != AVERROR(EAGAIN)) {
            i.at_end = true;
            return {};
        }

        // Codec needs more input
        ret = av_read_frame(i.fmt_ctx.get(), i.pkt.get());
        if (ret < 0) {
            avcodec_send_packet(i.audio_ctx.get(), nullptr); // flush
            continue;
        }

        if (i.pkt->stream_index == i.audio_stream_idx)
            avcodec_send_packet(i.audio_ctx.get(), i.pkt.get());

        av_packet_unref(i.pkt.get());
    }
}

bool AudioDecoder::atEnd() const { return impl_->at_end; }
