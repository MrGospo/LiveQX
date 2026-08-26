#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "encoding/Encoder.h"
#include "encoding/EncoderFactory.h"
#include "encoding/IVideoEncoder.h"
#include "encoding/Mpeg2VideoEncoder.h"
#include "encoding/NvencVideoEncoder.h"
#include "encoding/QsvVideoEncoder.h"
#include "encoding/VaapiVideoEncoder.h"
#include "encoding/X264VideoEncoder.h"

// ─── helpers ─────────────────────────────────────────────────────────────────

static Frame solidFrame(int w, int h, uint8_t r, uint8_t g, uint8_t b) {
    Frame f;
    f.width  = w;
    f.height = h;
    const size_t sz = static_cast<size_t>(w) * h * 4;
    f.data = std::make_shared<uint8_t[]>(sz);
    for (size_t i = 0; i < sz / 4; ++i) {
        f.data[i * 4 + 0] = r;
        f.data[i * 4 + 1] = g;
        f.data[i * 4 + 2] = b;
        f.data[i * 4 + 3] = 255;
    }
    return f;
}

static AudioFrame silenceFrame(int sample_rate = 48000, int fps = 25) {
    const int n = sample_rate / fps;
    AudioFrame af;
    af.num_samples = n;
    af.sample_rate = sample_rate;
    af.channels    = 2;
    af.samples.assign(static_cast<size_t>(n) * 2, 0.0f);
    af.valid = true;
    return af;
}

static Encoder::Config testConfig() {
    Encoder::Config c;
    c.width         = 320;   // small → fast encode
    c.height        = 240;
    c.fps           = 25;
    c.video_bitrate = 500'000;
    c.audio_bitrate = 64'000;
    c.preset        = "ultrafast";
    return c;
}

// ─── tests ───────────────────────────────────────────────────────────────────

// Encoder must open successfully and produce at least some TS packets.
TEST(EncoderTest, OpenSucceeds) {
    Encoder enc(testConfig());
    ASSERT_TRUE(enc.open());
}

// 100 solid-color frames → callback fires at least once.
TEST(EncoderTest, Push100FramesReceivesPackets) {
    Encoder enc(testConfig());
    ASSERT_TRUE(enc.open());

    std::atomic<int> packet_count{0};
    std::atomic<size_t> total_bytes{0};

    enc.onPacket([&](const Packet& pkt) {
        ++packet_count;
        total_bytes += pkt.data.size();
    });

    const auto video = solidFrame(320, 240, 128, 64, 200);
    const auto audio = silenceFrame();

    for (int i = 0; i < 100; ++i)
        enc.pushFrame(video, audio);

    enc.close();

    EXPECT_GT(packet_count.load(), 0) << "no TS packets received";
    EXPECT_GT(total_bytes.load(),  0u) << "total output is zero bytes";
}

// Packets must carry non-zero data.
TEST(EncoderTest, PacketDataNonEmpty) {
    Encoder enc(testConfig());
    ASSERT_TRUE(enc.open());

    bool got_nonempty = false;
    enc.onPacket([&](const Packet& pkt) {
        if (!pkt.data.empty()) got_nonempty = true;
    });

    const auto video = solidFrame(320, 240, 200, 100, 50);
    const auto audio = silenceFrame();

    for (int i = 0; i < 50; ++i)
        enc.pushFrame(video, audio);

    enc.close();
    EXPECT_TRUE(got_nonempty);
}

// close() followed by open() + push must work (re-entrant).
TEST(EncoderTest, ReopenAfterClose) {
    Encoder enc(testConfig());
    ASSERT_TRUE(enc.open());

    for (int i = 0; i < 10; ++i)
        enc.pushFrame(solidFrame(320, 240, 0, 0, 0), silenceFrame());

    enc.close();

    int count = 0;
    enc.onPacket([&](const Packet&) { ++count; });
    ASSERT_TRUE(enc.open());

    for (int i = 0; i < 10; ++i)
        enc.pushFrame(solidFrame(320, 240, 255, 255, 255), silenceFrame());

    enc.close();
    EXPECT_GT(count, 0);
}

// pushFrame() before open() must not crash.
TEST(EncoderTest, PushBeforeOpenIsNoop) {
    Encoder enc(testConfig());
    EXPECT_NO_THROW(enc.pushFrame(solidFrame(320, 240, 0, 0, 0), silenceFrame()));
}

// Varying colours should still produce valid packets (encoder handles motion).
TEST(EncoderTest, AlternatingColourFrames) {
    Encoder enc(testConfig());
    ASSERT_TRUE(enc.open());

    int count = 0;
    enc.onPacket([&](const Packet&) { ++count; });

    const auto audio = silenceFrame();
    for (int i = 0; i < 100; ++i) {
        const uint8_t v = static_cast<uint8_t>(i * 2);
        enc.pushFrame(solidFrame(320, 240, v, 255 - v, v / 2), audio);
    }

    enc.close();
    EXPECT_GT(count, 0);
}

// max_b_frames>0 must open successfully (libx264 will silently re-clamp it
// if tune=zerolatency leaks through, so this also covers the conditional
// tune logic in Encoder::open).
TEST(EncoderTest, OpensWithBFrames) {
    auto cfg = testConfig();
    cfg.max_b_frames = 2;
    Encoder enc(cfg);
    ASSERT_TRUE(enc.open());

    int count = 0;
    enc.onPacket([&](const Packet&) { ++count; });

    const auto audio = silenceFrame();
    for (int i = 0; i < 60; ++i) {
        const uint8_t v = static_cast<uint8_t>(i * 4);
        enc.pushFrame(solidFrame(320, 240, v, 128, 255 - v), audio);
    }

    enc.close();
    EXPECT_GT(count, 0);
}

// ─── fix29 c14: EncoderFactory smoke tests ───────────────────────────────────

namespace ec = liveqx::encoding;

static ec::IVideoEncoder::Config factoryCfg() {
    ec::IVideoEncoder::Config c;
    c.width  = 320;
    c.height = 240;
    c.fps    = 25;
    c.bitrate = 500'000;
    c.preset  = "ultrafast";
    return c;
}

TEST(EncoderFactoryTest, NormalizeLowercases) {
    EXPECT_EQ(ec::normalizeEncoderMode("CPU"),   "cpu");
    EXPECT_EQ(ec::normalizeEncoderMode("NvEnc"), "nvenc");
    EXPECT_EQ(ec::normalizeEncoderMode(""),      "");
}

// "cpu" must always succeed on a stock build — x264 is the always-on baseline.
TEST(EncoderFactoryTest, CpuModeOpensX264) {
    auto enc = ec::pickVideoEncoder("cpu", "h264", factoryCfg(), nullptr);
    ASSERT_NE(enc, nullptr);
    EXPECT_STREQ(enc->name(), "x264");
}

// Same for the alias "x264".
TEST(EncoderFactoryTest, X264AliasOpensX264) {
    auto enc = ec::pickVideoEncoder("x264", "h264", factoryCfg(), nullptr);
    ASSERT_NE(enc, nullptr);
    EXPECT_STREQ(enc->name(), "x264");
}

// On a stock build none of the GPU backends are built in, so an explicit
// request for any of them must return nullptr (caller decides if that's
// fatal — Encoder treats it as a hard error, "auto" mode falls through).
TEST(EncoderFactoryTest, ExplicitGpuFailsWhenNotBuiltIn) {
    if (!ec::NvencVideoEncoder::isBuiltIn()) {
        EXPECT_EQ(ec::pickVideoEncoder("nvenc", "h264", factoryCfg(), nullptr), nullptr);
    }
    if (!ec::QsvVideoEncoder::isBuiltIn()) {
        EXPECT_EQ(ec::pickVideoEncoder("qsv",   "h264", factoryCfg(), nullptr), nullptr);
    }
    if (!ec::VaapiVideoEncoder::isBuiltIn()) {
        EXPECT_EQ(ec::pickVideoEncoder("vaapi", "h264", factoryCfg(), nullptr), nullptr);
    }
}

// "auto" must always reach a working backend — at minimum x264.
TEST(EncoderFactoryTest, AutoModeFallsBackToX264) {
    auto enc = ec::pickVideoEncoder("auto", "h264", factoryCfg(), nullptr);
    ASSERT_NE(enc, nullptr);
    // On stock build it lands on x264; on a GPU-enabled build it might
    // be something else, but it must still produce a usable encoder.
    EXPECT_NE(enc->name(), nullptr);
}

// Empty string is treated as "auto".
TEST(EncoderFactoryTest, EmptyModeFallsBackToX264) {
    auto enc = ec::pickVideoEncoder("", "h264", factoryCfg(), nullptr);
    ASSERT_NE(enc, nullptr);
}

// Unknown mode is treated as "auto" (with a warning logged) — we don't
// want a typo'd config to permanently kill a channel.
TEST(EncoderFactoryTest, UnknownModeFallsBackToAuto) {
    auto enc = ec::pickVideoEncoder("turbo-encoder-9000", "h264", factoryCfg(), nullptr);
    ASSERT_NE(enc, nullptr);
}

// MPEG-2 codec dispatches to the CPU MPEG-2 backend regardless of mode.
TEST(EncoderFactoryTest, Mpeg2CodecOpensMpeg2) {
    auto enc = ec::pickVideoEncoder("cpu", "mpeg2video", factoryCfg(), nullptr);
    ASSERT_NE(enc, nullptr);
    EXPECT_STREQ(enc->name(), "mpeg2video");
}

// GPU mode + MPEG-2 codec must still produce a working CPU MPEG-2 encoder
// (a warning is logged). Guards against a config that would otherwise
// silently fall through to h264 or nullptr.
TEST(EncoderFactoryTest, Mpeg2CodecIgnoresGpuMode) {
    auto enc = ec::pickVideoEncoder("nvenc", "mpeg2video", factoryCfg(), nullptr);
    ASSERT_NE(enc, nullptr);
    EXPECT_STREQ(enc->name(), "mpeg2video");
}

// Encoder::Config defaults must match the pre-fix29 behavior.
TEST(EncoderTest, DefaultConfigIsCpuMode) {
    Encoder::Config c;
    EXPECT_EQ(c.encoder_mode, "cpu");
    EXPECT_EQ(c.gpu_index,    0);
}

// Explicit cpu mode through the Encoder facade still works end-to-end.
TEST(EncoderTest, OpensWithExplicitCpuMode) {
    auto cfg = testConfig();
    cfg.encoder_mode = "cpu";
    Encoder enc(cfg);
    ASSERT_TRUE(enc.open());
    int count = 0;
    enc.onPacket([&](const Packet&) { ++count; });
    const auto audio = silenceFrame();
    for (int i = 0; i < 50; ++i)
        enc.pushFrame(solidFrame(320, 240, 100, 100, 100), audio);
    enc.close();
    EXPECT_GT(count, 0);
}

// "auto" mode end-to-end through Encoder facade — must produce packets
// (lands on x264 in a stock build).
TEST(EncoderTest, OpensInAutoMode) {
    auto cfg = testConfig();
    cfg.encoder_mode = "auto";
    Encoder enc(cfg);
    ASSERT_TRUE(enc.open());
    int count = 0;
    enc.onPacket([&](const Packet&) { ++count; });
    const auto audio = silenceFrame();
    for (int i = 0; i < 50; ++i)
        enc.pushFrame(solidFrame(320, 240, 50, 200, 75), audio);
    enc.close();
    EXPECT_GT(count, 0);
}

// Explicit GPU mode that isn't built in must surface as a clean open()
// failure (not a crash, not a silent fallback). Mirrors a real ops
// scenario where someone enables encoder_mode=nvenc on a stock binary.
TEST(EncoderTest, ExplicitGpuModeFailsCleanly) {
    if (ec::NvencVideoEncoder::isBuiltIn()) GTEST_SKIP() << "NVENC built in";
    auto cfg = testConfig();
    cfg.encoder_mode = "nvenc";
    Encoder enc(cfg);
    EXPECT_FALSE(enc.open());
}

// ─── MPEG-2 max_b_frames: caller value must reach AVCodecContext ─────────────
// Regression guard: earlier the Mpeg2VideoEncoder clamped max_b_frames to
// 0-or-2, silently overwriting anything the caller passed. Non-enterprise:
// the UI showed 0-16 while the backend picked its own number. The fix
// (Mpeg2VideoEncoder.cpp) now propagates cfg.max_b_frames verbatim, matching
// X264/VAAPI/QSV/NVENC. This test locks that in for 0, 2, and 5.
namespace {
int openMpeg2AndReadBFrames(int requested) {
    ec::IVideoEncoder::Config c;
    c.width         = 320;
    c.height        = 240;
    c.fps           = 25;
    c.bitrate       = 500'000;
    c.max_b_frames  = requested;
    ec::Mpeg2VideoEncoder enc(c, nullptr);
    if (!enc.open()) return -1;
    const int got = enc.effectiveMaxBFrames();
    enc.close();
    return got;
}
}  // namespace

TEST(Mpeg2EncoderTest, HonorsZeroBFramesRequest) {
    EXPECT_EQ(openMpeg2AndReadBFrames(0), 0);
}

TEST(Mpeg2EncoderTest, HonorsDvbTraditionalTwoBFrames) {
    EXPECT_EQ(openMpeg2AndReadBFrames(2), 2);
}

TEST(Mpeg2EncoderTest, HonorsHigherBFrameCountVerbatim) {
    // Value above the DVB-traditional 2 must reach the encoder unchanged.
    // Legacy set-top boxes may not like this, but that's a UI-level hint,
    // not a backend override.
    EXPECT_EQ(openMpeg2AndReadBFrames(5), 5);
}

// ─── gop_size wiring: caller value must reach AVCodecContext ────────────────
// gop_size=0 in the caller Config means "auto per backend": x264 uses fps,
// mpeg2video uses max(fps/2, 6). Positive caller values are honored verbatim
// by every backend (mirroring the max_b_frames contract — no silent clamp).
namespace {
int openX264AndReadGop(int requestedGop, int fps) {
    ec::IVideoEncoder::Config c;
    c.width    = 320;
    c.height   = 240;
    c.fps      = fps;
    c.bitrate  = 500'000;
    c.preset   = "ultrafast";
    c.gop_size = requestedGop;
    ec::X264VideoEncoder enc(c, nullptr);
    if (!enc.open()) return -1;
    const int got = enc.effectiveGopSize();
    enc.close();
    return got;
}

int openMpeg2AndReadGop(int requestedGop, int fps) {
    ec::IVideoEncoder::Config c;
    c.width    = 320;
    c.height   = 240;
    c.fps      = fps;
    c.bitrate  = 500'000;
    c.gop_size = requestedGop;
    ec::Mpeg2VideoEncoder enc(c, nullptr);
    if (!enc.open()) return -1;
    const int got = enc.effectiveGopSize();
    enc.close();
    return got;
}
}  // namespace

TEST(X264EncoderTest, GopSizeAutoFallsBackToFps) {
    // gop_size=0 must produce the historical x264 default: fps frames (~1 s).
    EXPECT_EQ(openX264AndReadGop(0, 25), 25);
    EXPECT_EQ(openX264AndReadGop(0, 50), 50);
}

TEST(X264EncoderTest, GopSizeHonoredVerbatim) {
    EXPECT_EQ(openX264AndReadGop(12, 25),  12);   // DVB-style short GOP
    EXPECT_EQ(openX264AndReadGop(60, 30),  60);   // 2-second GOP for HLS/OTT
    EXPECT_EQ(openX264AndReadGop(300, 25), 300);  // long-GOP archival
}

TEST(Mpeg2EncoderTest, GopSizeAutoFallsBackToHalfFps) {
    // gop_size=0 must reproduce the DVB set-top default: max(fps/2, 6).
    EXPECT_EQ(openMpeg2AndReadGop(0, 25), 12);   // 25/2 = 12
    EXPECT_EQ(openMpeg2AndReadGop(0, 50), 25);   // 50/2 = 25
    EXPECT_EQ(openMpeg2AndReadGop(0, 10), 6);    // clamp floor at 6
}

TEST(Mpeg2EncoderTest, GopSizeHonoredVerbatim) {
    EXPECT_EQ(openMpeg2AndReadGop(15, 25),  15);
    EXPECT_EQ(openMpeg2AndReadGop(48, 24),  48);
    EXPECT_EQ(openMpeg2AndReadGop(120, 25), 120);
}
