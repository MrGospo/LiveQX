// Integration tests for FfmpegTranscodePipeline.
//
// We feed real H.264 ES + ADTS-framed AAC ES into the pipeline and verify:
//   - Pipeline construction + lazy decoder open via onInputStreamTypes().
//   - Stream-type → AV_CODEC_ID mapping table.
//   - Happy path: PES in → decoded → re-encoded → PES out → TS-packetised.
//   - Loss FSM: video freeze fires after >2 frame periods of input idle;
//     audio silence fires after >4 frame periods.
//   - Stats JSON exposes V/A states + counters.

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <thread>
#include <utility>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
}

#include "gateway/GatewayCfg.h"
#include "gateway/transcode/AudioTranscoder.h"
#include "gateway/transcode/FfmpegTranscodePipeline.h"
#include "gateway/ts/PesAssembler.h"
#include "gateway/ts/TsPacket.h"

namespace tx  = liveqx::gateway::transcode;
namespace gw  = liveqx::gateway;
namespace ts  = liveqx::gateway::ts;

namespace {

using clock_t_ = std::chrono::steady_clock;

// ─── H.264 fixture ──────────────────────────────────────────────────────────

struct H264Sample {
    std::vector<std::uint8_t> es;
    std::int64_t              pts_90khz;
    std::int64_t              dts_90khz;
};

std::vector<H264Sample> generateH264(int w, int h, int n, int fps) {
    const AVCodec* enc = avcodec_find_encoder(AV_CODEC_ID_H264);
    AVCodecContext* ctx = avcodec_alloc_context3(enc);
    ctx->width = w; ctx->height = h;
    ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    ctx->bit_rate = 200'000;
    ctx->time_base = {1, 90000};
    ctx->framerate = AVRational{fps, 1};
    ctx->gop_size  = fps;
    ctx->max_b_frames = 0;
    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "preset", "ultrafast", 0);
    av_dict_set(&opts, "tune",   "zerolatency", 0);
    EXPECT_GE(avcodec_open2(ctx, enc, &opts), 0);
    av_dict_free(&opts);

    AVFrame* frame = av_frame_alloc();
    frame->format = AV_PIX_FMT_YUV420P;
    frame->width = w; frame->height = h;
    EXPECT_GE(av_frame_get_buffer(frame, 32), 0);

    AVPacket* pkt = av_packet_alloc();
    std::vector<H264Sample> out;
    const std::int64_t step = 90000 / fps;
    for (int i = 0; i < n; ++i) {
        av_frame_make_writable(frame);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                frame->data[0][y * frame->linesize[0] + x] =
                    static_cast<std::uint8_t>((i * 13 + x + y) & 0xFF);
        for (int y = 0; y < h / 2; ++y)
            for (int x = 0; x < w / 2; ++x) {
                frame->data[1][y * frame->linesize[1] + x] = 128;
                frame->data[2][y * frame->linesize[2] + x] = 128;
            }
        frame->pts = static_cast<std::int64_t>(i) * step;
        avcodec_send_frame(ctx, frame);
        while (avcodec_receive_packet(ctx, pkt) == 0) {
            out.push_back({std::vector<std::uint8_t>(pkt->data, pkt->data + pkt->size),
                           pkt->pts, pkt->dts});
            av_packet_unref(pkt);
        }
    }
    avcodec_send_frame(ctx, nullptr);
    while (avcodec_receive_packet(ctx, pkt) == 0) {
        out.push_back({std::vector<std::uint8_t>(pkt->data, pkt->data + pkt->size),
                       pkt->pts, pkt->dts});
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    av_frame_free(&frame);
    avcodec_free_context(&ctx);
    return out;
}

// ─── AAC ADTS fixture ───────────────────────────────────────────────────────

struct AacSample { std::vector<std::uint8_t> es; std::int64_t pts_90khz; };

std::vector<AacSample> generateAac(int sr, int ch, int n) {
    const AVCodec* enc = avcodec_find_encoder(AV_CODEC_ID_AAC);
    AVCodecContext* ctx = avcodec_alloc_context3(enc);
    ctx->sample_rate = sr;
    ctx->bit_rate = 96'000;
    ctx->sample_fmt = AV_SAMPLE_FMT_FLTP;
    av_channel_layout_default(&ctx->ch_layout, ch);
    ctx->time_base = AVRational{1, sr};
    EXPECT_GE(avcodec_open2(ctx, enc, nullptr), 0);

    const int nb = ctx->frame_size > 0 ? ctx->frame_size : 1024;
    AVFrame* frame = av_frame_alloc();
    frame->format = AV_SAMPLE_FMT_FLTP;
    frame->sample_rate = sr;
    av_channel_layout_copy(&frame->ch_layout, &ctx->ch_layout);
    frame->nb_samples = nb;
    EXPECT_GE(av_frame_get_buffer(frame, 0), 0);

    AVPacket* pkt = av_packet_alloc();
    std::vector<AacSample> out;
    std::int64_t si = 0;
    const std::uint8_t sr_idx = tx::aacSampleRateIndex(sr);
    const std::uint8_t ch_cfg = static_cast<std::uint8_t>(ch);

    auto drain = [&] {
        while (avcodec_receive_packet(ctx, pkt) == 0) {
            std::vector<std::uint8_t> es(static_cast<std::size_t>(pkt->size + 7));
            tx::writeAdtsHeader(es.data(), 1, sr_idx, ch_cfg,
                                static_cast<std::size_t>(pkt->size));
            std::memcpy(es.data() + 7, pkt->data, static_cast<std::size_t>(pkt->size));
            const std::int64_t pts_samples = (pkt->pts != AV_NOPTS_VALUE) ? pkt->pts : 0;
            out.push_back({std::move(es), av_rescale(pts_samples, 90000, sr)});
            av_packet_unref(pkt);
        }
    };

    for (int f = 0; f < n; ++f) {
        av_frame_make_writable(frame);
        for (int c = 0; c < ch; ++c) {
            float* data = reinterpret_cast<float*>(frame->data[c]);
            for (int i = 0; i < nb; ++i) {
                const float t = static_cast<float>(si + i) / static_cast<float>(sr);
                data[i] = 0.2f * std::sin(2.0f * 3.14159f * 440.0f * t);
            }
        }
        frame->pts = si;
        si += nb;
        avcodec_send_frame(ctx, frame);
        drain();
    }
    avcodec_send_frame(ctx, nullptr);
    drain();
    av_packet_free(&pkt);
    av_frame_free(&frame);
    avcodec_free_context(&ctx);
    return out;
}

gw::TranscodeCfg makeCfg() {
    gw::TranscodeCfg c;
    c.video_width = 64; c.video_height = 48; c.video_fps = 25;
    c.video_bitrate_bps = 200'000;
    c.video_max_b_frames = 0;
    c.video_preset = "ultrafast";
    c.video_encoder_mode = "x264";
    c.audio_sample_rate = 48000;
    c.audio_channels = 2;
    c.video_pid = 0x101; c.audio_pid = 0x102;
    c.loss_grace_ms = 250;        // small window so the loss test is quick
    return c;
}

// Helper: collect TS packets emitted via the pipeline's sink.
struct PacketCollector {
    std::vector<std::vector<std::uint8_t>> packets;
    void operator()(std::span<const std::uint8_t> p) {
        packets.emplace_back(p.begin(), p.end());
    }
};

}  // namespace

// ─── Mapping ────────────────────────────────────────────────────────────────

TEST(FfmpegTranscodePipeline, MapsStreamTypeToCodecId) {
    EXPECT_EQ(tx::FfmpegTranscodePipeline::mapVideoStreamType(0x1B), AV_CODEC_ID_H264);
    EXPECT_EQ(tx::FfmpegTranscodePipeline::mapVideoStreamType(0x24), AV_CODEC_ID_HEVC);
    EXPECT_EQ(tx::FfmpegTranscodePipeline::mapVideoStreamType(0x02), AV_CODEC_ID_MPEG2VIDEO);
    EXPECT_EQ(tx::FfmpegTranscodePipeline::mapVideoStreamType(0x00), AV_CODEC_ID_NONE);

    EXPECT_EQ(tx::FfmpegTranscodePipeline::mapAudioStreamType(0x0F), AV_CODEC_ID_AAC);
    EXPECT_EQ(tx::FfmpegTranscodePipeline::mapAudioStreamType(0x11), AV_CODEC_ID_AAC);
    EXPECT_EQ(tx::FfmpegTranscodePipeline::mapAudioStreamType(0x03), AV_CODEC_ID_MP3);
    EXPECT_EQ(tx::FfmpegTranscodePipeline::mapAudioStreamType(0x00), AV_CODEC_ID_NONE);
}

// ─── Construction + lazy decoder open ───────────────────────────────────────

TEST(FfmpegTranscodePipeline, ConstructsAndOpensDecodersOnStreamTypes) {
    tx::FfmpegTranscodePipeline p(makeCfg());
    EXPECT_FALSE(p.videoDecoderOpen());
    EXPECT_FALSE(p.audioDecoderOpen());
    p.onInputStreamTypes(/*v_st=*/0x1B, /*a_st=*/0x0F);
    EXPECT_TRUE(p.videoDecoderOpen());
    EXPECT_TRUE(p.audioDecoderOpen());
}

// ─── Happy path video ───────────────────────────────────────────────────────

TEST(FfmpegTranscodePipeline, VideoPesRoundTripProducesTsPackets) {
    auto cfg = makeCfg();
    tx::FfmpegTranscodePipeline p(cfg);
    PacketCollector pc;
    p.setOutputSink(std::ref(pc));
    p.onInputStreamTypes(0x1B, 0x0F);

    auto in = generateH264(64, 48, /*frames=*/8, 25);
    ASSERT_FALSE(in.empty());
    for (const auto& s : in) {
        ts::PesPacket pes;
        pes.stream_id = 0xE0;
        pes.pts_90khz = s.pts_90khz;
        pes.dts_90khz = s.dts_90khz;
        pes.es = s.es;
        p.feedVideoPes(std::move(pes));
    }
    p.flush();

    ASSERT_FALSE(pc.packets.empty());
    bool saw_video_pid = false;
    bool saw_pusi      = false;
    for (const auto& pkt : pc.packets) {
        ASSERT_EQ(pkt.size(), 188u);
        ts::TsPacketView v(std::span<const std::uint8_t, 188>(pkt.data(), 188));
        ASSERT_TRUE(v.isValidSync());
        if (v.pid() == cfg.video_pid) saw_video_pid = true;
        if (v.pusi())                  saw_pusi      = true;
    }
    EXPECT_TRUE(saw_video_pid);
    EXPECT_TRUE(saw_pusi);
}

// ─── Happy path audio ───────────────────────────────────────────────────────

TEST(FfmpegTranscodePipeline, AudioPesRoundTripProducesTsPackets) {
    auto cfg = makeCfg();
    tx::FfmpegTranscodePipeline p(cfg);
    PacketCollector pc;
    p.setOutputSink(std::ref(pc));
    p.onInputStreamTypes(0x1B, 0x0F);

    auto in = generateAac(48000, 2, /*frames=*/12);
    ASSERT_FALSE(in.empty());
    for (const auto& s : in) {
        ts::PesPacket pes;
        pes.stream_id = 0xC0;
        pes.pts_90khz = s.pts_90khz;
        pes.es        = s.es;
        p.feedAudioPes(std::move(pes));
    }
    p.flush();

    bool saw_audio_pid = false;
    for (const auto& pkt : pc.packets) {
        ASSERT_EQ(pkt.size(), 188u);
        ts::TsPacketView v(std::span<const std::uint8_t, 188>(pkt.data(), 188));
        if (v.pid() == cfg.audio_pid) { saw_audio_pid = true; break; }
    }
    EXPECT_TRUE(saw_audio_pid);
}

// ─── Loss FSM: video freeze ─────────────────────────────────────────────────

TEST(FfmpegTranscodePipeline, TickProducesFreezeFramesAfterIdle) {
    auto cfg = makeCfg();
    cfg.fallback_logo_path.clear();   // no fallback → stays in Freeze
    tx::FfmpegTranscodePipeline p(cfg);
    PacketCollector pc;
    p.setOutputSink(std::ref(pc));
    p.onInputStreamTypes(0x1B, 0x0F);

    // Prime with real frames so the encoder is open and a freeze cache exists.
    auto in = generateH264(64, 48, /*frames=*/4, 25);
    for (const auto& s : in) {
        ts::PesPacket pes;
        pes.stream_id = 0xE0;
        pes.pts_90khz = s.pts_90khz;
        pes.dts_90khz = s.dts_90khz;
        pes.es = s.es;
        p.feedVideoPes(std::move(pes));
    }
    const auto pre_freeze_packets = pc.packets.size();

    // Now simulate idle. Move the wall clock forward across multiple frame
    // periods (40ms at 25 fps → idle threshold = 80ms). Drive tick at the
    // configured frame rate.
    const auto t0 = clock_t_::now();
    for (int i = 0; i < 10; ++i) {
        const auto now = t0 + std::chrono::milliseconds(80 + i * 40);
        p.tick(now);
    }

    EXPECT_GT(pc.packets.size(), pre_freeze_packets);
    auto j = p.statsJson();
    EXPECT_GT(j.at("video").at("freeze_frames_out").get<std::uint64_t>(), 0u);
    // 80 ms idle is within loss_grace_ms (250 ms), so should be Freeze, not
    // Fallback.
    EXPECT_EQ(j.at("video").at("state").get<std::string>(), "freeze");
}

// ─── Stats JSON shape ───────────────────────────────────────────────────────

TEST(FfmpegTranscodePipeline, StatsJsonExposesPerStreamCounters) {
    tx::FfmpegTranscodePipeline p(makeCfg());
    auto j = p.statsJson();
    ASSERT_TRUE(j.contains("video"));
    ASSERT_TRUE(j.contains("audio"));
    EXPECT_EQ(j.at("video").at("state").get<std::string>(), "live");
    EXPECT_EQ(j.at("audio").at("state").get<std::string>(), "live");
    EXPECT_EQ(j.at("video").at("frames_in").get<std::uint64_t>(), 0u);
    EXPECT_EQ(j.at("audio").at("frames_in").get<std::uint64_t>(), 0u);
}
