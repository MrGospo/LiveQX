// Unit tests for VideoTranscoder.
//
// We inline-encode a handful of synthetic YUV420P frames with libx264 to
// produce real H.264 Annex-B ES bytes, feed them through VideoTranscoder,
// and verify the output is real H.264 (decodable, has at least one keyframe).

#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <utility>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
}

#include "gateway/GatewayCfg.h"
#include "gateway/transcode/VideoTranscoder.h"

namespace tx = liveqx::gateway::transcode;
namespace gw = liveqx::gateway;

namespace {

struct H264Sample {
    std::vector<std::uint8_t> es;
    std::int64_t              pts_90khz;
    std::int64_t              dts_90khz;
};

// Encode `frame_count` solid-gradient YUV420P frames at (w,h)@fps and return
// each output AVPacket as one H264Sample. PTS/DTS are in 90 kHz; B-frames are
// disabled so PTS == DTS.
std::vector<H264Sample> generateH264Samples(int width, int height, int frame_count, int fps) {
    const AVCodec* enc = avcodec_find_encoder(AV_CODEC_ID_H264);
    EXPECT_NE(enc, nullptr);
    AVCodecContext* ctx = avcodec_alloc_context3(enc);
    EXPECT_NE(ctx, nullptr);

    ctx->width        = width;
    ctx->height       = height;
    ctx->pix_fmt      = AV_PIX_FMT_YUV420P;
    ctx->bit_rate     = 200'000;
    ctx->time_base    = {1, 90000};
    ctx->framerate    = AVRational{fps, 1};
    ctx->gop_size     = fps;
    ctx->max_b_frames = 0;

    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "preset", "ultrafast", 0);
    av_dict_set(&opts, "tune",   "zerolatency", 0);
    EXPECT_GE(avcodec_open2(ctx, enc, &opts), 0);
    av_dict_free(&opts);

    AVFrame* frame = av_frame_alloc();
    frame->format = AV_PIX_FMT_YUV420P;
    frame->width  = width;
    frame->height = height;
    EXPECT_GE(av_frame_get_buffer(frame, 32), 0);

    AVPacket* pkt = av_packet_alloc();
    std::vector<H264Sample> out;

    const std::int64_t step_90k = 90000 / fps;
    for (int i = 0; i < frame_count; ++i) {
        av_frame_make_writable(frame);
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                frame->data[0][y * frame->linesize[0] + x] =
                    static_cast<std::uint8_t>((i * 13 + x + y) & 0xFF);
            }
        }
        for (int y = 0; y < height / 2; ++y) {
            for (int x = 0; x < width / 2; ++x) {
                frame->data[1][y * frame->linesize[1] + x] = 128;
                frame->data[2][y * frame->linesize[2] + x] = 128;
            }
        }
        frame->pts = static_cast<std::int64_t>(i) * step_90k;
        EXPECT_GE(avcodec_send_frame(ctx, frame), 0);
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

// Decode a vector of H.264 ES samples back to AVFrames; returns the number of
// successfully decoded frames. Used to verify VideoTranscoder output is valid
// H.264 (an opaque-bytes assertion is too weak).
int countDecodableFrames(const std::vector<tx::VideoOutFrame>& samples,
                         int* out_first_w = nullptr, int* out_first_h = nullptr) {
    const AVCodec* dec = avcodec_find_decoder(AV_CODEC_ID_H264);
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
            if (out_first_w && *out_first_w == 0) *out_first_w = frm->width;
            if (out_first_h && *out_first_h == 0) *out_first_h = frm->height;
            ++ok;
            av_frame_unref(frm);
        }
    }
    pkt->data = nullptr;
    pkt->size = 0;
    avcodec_send_packet(ctx, nullptr);
    while (avcodec_receive_frame(ctx, frm) == 0) {
        if (out_first_w && *out_first_w == 0) *out_first_w = frm->width;
        if (out_first_h && *out_first_h == 0) *out_first_h = frm->height;
        ++ok;
        av_frame_unref(frm);
    }
    av_frame_free(&frm);
    av_packet_free(&pkt);
    avcodec_free_context(&ctx);
    return ok;
}

// Write a tiny RGB24 PPM image (P6 magic — trivial binary format) to a temp
// file. Bundled FFmpeg in this project enables only libx264 + AAC encoders,
// so we build the test asset by hand instead of going through avcodec.
std::string writeTestPpm(int w, int h) {
    std::string path = (std::filesystem::temp_directory_path() /
                        ("liveqx_logo_" + std::to_string(::getpid()) + "_" +
                         std::to_string(w) + "x" + std::to_string(h) + ".ppm")).string();
    std::ofstream os(path, std::ios::binary);
    os << "P6\n" << w << ' ' << h << "\n255\n";
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            os.put(static_cast<char>((x * 4) & 0xFF));
            os.put(static_cast<char>((y * 4) & 0xFF));
            os.put(static_cast<char>(128));
        }
    }
    os.close();
    return path;
}

gw::TranscodeCfg makeCfg(int w, int h, int fps) {
    gw::TranscodeCfg c;
    c.video_width        = w;
    c.video_height       = h;
    c.video_fps          = fps;
    c.video_bitrate_bps  = 500'000;
    c.video_max_b_frames = 0;
    c.video_preset       = "ultrafast";
    c.video_encoder_mode = "x264";
    return c;
}

}  // namespace

// ─── Init ────────────────────────────────────────────────────────────────────

TEST(VideoTranscoder, InitH264Succeeds) {
    tx::VideoTranscoder vt(makeCfg(64, 48, 25));
    ASSERT_TRUE(vt.init(AV_CODEC_ID_H264));
    EXPECT_TRUE(vt.decoderOpen());
    EXPECT_FALSE(vt.encoderOpen());  // encoder is lazy until first decoded frame
}

TEST(VideoTranscoder, InitWithUnsupportedCodecFails) {
    tx::VideoTranscoder vt(makeCfg(64, 48, 25));
    EXPECT_FALSE(vt.init(AV_CODEC_ID_NONE));
    EXPECT_FALSE(vt.decoderOpen());
}

// ─── Round-trip ──────────────────────────────────────────────────────────────

TEST(VideoTranscoder, ProducesDecodableOutputForH264Input) {
    constexpr int W = 64, H = 48, FPS = 25;
    auto samples = generateH264Samples(W, H, /*frames=*/10, FPS);
    ASSERT_GE(samples.size(), 5u);  // sanity: at least a handful of NALU groups

    tx::VideoTranscoder vt(makeCfg(W, H, FPS));
    ASSERT_TRUE(vt.init(AV_CODEC_ID_H264));

    std::vector<tx::VideoOutFrame> out;
    auto sink = [&](tx::VideoOutFrame&& f) { out.push_back(std::move(f)); };
    for (const auto& s : samples) {
        vt.feed(std::span<const std::uint8_t>(s.es.data(), s.es.size()),
                s.pts_90khz, s.dts_90khz, sink);
    }
    vt.flush(sink);

    ASSERT_GT(out.size(), 0u);
    EXPECT_GT(vt.framesOut(), 0u);
    EXPECT_EQ(vt.encodeErrors(), 0u);
    EXPECT_EQ(vt.decodeErrors(), 0u);

    int first_w = 0, first_h = 0;
    const int decoded = countDecodableFrames(out, &first_w, &first_h);
    EXPECT_GT(decoded, 0);
    EXPECT_EQ(first_w, W);
    EXPECT_EQ(first_h, H);
}

TEST(VideoTranscoder, ScalesToConfiguredOutputDimensions) {
    constexpr int IN_W = 64, IN_H = 48;
    constexpr int OUT_W = 160, OUT_H = 96;
    auto samples = generateH264Samples(IN_W, IN_H, 6, 25);

    tx::VideoTranscoder vt(makeCfg(OUT_W, OUT_H, 25));
    ASSERT_TRUE(vt.init(AV_CODEC_ID_H264));

    std::vector<tx::VideoOutFrame> out;
    auto sink = [&](tx::VideoOutFrame&& f) { out.push_back(std::move(f)); };
    for (const auto& s : samples) {
        vt.feed(std::span<const std::uint8_t>(s.es.data(), s.es.size()),
                s.pts_90khz, s.dts_90khz, sink);
    }
    vt.flush(sink);

    ASSERT_GT(out.size(), 0u);
    EXPECT_EQ(vt.outputWidth(),  OUT_W);
    EXPECT_EQ(vt.outputHeight(), OUT_H);

    int first_w = 0, first_h = 0;
    const int decoded = countDecodableFrames(out, &first_w, &first_h);
    EXPECT_GT(decoded, 0);
    EXPECT_EQ(first_w, OUT_W);
    EXPECT_EQ(first_h, OUT_H);
}

TEST(VideoTranscoder, OutputMarksFirstFrameAsKeyframe) {
    auto samples = generateH264Samples(64, 48, 5, 25);
    tx::VideoTranscoder vt(makeCfg(64, 48, 25));
    ASSERT_TRUE(vt.init(AV_CODEC_ID_H264));

    std::vector<tx::VideoOutFrame> out;
    auto sink = [&](tx::VideoOutFrame&& f) { out.push_back(std::move(f)); };
    for (const auto& s : samples) {
        vt.feed(std::span<const std::uint8_t>(s.es.data(), s.es.size()),
                s.pts_90khz, s.dts_90khz, sink);
    }
    vt.flush(sink);

    ASSERT_GT(out.size(), 0u);
    EXPECT_TRUE(out[0].is_keyframe);
}

// ─── Freeze-frame on input loss ─────────────────────────────────────────────

TEST(VideoTranscoder, EmitFreezeBeforeAnyFeedIsNoop) {
    tx::VideoTranscoder vt(makeCfg(64, 48, 25));
    ASSERT_TRUE(vt.init(AV_CODEC_ID_H264));
    EXPECT_FALSE(vt.hasLastFrame());

    int n = 0;
    vt.emitFreeze(0, /*force_keyframe=*/true, [&](tx::VideoOutFrame&&) { ++n; });
    EXPECT_EQ(n, 0);
    EXPECT_EQ(vt.freezeFramesOut(), 0u);
}

TEST(VideoTranscoder, EmitFreezeAfterFeedProducesAdditionalFrames) {
    constexpr int W = 64, H = 48, FPS = 25;
    auto samples = generateH264Samples(W, H, /*frames=*/8, FPS);

    tx::VideoTranscoder vt(makeCfg(W, H, FPS));
    ASSERT_TRUE(vt.init(AV_CODEC_ID_H264));

    std::vector<tx::VideoOutFrame> out;
    auto sink = [&](tx::VideoOutFrame&& f) { out.push_back(std::move(f)); };
    for (const auto& s : samples) {
        vt.feed(std::span<const std::uint8_t>(s.es.data(), s.es.size()),
                s.pts_90khz, s.dts_90khz, sink);
    }
    EXPECT_TRUE(vt.hasLastFrame());

    const std::size_t before_freeze = vt.framesOut();
    const std::int64_t step_90k = 90000 / FPS;
    // Anchor the freeze PTS one tick past the last fed sample.
    std::int64_t pts = samples.back().pts_90khz + step_90k;
    constexpr int FREEZE_N = 5;
    for (int i = 0; i < FREEZE_N; ++i) {
        vt.emitFreeze(pts, /*force_keyframe=*/(i == 0), sink);
        pts += step_90k;
    }
    vt.flush(sink);

    EXPECT_EQ(vt.freezeFramesOut(), static_cast<std::uint64_t>(FREEZE_N));
    EXPECT_GE(vt.framesOut(), before_freeze + FREEZE_N);
    EXPECT_EQ(vt.encodeErrors(), 0u);

    int decoded_w = 0, decoded_h = 0;
    const int decoded = countDecodableFrames(out, &decoded_w, &decoded_h);
    EXPECT_GE(decoded, FREEZE_N);
    EXPECT_EQ(decoded_w, W);
    EXPECT_EQ(decoded_h, H);
}

TEST(VideoTranscoder, FreezeForceKeyframeProducesIdrAtRequest) {
    constexpr int W = 64, H = 48, FPS = 25;
    auto samples = generateH264Samples(W, H, /*frames=*/3, FPS);

    // Wide GOP so the encoder would not naturally emit IDR during the freeze.
    auto cfg = makeCfg(W, H, FPS);
    tx::VideoTranscoder vt(cfg);
    ASSERT_TRUE(vt.init(AV_CODEC_ID_H264));

    std::vector<tx::VideoOutFrame> baseline;
    auto baseline_sink = [&](tx::VideoOutFrame&& f) { baseline.push_back(std::move(f)); };
    for (const auto& s : samples) {
        vt.feed(std::span<const std::uint8_t>(s.es.data(), s.es.size()),
                s.pts_90khz, s.dts_90khz, baseline_sink);
    }
    const std::size_t baseline_n = baseline.size();

    std::vector<tx::VideoOutFrame> freeze_out;
    auto freeze_sink = [&](tx::VideoOutFrame&& f) { freeze_out.push_back(std::move(f)); };
    const std::int64_t step_90k = 90000 / FPS;
    std::int64_t pts = samples.back().pts_90khz + step_90k;
    vt.emitFreeze(pts, /*force_keyframe=*/true, freeze_sink);
    vt.flush(freeze_sink);

    ASSERT_GT(freeze_out.size(), 0u);
    // Some buffered baseline frames may flush ahead of the freeze frame, but
    // at least one of the new outputs must be the freeze IDR.
    bool saw_keyframe = false;
    for (const auto& f : freeze_out) saw_keyframe = saw_keyframe || f.is_keyframe;
    EXPECT_TRUE(saw_keyframe);
    EXPECT_EQ(vt.freezeFramesOut(), 1u);
    (void)baseline_n;
}

TEST(VideoTranscoder, FreezePtsIsForwardedToOutput) {
    constexpr int W = 64, H = 48, FPS = 25;
    auto samples = generateH264Samples(W, H, /*frames=*/2, FPS);

    tx::VideoTranscoder vt(makeCfg(W, H, FPS));
    ASSERT_TRUE(vt.init(AV_CODEC_ID_H264));

    std::vector<tx::VideoOutFrame> out;
    auto sink = [&](tx::VideoOutFrame&& f) { out.push_back(std::move(f)); };
    for (const auto& s : samples) {
        vt.feed(std::span<const std::uint8_t>(s.es.data(), s.es.size()),
                s.pts_90khz, s.dts_90khz, sink);
    }
    const std::size_t before = out.size();

    constexpr std::int64_t kFreezePts = 1'000'000;
    vt.emitFreeze(kFreezePts, /*force_keyframe=*/true, sink);
    vt.flush(sink);

    bool found = false;
    for (std::size_t i = before; i < out.size(); ++i) {
        if (out[i].pts_90khz == kFreezePts) { found = true; break; }
    }
    EXPECT_TRUE(found);
}

// ─── Fallback logo image ────────────────────────────────────────────────────

TEST(VideoTranscoder, LoadFallbackBeforeEncoderOpenFails) {
    auto ppm_path = writeTestPpm(48, 32);
    tx::VideoTranscoder vt(makeCfg(64, 48, 25));
    ASSERT_TRUE(vt.init(AV_CODEC_ID_H264));
    EXPECT_FALSE(vt.encoderOpen());
    EXPECT_FALSE(vt.loadFallbackImage(ppm_path));
    EXPECT_FALSE(vt.hasFallback());
    std::filesystem::remove(ppm_path);
}

TEST(VideoTranscoder, LoadFallbackMissingFileFails) {
    constexpr int W = 64, H = 48, FPS = 25;
    auto samples = generateH264Samples(W, H, /*frames=*/2, FPS);
    tx::VideoTranscoder vt(makeCfg(W, H, FPS));
    ASSERT_TRUE(vt.init(AV_CODEC_ID_H264));

    std::vector<tx::VideoOutFrame> out;
    auto sink = [&](tx::VideoOutFrame&& f) { out.push_back(std::move(f)); };
    for (const auto& s : samples) {
        vt.feed(std::span<const std::uint8_t>(s.es.data(), s.es.size()),
                s.pts_90khz, s.dts_90khz, sink);
    }
    ASSERT_TRUE(vt.encoderOpen());

    EXPECT_FALSE(vt.loadFallbackImage("/nonexistent/path/does/not/exist.jpg"));
    EXPECT_FALSE(vt.hasFallback());
}

TEST(VideoTranscoder, LoadFallbackImageFromTempPpmSucceeds) {
    constexpr int W = 64, H = 48, FPS = 25;
    auto samples = generateH264Samples(W, H, /*frames=*/2, FPS);
    tx::VideoTranscoder vt(makeCfg(W, H, FPS));
    ASSERT_TRUE(vt.init(AV_CODEC_ID_H264));

    std::vector<tx::VideoOutFrame> out;
    auto sink = [&](tx::VideoOutFrame&& f) { out.push_back(std::move(f)); };
    for (const auto& s : samples) {
        vt.feed(std::span<const std::uint8_t>(s.es.data(), s.es.size()),
                s.pts_90khz, s.dts_90khz, sink);
    }
    ASSERT_TRUE(vt.encoderOpen());

    auto ppm_path = writeTestPpm(/*w=*/96, /*h=*/72);
    EXPECT_TRUE(vt.loadFallbackImage(ppm_path));
    EXPECT_TRUE(vt.hasFallback());
    std::filesystem::remove(ppm_path);
}

TEST(VideoTranscoder, EmitFallbackBeforeLoadIsNoop) {
    constexpr int W = 64, H = 48, FPS = 25;
    auto samples = generateH264Samples(W, H, /*frames=*/2, FPS);
    tx::VideoTranscoder vt(makeCfg(W, H, FPS));
    ASSERT_TRUE(vt.init(AV_CODEC_ID_H264));

    std::vector<tx::VideoOutFrame> seed;
    auto seed_sink = [&](tx::VideoOutFrame&& f) { seed.push_back(std::move(f)); };
    for (const auto& s : samples) {
        vt.feed(std::span<const std::uint8_t>(s.es.data(), s.es.size()),
                s.pts_90khz, s.dts_90khz, seed_sink);
    }

    int n = 0;
    vt.emitFallback(0, /*force_keyframe=*/true,
                    [&](tx::VideoOutFrame&&) { ++n; });
    EXPECT_EQ(n, 0);
    EXPECT_EQ(vt.fallbackFramesOut(), 0u);
}

TEST(VideoTranscoder, EmitFallbackAfterLoadProducesDecodableOutput) {
    constexpr int W = 64, H = 48, FPS = 25;
    auto samples = generateH264Samples(W, H, /*frames=*/2, FPS);
    tx::VideoTranscoder vt(makeCfg(W, H, FPS));
    ASSERT_TRUE(vt.init(AV_CODEC_ID_H264));

    std::vector<tx::VideoOutFrame> out;
    auto sink = [&](tx::VideoOutFrame&& f) { out.push_back(std::move(f)); };
    for (const auto& s : samples) {
        vt.feed(std::span<const std::uint8_t>(s.es.data(), s.es.size()),
                s.pts_90khz, s.dts_90khz, sink);
    }
    ASSERT_TRUE(vt.encoderOpen());

    auto ppm_path = writeTestPpm(96, 72);
    ASSERT_TRUE(vt.loadFallbackImage(ppm_path));

    constexpr int N = 4;
    const std::int64_t step_90k = 90000 / FPS;
    std::int64_t pts = samples.back().pts_90khz + 100 * step_90k;
    for (int i = 0; i < N; ++i) {
        vt.emitFallback(pts, /*force_keyframe=*/(i == 0), sink);
        pts += step_90k;
    }
    vt.flush(sink);

    EXPECT_EQ(vt.fallbackFramesOut(), static_cast<std::uint64_t>(N));
    EXPECT_EQ(vt.encodeErrors(), 0u);

    int decoded_w = 0, decoded_h = 0;
    const int decoded = countDecodableFrames(out, &decoded_w, &decoded_h);
    EXPECT_GE(decoded, N);
    EXPECT_EQ(decoded_w, W);
    EXPECT_EQ(decoded_h, H);

    std::filesystem::remove(ppm_path);
}

TEST(VideoTranscoder, FlushDrainsBufferedFrames) {
    auto samples = generateH264Samples(64, 48, 5, 25);
    tx::VideoTranscoder vt(makeCfg(64, 48, 25));
    ASSERT_TRUE(vt.init(AV_CODEC_ID_H264));

    std::vector<tx::VideoOutFrame> mid;
    auto sink_mid = [&](tx::VideoOutFrame&& f) { mid.push_back(std::move(f)); };
    for (const auto& s : samples) {
        vt.feed(std::span<const std::uint8_t>(s.es.data(), s.es.size()),
                s.pts_90khz, s.dts_90khz, sink_mid);
    }
    const std::size_t mid_count = mid.size();

    std::vector<tx::VideoOutFrame> tail;
    auto sink_tail = [&](tx::VideoOutFrame&& f) { tail.push_back(std::move(f)); };
    vt.flush(sink_tail);

    EXPECT_EQ(mid_count + tail.size(), vt.framesOut());
}
