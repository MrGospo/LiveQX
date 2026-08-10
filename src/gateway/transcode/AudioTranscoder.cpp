#include "gateway/transcode/AudioTranscoder.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <utility>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

namespace liveqx::gateway::transcode {

namespace {

struct CodecCtxDeleter { void operator()(AVCodecContext* p) const noexcept { if (p) avcodec_free_context(&p); } };
struct FrameDeleter    { void operator()(AVFrame*         p) const noexcept { if (p) av_frame_free(&p); } };
struct PacketDeleter   { void operator()(AVPacket*        p) const noexcept { if (p) av_packet_free(&p); } };
struct SwrDeleter      { void operator()(SwrContext*      p) const noexcept { if (p) swr_free(&p); } };
struct AudioFifoDeleter{ void operator()(AVAudioFifo*     p) const noexcept { if (p) av_audio_fifo_free(p); } };

using CodecCtxPtr = std::unique_ptr<AVCodecContext, CodecCtxDeleter>;
using FramePtr    = std::unique_ptr<AVFrame,         FrameDeleter>;
using PacketPtr   = std::unique_ptr<AVPacket,        PacketDeleter>;
using SwrPtr      = std::unique_ptr<SwrContext,      SwrDeleter>;
using AudioFifoPtr= std::unique_ptr<AVAudioFifo,     AudioFifoDeleter>;

constexpr AVRational kTbTs90k = {1, 90000};

// ISO/IEC 14496-3 Table 1.16 (sampling_frequency_index).
constexpr std::array<int, 13> kAacSampleRates = {
    96000, 88200, 64000, 48000, 44100, 32000,
    24000, 22050, 16000, 12000, 11025, 8000, 7350,
};

}  // namespace

std::uint8_t aacSampleRateIndex(int sample_rate) noexcept {
    for (std::uint8_t i = 0; i < kAacSampleRates.size(); ++i) {
        if (kAacSampleRates[i] == sample_rate) return i;
    }
    return 0xFF;
}

void writeAdtsHeader(std::uint8_t* h,
                     std::uint8_t  profile_minus_one,
                     std::uint8_t  sample_rate_idx,
                     std::uint8_t  channel_cfg,
                     std::size_t   aac_payload_len) noexcept {
    const std::uint16_t frame_length = static_cast<std::uint16_t>(aac_payload_len + 7u);
    h[0] = 0xFF;                                          // syncword high
    // syncword low (4) | ID(1)=0(MPEG-4) | layer(2)=00 | protect_absent(1)=1
    h[1] = 0xF1;
    // profile(2) | sr_idx(4) | private(1)=0 | ch_cfg high bit
    h[2] = static_cast<std::uint8_t>(((profile_minus_one & 0x03) << 6) |
                                     ((sample_rate_idx   & 0x0F) << 2) |
                                     ((channel_cfg       >> 2)   & 0x01));
    // ch_cfg low 2 bits | original=0 | home=0 | copyright_id=0 | copyright_id_start=0 | frame_length top 2 bits
    h[3] = static_cast<std::uint8_t>(((channel_cfg & 0x03) << 6) |
                                     ((frame_length >> 11) & 0x03));
    h[4] = static_cast<std::uint8_t>((frame_length >> 3) & 0xFF);
    // frame_length low 3 bits | buffer_fullness top 5 bits (set to 0x7FF → top 5 = 0x1F)
    h[5] = static_cast<std::uint8_t>(((frame_length & 0x07) << 5) | 0x1F);
    // buffer_fullness low 6 bits | n_raw_data_blocks(2)=0
    h[6] = 0xFC;
}

struct AudioTranscoder::Impl {
    CodecCtxPtr   dec_ctx;
    CodecCtxPtr   enc_ctx;
    SwrPtr        swr;
    AudioFifoPtr  fifo;

    PacketPtr     in_pkt;
    FramePtr      dec_frame;
    FramePtr      enc_frame;
    PacketPtr     out_pkt;

    int input_sample_rate = 0;
    int input_channels    = 0;

    int  out_sample_rate  = 0;
    int  out_channels     = 0;
    int  out_frame_size   = 0;          // samples per AAC frame (typically 1024)
    std::int64_t pts_samples_emitted = 0;  // running sample counter for output PTS
    std::int64_t first_pts_90k       = AV_NOPTS_VALUE;

    std::uint8_t adts_profile_minus_one = 1;   // LC = 1
    std::uint8_t adts_sr_idx            = 0;
    std::uint8_t adts_ch_cfg            = 2;
};

AudioTranscoder::AudioTranscoder(TranscodeCfg cfg)
    : impl_(std::make_unique<Impl>()), cfg_(std::move(cfg)) {}

AudioTranscoder::~AudioTranscoder() = default;

int AudioTranscoder::outputSampleRate() const noexcept {
    return impl_ ? impl_->out_sample_rate : 0;
}
int AudioTranscoder::outputChannels() const noexcept {
    return impl_ ? impl_->out_channels : 0;
}

bool AudioTranscoder::init(int input_avcodec_id) {
    if (decoder_open_) return true;

    const AVCodec* dec = avcodec_find_decoder(static_cast<AVCodecID>(input_avcodec_id));
    if (!dec) return false;

    impl_->dec_ctx.reset(avcodec_alloc_context3(dec));
    if (!impl_->dec_ctx) return false;

    impl_->dec_ctx->time_base    = kTbTs90k;
    impl_->dec_ctx->pkt_timebase = kTbTs90k;

    if (avcodec_open2(impl_->dec_ctx.get(), dec, nullptr) < 0) {
        impl_->dec_ctx.reset();
        return false;
    }

    impl_->in_pkt.reset(av_packet_alloc());
    impl_->dec_frame.reset(av_frame_alloc());
    impl_->out_pkt.reset(av_packet_alloc());
    impl_->enc_frame.reset(av_frame_alloc());
    if (!impl_->in_pkt || !impl_->dec_frame || !impl_->out_pkt || !impl_->enc_frame) {
        impl_->dec_ctx.reset();
        return false;
    }

    decoder_open_ = true;
    return true;
}

bool AudioTranscoder::openEncoderForInput(int input_sample_rate, int input_channels) {
    if (encoder_open_) return true;

    const AVCodec* enc = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!enc) return false;

    impl_->enc_ctx.reset(avcodec_alloc_context3(enc));
    if (!impl_->enc_ctx) return false;

    const int out_sr   = (cfg_.audio_sample_rate > 0) ? cfg_.audio_sample_rate : input_sample_rate;
    const int out_ch   = (cfg_.audio_channels    > 0) ? cfg_.audio_channels    : input_channels;
    const int out_brate = static_cast<int>(cfg_.audio_bitrate_bps);

    AVCodecContext* ec = impl_->enc_ctx.get();
    ec->sample_rate = out_sr;
    ec->bit_rate    = out_brate;
    ec->sample_fmt  = AV_SAMPLE_FMT_FLTP;            // FFmpeg native AAC encoder native format
    av_channel_layout_default(&ec->ch_layout, out_ch);
    ec->time_base   = AVRational{1, out_sr};

    if (avcodec_open2(ec, enc, nullptr) < 0) {
        impl_->enc_ctx.reset();
        return false;
    }

    // Set up resampler: input AVFrame.sample_fmt/sample_rate/channels →
    // FLTP/out_sr/out_ch.
    SwrContext* swr_raw = nullptr;
    AVChannelLayout in_layout;
    av_channel_layout_default(&in_layout, input_channels);
    const int rc = swr_alloc_set_opts2(
        &swr_raw,
        &ec->ch_layout, AV_SAMPLE_FMT_FLTP, out_sr,
        &in_layout,     impl_->dec_ctx->sample_fmt, input_sample_rate,
        0, nullptr);
    av_channel_layout_uninit(&in_layout);
    if (rc < 0 || !swr_raw) {
        impl_->enc_ctx.reset();
        return false;
    }
    impl_->swr.reset(swr_raw);
    if (swr_init(impl_->swr.get()) < 0) {
        impl_->enc_ctx.reset();
        impl_->swr.reset();
        return false;
    }

    impl_->fifo.reset(av_audio_fifo_alloc(AV_SAMPLE_FMT_FLTP, out_ch, ec->frame_size > 0 ? ec->frame_size : 1024));
    if (!impl_->fifo) {
        impl_->enc_ctx.reset();
        impl_->swr.reset();
        return false;
    }

    impl_->input_sample_rate = input_sample_rate;
    impl_->input_channels    = input_channels;
    impl_->out_sample_rate   = out_sr;
    impl_->out_channels      = out_ch;
    impl_->out_frame_size    = ec->frame_size > 0 ? ec->frame_size : 1024;
    impl_->adts_sr_idx       = aacSampleRateIndex(out_sr);
    impl_->adts_ch_cfg       = static_cast<std::uint8_t>(std::clamp(out_ch, 1, 7));
    encoder_open_ = true;
    return true;
}

void AudioTranscoder::feed(std::span<const std::uint8_t> es,
                           std::optional<std::int64_t>   pts_90khz,
                           const OutSink&                sink) {
    if (!decoder_open_) return;
    ++frames_in_;

    if (impl_->first_pts_90k == AV_NOPTS_VALUE && pts_90khz.has_value()) {
        impl_->first_pts_90k = *pts_90khz;
    }

    AVPacket* pkt = impl_->in_pkt.get();
    av_packet_unref(pkt);
    pkt->data = const_cast<std::uint8_t*>(es.data());
    pkt->size = static_cast<int>(es.size());
    pkt->pts  = pts_90khz.value_or(AV_NOPTS_VALUE);
    pkt->dts  = pts_90khz.value_or(AV_NOPTS_VALUE);
    pkt->time_base = kTbTs90k;

    const int rc = avcodec_send_packet(impl_->dec_ctx.get(), pkt);
    pkt->data = nullptr;
    pkt->size = 0;
    if (rc < 0 && rc != AVERROR(EAGAIN)) {
        ++decode_errors_;
        return;
    }
    drainDecoder(sink);
    encodeFromFifo(sink, /*flush=*/false);
}

void AudioTranscoder::drainDecoder(const OutSink& sink) {
    (void)sink;
    while (true) {
        const int rc = avcodec_receive_frame(impl_->dec_ctx.get(), impl_->dec_frame.get());
        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) return;
        if (rc < 0) {
            ++decode_errors_;
            return;
        }
        AVFrame* f = impl_->dec_frame.get();

        if (!encoder_open_) {
            const int in_ch = f->ch_layout.nb_channels > 0 ? f->ch_layout.nb_channels
                                                           : impl_->dec_ctx->ch_layout.nb_channels;
            if (!openEncoderForInput(f->sample_rate, in_ch)) {
                ++encode_errors_;
                av_frame_unref(f);
                return;
            }
        }

        // Resample → FLTP/out_sr/out_ch and stuff into the FIFO.
        const int max_out = static_cast<int>(av_rescale_rnd(
            swr_get_delay(impl_->swr.get(), f->sample_rate) + f->nb_samples,
            impl_->out_sample_rate, f->sample_rate, AV_ROUND_UP));

        std::uint8_t** out_buf = nullptr;
        if (av_samples_alloc_array_and_samples(&out_buf, nullptr,
                                               impl_->out_channels, max_out,
                                               AV_SAMPLE_FMT_FLTP, 0) < 0) {
            ++resample_errors_;
            av_frame_unref(f);
            continue;
        }
        const int got = swr_convert(impl_->swr.get(), out_buf, max_out,
                                    const_cast<const std::uint8_t**>(f->extended_data),
                                    f->nb_samples);
        if (got < 0) {
            av_freep(&out_buf[0]);
            av_freep(&out_buf);
            ++resample_errors_;
            av_frame_unref(f);
            continue;
        }
        if (got > 0) {
            av_audio_fifo_write(impl_->fifo.get(),
                                reinterpret_cast<void**>(out_buf), got);
        }
        av_freep(&out_buf[0]);
        av_freep(&out_buf);
        av_frame_unref(f);
    }
}

void AudioTranscoder::encodeFromFifo(const OutSink& sink, bool flush) {
    if (!encoder_open_) return;
    const int frame_size = impl_->out_frame_size;
    AVFrame* enc_in = impl_->enc_frame.get();

    while (av_audio_fifo_size(impl_->fifo.get()) >= frame_size ||
           (flush && av_audio_fifo_size(impl_->fifo.get()) > 0)) {

        const int n = std::min<int>(frame_size, av_audio_fifo_size(impl_->fifo.get()));

        av_frame_unref(enc_in);
        enc_in->nb_samples = n;
        enc_in->format     = AV_SAMPLE_FMT_FLTP;
        av_channel_layout_copy(&enc_in->ch_layout, &impl_->enc_ctx->ch_layout);
        enc_in->sample_rate = impl_->out_sample_rate;
        if (av_frame_get_buffer(enc_in, 0) < 0) {
            ++encode_errors_;
            av_audio_fifo_drain(impl_->fifo.get(), n);
            continue;
        }
        av_audio_fifo_read(impl_->fifo.get(),
                           reinterpret_cast<void**>(enc_in->data), n);

        // Encoder frame.pts in encoder time-base (1 / out_sample_rate). We
        // recompute output PTS in 90 kHz from a sample-counter anchored at
        // the first input PTS we ever saw.
        enc_in->pts = impl_->pts_samples_emitted;
        impl_->pts_samples_emitted += n;

        const int rc = avcodec_send_frame(impl_->enc_ctx.get(), enc_in);
        if (rc < 0 && rc != AVERROR(EAGAIN)) {
            ++encode_errors_;
            continue;
        }
        while (true) {
            const int er = avcodec_receive_packet(impl_->enc_ctx.get(), impl_->out_pkt.get());
            if (er == AVERROR(EAGAIN) || er == AVERROR_EOF) break;
            if (er < 0) {
                ++encode_errors_;
                break;
            }
            AVPacket* p = impl_->out_pkt.get();
            const std::int64_t pts_samples = (p->pts != AV_NOPTS_VALUE) ? p->pts : 0;
            const std::int64_t base = (impl_->first_pts_90k != AV_NOPTS_VALUE)
                                          ? impl_->first_pts_90k : 0;
            const std::int64_t pts_90k = base + av_rescale(
                pts_samples, kTbTs90k.den, impl_->out_sample_rate);
            emitEncodedPacket(p->data, p->size, pts_90k, sink);
            av_packet_unref(p);
        }
        if (flush && av_audio_fifo_size(impl_->fifo.get()) == 0) break;
    }

    if (flush) {
        avcodec_send_frame(impl_->enc_ctx.get(), nullptr);
        while (true) {
            const int er = avcodec_receive_packet(impl_->enc_ctx.get(), impl_->out_pkt.get());
            if (er == AVERROR(EAGAIN) || er == AVERROR_EOF) break;
            if (er < 0) { ++encode_errors_; break; }
            AVPacket* p = impl_->out_pkt.get();
            const std::int64_t pts_samples = (p->pts != AV_NOPTS_VALUE) ? p->pts : 0;
            const std::int64_t base = (impl_->first_pts_90k != AV_NOPTS_VALUE)
                                          ? impl_->first_pts_90k : 0;
            const std::int64_t pts_90k = base + av_rescale(
                pts_samples, kTbTs90k.den, impl_->out_sample_rate);
            emitEncodedPacket(p->data, p->size, pts_90k, sink);
            av_packet_unref(p);
        }
    }
}

void AudioTranscoder::emitSilence(std::int64_t pts_90khz, const OutSink& sink) {
    if (!encoder_open_) return;

    // Anchor the 90 kHz timeline on the very first emitSilence too — covers
    // the (rare) case where audio loss happens before any decoded input has
    // ever produced an output PTS.
    if (impl_->first_pts_90k == AV_NOPTS_VALUE) {
        impl_->first_pts_90k = pts_90khz;
    }

    AVCodecContext* ec = impl_->enc_ctx.get();
    const int frame_size = impl_->out_frame_size;

    AVFrame* fr = av_frame_alloc();
    if (!fr) {
        ++encode_errors_;
        return;
    }
    fr->nb_samples  = frame_size;
    fr->format      = AV_SAMPLE_FMT_FLTP;
    fr->sample_rate = impl_->out_sample_rate;
    av_channel_layout_copy(&fr->ch_layout, &ec->ch_layout);
    if (av_frame_get_buffer(fr, 0) < 0) {
        av_frame_free(&fr);
        ++encode_errors_;
        return;
    }
    for (int ch = 0; ch < impl_->out_channels; ++ch) {
        std::memset(fr->data[ch], 0, sizeof(float) * static_cast<std::size_t>(frame_size));
    }

    // Sample-counter PTS that round-trips through the encoder back to the
    // requested 90 kHz value via the same rescale used by the regular path.
    fr->pts = av_rescale(pts_90khz - impl_->first_pts_90k,
                         impl_->out_sample_rate, kTbTs90k.den);

    const int rc = avcodec_send_frame(ec, fr);
    av_frame_free(&fr);
    if (rc < 0 && rc != AVERROR(EAGAIN)) {
        ++encode_errors_;
        return;
    }
    ++silence_frames_out_;

    while (true) {
        const int er = avcodec_receive_packet(ec, impl_->out_pkt.get());
        if (er == AVERROR(EAGAIN) || er == AVERROR_EOF) break;
        if (er < 0) { ++encode_errors_; break; }
        AVPacket* p = impl_->out_pkt.get();
        const std::int64_t pts_samples = (p->pts != AV_NOPTS_VALUE) ? p->pts : 0;
        const std::int64_t pts_90k = impl_->first_pts_90k + av_rescale(
            pts_samples, kTbTs90k.den, impl_->out_sample_rate);
        emitEncodedPacket(p->data, p->size, pts_90k, sink);
        av_packet_unref(p);
    }
}

void AudioTranscoder::emitEncodedPacket(const std::uint8_t* data, int size,
                                        std::int64_t pts_90k, const OutSink& sink) {
    AudioOutFrame out;
    out.es.resize(static_cast<std::size_t>(size + 7));
    writeAdtsHeader(out.es.data(),
                    impl_->adts_profile_minus_one,
                    impl_->adts_sr_idx,
                    impl_->adts_ch_cfg,
                    static_cast<std::size_t>(size));
    std::memcpy(out.es.data() + 7, data, static_cast<std::size_t>(size));
    out.pts_90khz = pts_90k;
    ++frames_out_;
    if (sink) sink(std::move(out));
}

void AudioTranscoder::flush(const OutSink& sink) {
    if (decoder_open_) {
        avcodec_send_packet(impl_->dec_ctx.get(), nullptr);
        drainDecoder(sink);
    }
    if (encoder_open_) {
        encodeFromFifo(sink, /*flush=*/true);
    }
}

}  // namespace liveqx::gateway::transcode
