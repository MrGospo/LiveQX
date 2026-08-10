#include "audio/AudioEncoder.h"
#include "utils/Log.h"

#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
}

namespace {
struct CodecCtxDeleter { void operator()(AVCodecContext* p) const noexcept { if (p) avcodec_free_context(&p); } };
struct FrameDeleter    { void operator()(AVFrame*        p) const noexcept { if (p) av_frame_free(&p); } };
struct PacketDeleter   { void operator()(AVPacket*       p) const noexcept { if (p) av_packet_free(&p); } };

using CodecCtxPtr = std::unique_ptr<AVCodecContext, CodecCtxDeleter>;
using FramePtr    = std::unique_ptr<AVFrame,        FrameDeleter>;
using PacketPtr   = std::unique_ptr<AVPacket,       PacketDeleter>;
} // namespace

// ─── Impl ─────────────────────────────────────────────────────────────────────

struct AudioEncoder::Impl {
    CodecCtxPtr audio_ctx;
    FramePtr    audio_frame;
    PacketPtr   enc_pkt;
    int         frame_size = 1024;  // samples per channel per AAC frame
    int64_t     pts        = 0;

    std::vector<float> buf;         // interleaved stereo float accumulator
    size_t             buf_pos = 0; // read offset (avoids O(n) erase)
};

// ─── AudioEncoder ─────────────────────────────────────────────────────────────

AudioEncoder::AudioEncoder(int bitrate, int sample_rate, int channels)
    : impl_(std::make_unique<Impl>()),
      bitrate_(bitrate), sample_rate_(sample_rate), channels_(channels) {}

AudioEncoder::~AudioEncoder() { close(); }

bool AudioEncoder::open() {
    close();

    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!codec) {
        LOG_ERROR("AudioEncoder: AAC encoder not found");
        return false;
    }

    CodecCtxPtr ctx{avcodec_alloc_context3(codec)};
    if (!ctx) return false;

    ctx->sample_rate = sample_rate_;
    ctx->bit_rate    = bitrate_;
    ctx->sample_fmt  = AV_SAMPLE_FMT_FLTP;
    ctx->time_base   = { 1, sample_rate_ };
    av_channel_layout_default(&ctx->ch_layout, channels_);

    if (avcodec_open2(ctx.get(), codec, nullptr) < 0) {
        LOG_ERROR("AudioEncoder: avcodec_open2 failed");
        return false;
    }

    const int fs = ctx->frame_size > 0 ? ctx->frame_size : 1024;

    FramePtr af{av_frame_alloc()};
    if (!af) return false;
    af->format      = AV_SAMPLE_FMT_FLTP;
    af->sample_rate = sample_rate_;
    af->nb_samples  = fs;
    av_channel_layout_default(&af->ch_layout, channels_);
    if (av_frame_get_buffer(af.get(), 0) < 0) return false;

    PacketPtr pkt{av_packet_alloc()};
    if (!pkt) return false;

    impl_->audio_ctx   = std::move(ctx);
    impl_->audio_frame = std::move(af);
    impl_->enc_pkt     = std::move(pkt);
    impl_->frame_size  = fs;
    impl_->pts         = 0;
    impl_->buf.clear();
    impl_->buf_pos = 0;

    return true;
}

void AudioEncoder::close() {
    if (!impl_->audio_ctx) return;
    // Flush
    avcodec_send_frame(impl_->audio_ctx.get(), nullptr);
    while (avcodec_receive_packet(impl_->audio_ctx.get(),
                                   impl_->enc_pkt.get()) == 0)
        av_packet_unref(impl_->enc_pkt.get());

    impl_->audio_ctx.reset();
    impl_->audio_frame.reset();
    impl_->enc_pkt.reset();
    impl_->buf.clear();
    impl_->buf_pos = 0;
    impl_->pts = 0;
}

Packet AudioEncoder::encode(const AudioFrame& input) {
    if (!impl_->audio_ctx || !input.valid) return {};

    // Append samples to accumulator
    impl_->buf.insert(impl_->buf.end(),
                      input.samples.begin(), input.samples.end());

    const int chunk = impl_->frame_size * channels_;
    if (static_cast<int>(impl_->buf.size() - impl_->buf_pos) < chunk)
        return {}; // not enough samples yet

    av_frame_make_writable(impl_->audio_frame.get());

    // De-interleave: interleaved L R L R → planar ch0[] ch1[]
    for (int ch = 0; ch < channels_; ++ch) {
        float* plane = reinterpret_cast<float*>(
            impl_->audio_frame->data[ch]);
        for (int i = 0; i < impl_->frame_size; ++i)
            plane[i] = impl_->buf[impl_->buf_pos + i * channels_ + ch];
    }

    impl_->audio_frame->pts = impl_->pts;
    impl_->pts += impl_->frame_size;

    impl_->buf_pos += static_cast<size_t>(chunk);
    if (impl_->buf_pos > 8192) {
        impl_->buf.erase(impl_->buf.begin(),
                         impl_->buf.begin() + impl_->buf_pos);
        impl_->buf_pos = 0;
    }

    if (avcodec_send_frame(impl_->audio_ctx.get(),
                           impl_->audio_frame.get()) < 0)
        return {};

    if (avcodec_receive_packet(impl_->audio_ctx.get(),
                                impl_->enc_pkt.get()) < 0)
        return {};

    Packet out;
    out.data.assign(impl_->enc_pkt->data,
                    impl_->enc_pkt->data + impl_->enc_pkt->size);
    out.pts      = impl_->enc_pkt->pts;
    out.is_video = false;
    av_packet_unref(impl_->enc_pkt.get());
    return out;
}
