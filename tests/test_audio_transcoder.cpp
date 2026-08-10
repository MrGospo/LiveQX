// Unit tests for AudioTranscoder. We inline-encode synthetic PCM (a small
// burst of pseudo-tone samples) to AAC-ADTS via libavcodec's native AAC
// encoder, feed those ADTS frames through AudioTranscoder, and verify:
//  - construction / init success and unsupported-codec rejection
//  - output ADTS frames round-trip back through an AAC decoder
//  - sample-rate / channel adaptation via libswresample produces the
//    configured output dims
//  - ADTS header writer round-trips through aacSampleRateIndex

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

#include "gateway/GatewayCfg.h"
#include "gateway/transcode/AudioTranscoder.h"

namespace tx = liveqx::gateway::transcode;
namespace gw = liveqx::gateway;

namespace {

struct AacAdtsSample {
    std::vector<std::uint8_t> es;        // ADTS framing
    std::int64_t              pts_90khz; // monotonically rising, base=0
};

// Build a small chunk of ADTS-framed AAC by encoding a sine wave via the
// native FFmpeg AAC encoder and prepending an ADTS header per packet.
std::vector<AacAdtsSample> generateAacAdts(int sample_rate, int channels, int frame_count) {
    const AVCodec* enc = avcodec_find_encoder(AV_CODEC_ID_AAC);
    EXPECT_NE(enc, nullptr);
    AVCodecContext* ctx = avcodec_alloc_context3(enc);
    EXPECT_NE(ctx, nullptr);

    ctx->sample_rate = sample_rate;
    ctx->bit_rate    = 96'000;
    ctx->sample_fmt  = AV_SAMPLE_FMT_FLTP;
    av_channel_layout_default(&ctx->ch_layout, channels);
    ctx->time_base   = AVRational{1, sample_rate};
    EXPECT_GE(avcodec_open2(ctx, enc, nullptr), 0);

    const int nb = ctx->frame_size > 0 ? ctx->frame_size : 1024;

    AVFrame* frame = av_frame_alloc();
    frame->format     = AV_SAMPLE_FMT_FLTP;
    frame->sample_rate= sample_rate;
    av_channel_layout_copy(&frame->ch_layout, &ctx->ch_layout);
    frame->nb_samples = nb;
    EXPECT_GE(av_frame_get_buffer(frame, 0), 0);

    AVPacket* pkt = av_packet_alloc();

    std::vector<AacAdtsSample> out;
    std::int64_t sample_index = 0;
    const std::uint8_t sr_idx = tx::aacSampleRateIndex(sample_rate);
    EXPECT_NE(sr_idx, 0xFF);
    const std::uint8_t ch_cfg = static_cast<std::uint8_t>(channels);

    auto drain = [&](bool flushing) {
        while (true) {
            const int rc = avcodec_receive_packet(ctx, pkt);
            if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) return;
            if (rc < 0) { av_packet_unref(pkt); return; }
            std::vector<std::uint8_t> es(static_cast<std::size_t>(pkt->size + 7));
            tx::writeAdtsHeader(es.data(), 1, sr_idx, ch_cfg,
                                static_cast<std::size_t>(pkt->size));
            std::memcpy(es.data() + 7, pkt->data, static_cast<std::size_t>(pkt->size));
            const std::int64_t pts_samples = (pkt->pts != AV_NOPTS_VALUE) ? pkt->pts : 0;
            const std::int64_t pts_90k = av_rescale(pts_samples, 90000, sample_rate);
            out.push_back({std::move(es), pts_90k});
            av_packet_unref(pkt);
            (void)flushing;
        }
    };

    for (int f = 0; f < frame_count; ++f) {
        av_frame_make_writable(frame);
        for (int ch = 0; ch < channels; ++ch) {
            float* data = reinterpret_cast<float*>(frame->data[ch]);
            for (int i = 0; i < nb; ++i) {
                const float t = static_cast<float>((sample_index + i)) / static_cast<float>(sample_rate);
                data[i] = 0.25f * std::sin(2.0f * 3.14159265f * 440.0f * t);
            }
        }
        frame->pts = sample_index;
        sample_index += nb;
        EXPECT_GE(avcodec_send_frame(ctx, frame), 0);
        drain(false);
    }
    avcodec_send_frame(ctx, nullptr);
    drain(true);

    av_packet_free(&pkt);
    av_frame_free(&frame);
    avcodec_free_context(&ctx);
    return out;
}

// Decode ADTS-framed AAC back to PCM and count how many AVFrames we got.
int countDecodableFrames(const std::vector<tx::AudioOutFrame>& samples,
                         int* out_sr = nullptr, int* out_ch = nullptr) {
    const AVCodec* dec = avcodec_find_decoder(AV_CODEC_ID_AAC);
    EXPECT_NE(dec, nullptr);
    AVCodecContext* ctx = avcodec_alloc_context3(dec);
    EXPECT_NE(ctx, nullptr);
    EXPECT_GE(avcodec_open2(ctx, dec, nullptr), 0);

    AVPacket* pkt = av_packet_alloc();
    AVFrame*  frm = av_frame_alloc();
    int ok = 0;
    for (const auto& s : samples) {
        av_packet_unref(pkt);
        pkt->data = const_cast<std::uint8_t*>(s.es.data());
        pkt->size = static_cast<int>(s.es.size());
        if (avcodec_send_packet(ctx, pkt) < 0) continue;
        while (avcodec_receive_frame(ctx, frm) == 0) {
            if (out_sr && *out_sr == 0) *out_sr = frm->sample_rate;
            if (out_ch && *out_ch == 0) *out_ch = frm->ch_layout.nb_channels;
            ++ok;
            av_frame_unref(frm);
        }
    }
    pkt->data = nullptr; pkt->size = 0;
    avcodec_send_packet(ctx, nullptr);
    while (avcodec_receive_frame(ctx, frm) == 0) {
        if (out_sr && *out_sr == 0) *out_sr = frm->sample_rate;
        if (out_ch && *out_ch == 0) *out_ch = frm->ch_layout.nb_channels;
        ++ok;
        av_frame_unref(frm);
    }
    av_frame_free(&frm);
    av_packet_free(&pkt);
    avcodec_free_context(&ctx);
    return ok;
}

gw::TranscodeCfg makeCfg(int sr, int ch, int br = 96'000) {
    gw::TranscodeCfg c;
    c.audio_sample_rate = sr;
    c.audio_channels    = ch;
    c.audio_bitrate_bps = br;
    return c;
}

}  // namespace

// ─── ADTS header utility ─────────────────────────────────────────────────────

TEST(AudioTranscoderAdts, SampleRateIndexRoundTrip) {
    EXPECT_EQ(tx::aacSampleRateIndex(48000), 3);
    EXPECT_EQ(tx::aacSampleRateIndex(44100), 4);
    EXPECT_EQ(tx::aacSampleRateIndex(8000),  11);
    EXPECT_EQ(tx::aacSampleRateIndex(99999), 0xFF);
}

TEST(AudioTranscoderAdts, HeaderEncodesFrameLengthAndSr) {
    std::array<std::uint8_t, 7> hdr{};
    tx::writeAdtsHeader(hdr.data(), /*profile-1=*/1, /*sr_idx=*/3, /*ch=*/2, /*payload=*/100);
    EXPECT_EQ(hdr[0], 0xFF);                        // syncword
    EXPECT_EQ(hdr[1] & 0xF0, 0xF0);                 // syncword low
    EXPECT_EQ((hdr[1] >> 1) & 0x01, 0);             // ID = MPEG-4
    EXPECT_EQ(hdr[1] & 0x01, 1);                    // protection_absent
    EXPECT_EQ((hdr[2] >> 6) & 0x03, 1);             // profile-1 = LC
    EXPECT_EQ((hdr[2] >> 2) & 0x0F, 3);             // sr_idx = 48k
    // frame_length: top2|mid8|low3 = 100+7 = 107
    const std::uint16_t fl = static_cast<std::uint16_t>(((hdr[3] & 0x03) << 11) |
                                                        (hdr[4] << 3) |
                                                        ((hdr[5] >> 5) & 0x07));
    EXPECT_EQ(fl, 107u);
}

// ─── Init ────────────────────────────────────────────────────────────────────

TEST(AudioTranscoder, InitAacSucceeds) {
    tx::AudioTranscoder a(makeCfg(48000, 2));
    EXPECT_TRUE(a.init(AV_CODEC_ID_AAC));
    EXPECT_TRUE(a.decoderOpen());
    EXPECT_FALSE(a.encoderOpen());
}

TEST(AudioTranscoder, InitWithUnsupportedCodecFails) {
    tx::AudioTranscoder a(makeCfg(48000, 2));
    EXPECT_FALSE(a.init(AV_CODEC_ID_NONE));
}

// ─── Round-trip ──────────────────────────────────────────────────────────────

TEST(AudioTranscoder, ProducesDecodableOutputForAacInput) {
    auto samples = generateAacAdts(48000, 2, /*frames=*/10);
    ASSERT_GE(samples.size(), 5u);

    tx::AudioTranscoder a(makeCfg(48000, 2));
    ASSERT_TRUE(a.init(AV_CODEC_ID_AAC));

    std::vector<tx::AudioOutFrame> out;
    auto sink = [&](tx::AudioOutFrame&& f) { out.push_back(std::move(f)); };
    for (const auto& s : samples) {
        a.feed(std::span<const std::uint8_t>(s.es.data(), s.es.size()), s.pts_90khz, sink);
    }
    a.flush(sink);

    ASSERT_GT(out.size(), 0u);
    EXPECT_EQ(a.decodeErrors(), 0u);
    EXPECT_EQ(a.encodeErrors(), 0u);
    EXPECT_EQ(a.resampleErrors(), 0u);

    int sr = 0, ch = 0;
    const int decoded = countDecodableFrames(out, &sr, &ch);
    EXPECT_GT(decoded, 0);
    EXPECT_EQ(sr, 48000);
    EXPECT_EQ(ch, 2);
}

TEST(AudioTranscoder, ResamplesTo48kStereo) {
    auto samples = generateAacAdts(/*input_sr=*/44100, /*input_ch=*/2, 10);
    ASSERT_GE(samples.size(), 5u);

    tx::AudioTranscoder a(makeCfg(48000, 2));
    ASSERT_TRUE(a.init(AV_CODEC_ID_AAC));

    std::vector<tx::AudioOutFrame> out;
    auto sink = [&](tx::AudioOutFrame&& f) { out.push_back(std::move(f)); };
    for (const auto& s : samples) {
        a.feed(std::span<const std::uint8_t>(s.es.data(), s.es.size()), s.pts_90khz, sink);
    }
    a.flush(sink);

    ASSERT_GT(out.size(), 0u);
    EXPECT_EQ(a.outputSampleRate(), 48000);
    EXPECT_EQ(a.outputChannels(),   2);

    int sr = 0, ch = 0;
    const int decoded = countDecodableFrames(out, &sr, &ch);
    EXPECT_GT(decoded, 0);
    EXPECT_EQ(sr, 48000);
    EXPECT_EQ(ch, 2);
}

// ─── Silence on input loss ───────────────────────────────────────────────────

TEST(AudioTranscoder, EmitSilenceBeforeEncoderOpenIsNoop) {
    tx::AudioTranscoder a(makeCfg(48000, 2));
    ASSERT_TRUE(a.init(AV_CODEC_ID_AAC));
    EXPECT_FALSE(a.encoderOpen());

    int n = 0;
    a.emitSilence(0, [&](tx::AudioOutFrame&&) { ++n; });
    EXPECT_EQ(n, 0);
    EXPECT_EQ(a.silenceFramesOut(), 0u);
}

TEST(AudioTranscoder, EmitSilenceAfterFeedProducesAdditionalAdtsFrames) {
    auto samples = generateAacAdts(48000, 2, /*frames=*/4);
    tx::AudioTranscoder a(makeCfg(48000, 2));
    ASSERT_TRUE(a.init(AV_CODEC_ID_AAC));

    std::vector<tx::AudioOutFrame> out;
    auto sink = [&](tx::AudioOutFrame&& f) { out.push_back(std::move(f)); };
    for (const auto& s : samples) {
        a.feed(std::span<const std::uint8_t>(s.es.data(), s.es.size()), s.pts_90khz, sink);
    }
    ASSERT_TRUE(a.encoderOpen());

    // Emit a silence burst at PTS values just past the last fed sample.
    constexpr int kSilenceN = 6;
    const std::int64_t step_90k = av_rescale(1024, 90000, 48000);
    std::int64_t pts = samples.back().pts_90khz + step_90k;
    for (int i = 0; i < kSilenceN; ++i) {
        a.emitSilence(pts, sink);
        pts += step_90k;
    }
    a.flush(sink);

    EXPECT_EQ(a.silenceFramesOut(), static_cast<std::uint64_t>(kSilenceN));
    EXPECT_EQ(a.encodeErrors(), 0u);
    EXPECT_GT(out.size(), 0u);

    int sr = 0, ch = 0;
    const int decoded = countDecodableFrames(out, &sr, &ch);
    EXPECT_GT(decoded, 0);
    EXPECT_EQ(sr, 48000);
    EXPECT_EQ(ch, 2);
}

TEST(AudioTranscoder, SilenceFramePtsRoundTripsThroughEncoder) {
    auto samples = generateAacAdts(48000, 2, /*frames=*/2);
    tx::AudioTranscoder a(makeCfg(48000, 2));
    ASSERT_TRUE(a.init(AV_CODEC_ID_AAC));

    std::vector<tx::AudioOutFrame> out;
    auto sink = [&](tx::AudioOutFrame&& f) { out.push_back(std::move(f)); };
    for (const auto& s : samples) {
        a.feed(std::span<const std::uint8_t>(s.es.data(), s.es.size()), s.pts_90khz, sink);
    }

    // Pick a known silence PTS far in the future of the input baseline so we
    // can spot it unambiguously in the output.
    constexpr std::int64_t kSilencePts = 5'000'000;
    a.emitSilence(kSilencePts, sink);
    a.flush(sink);

    bool found = false;
    for (const auto& f : out) {
        // av_rescale can introduce ±1 sample of rounding when the silence
        // PTS isn't a multiple of (90000/sr); accept that tolerance.
        if (std::abs(f.pts_90khz - kSilencePts) <= 2) { found = true; break; }
    }
    EXPECT_TRUE(found);
}

TEST(AudioTranscoder, SilenceDecodesToNearZeroPcm) {
    // Seed the encoder with the bare minimum (one input frame) so it opens,
    // then dump a long silence burst and verify the *later* decoded output
    // averages near zero (early frames may carry encoder warm-up artefacts).
    auto samples = generateAacAdts(48000, 2, /*frames=*/1);
    tx::AudioTranscoder a(makeCfg(48000, 2));
    ASSERT_TRUE(a.init(AV_CODEC_ID_AAC));

    std::vector<tx::AudioOutFrame> out;
    auto sink = [&](tx::AudioOutFrame&& f) { out.push_back(std::move(f)); };
    for (const auto& s : samples) {
        a.feed(std::span<const std::uint8_t>(s.es.data(), s.es.size()), s.pts_90khz, sink);
    }
    ASSERT_TRUE(a.encoderOpen());

    constexpr int kSilenceN = 30;
    const std::int64_t step_90k = av_rescale(1024, 90000, 48000);
    std::int64_t pts = samples.back().pts_90khz + step_90k;
    for (int i = 0; i < kSilenceN; ++i) {
        a.emitSilence(pts, sink);
        pts += step_90k;
    }
    a.flush(sink);
    ASSERT_GT(out.size(), 5u);

    // Decode all output back to PCM and average the absolute value of the
    // last few decoded frames — they should be silence (~0).
    const AVCodec* dec = avcodec_find_decoder(AV_CODEC_ID_AAC);
    AVCodecContext* ctx = avcodec_alloc_context3(dec);
    ASSERT_GE(avcodec_open2(ctx, dec, nullptr), 0);
    AVPacket* pkt = av_packet_alloc();
    AVFrame*  frm = av_frame_alloc();

    std::vector<double> per_frame_abs;
    for (const auto& s : out) {
        av_packet_unref(pkt);
        pkt->data = const_cast<std::uint8_t*>(s.es.data());
        pkt->size = static_cast<int>(s.es.size());
        if (avcodec_send_packet(ctx, pkt) < 0) continue;
        while (avcodec_receive_frame(ctx, frm) == 0) {
            double sum = 0.0; std::size_t n = 0;
            for (int ch = 0; ch < frm->ch_layout.nb_channels; ++ch) {
                const float* d = reinterpret_cast<const float*>(frm->data[ch]);
                for (int i = 0; i < frm->nb_samples; ++i) {
                    sum += std::abs(d[i]); ++n;
                }
            }
            per_frame_abs.push_back(n > 0 ? sum / static_cast<double>(n) : 0.0);
            av_frame_unref(frm);
        }
    }
    pkt->data = nullptr; pkt->size = 0;
    avcodec_send_packet(ctx, nullptr);
    while (avcodec_receive_frame(ctx, frm) == 0) {
        double sum = 0.0; std::size_t n = 0;
        for (int ch = 0; ch < frm->ch_layout.nb_channels; ++ch) {
            const float* d = reinterpret_cast<const float*>(frm->data[ch]);
            for (int i = 0; i < frm->nb_samples; ++i) {
                sum += std::abs(d[i]); ++n;
            }
        }
        per_frame_abs.push_back(n > 0 ? sum / static_cast<double>(n) : 0.0);
        av_frame_unref(frm);
    }
    av_frame_free(&frm);
    av_packet_free(&pkt);
    avcodec_free_context(&ctx);

    ASSERT_GT(per_frame_abs.size(), 5u);
    // Tail: average abs of last 5 frames should be tiny (digital silence
    // through AAC is bit-imperfect but well below the seed tone amplitude).
    double tail = 0.0;
    for (std::size_t i = per_frame_abs.size() - 5; i < per_frame_abs.size(); ++i) {
        tail += per_frame_abs[i];
    }
    tail /= 5.0;
    EXPECT_LT(tail, 0.01);
}

TEST(AudioTranscoder, FlushDrainsBufferedFrames) {
    auto samples = generateAacAdts(48000, 2, 8);
    tx::AudioTranscoder a(makeCfg(48000, 2));
    ASSERT_TRUE(a.init(AV_CODEC_ID_AAC));

    std::vector<tx::AudioOutFrame> mid;
    auto sink_mid = [&](tx::AudioOutFrame&& f) { mid.push_back(std::move(f)); };
    for (const auto& s : samples) {
        a.feed(std::span<const std::uint8_t>(s.es.data(), s.es.size()), s.pts_90khz, sink_mid);
    }
    const std::size_t mid_count = mid.size();

    std::vector<tx::AudioOutFrame> tail;
    auto sink_tail = [&](tx::AudioOutFrame&& f) { tail.push_back(std::move(f)); };
    a.flush(sink_tail);

    EXPECT_EQ(mid_count + tail.size(), a.framesOut());
}
