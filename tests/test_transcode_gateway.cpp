#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <optional>
#include <vector>

#include "gateway/GatewayCfg.h"
#include "gateway/IGateway.h"
#include "gateway/TranscodeGateway.h"
#include "gateway/ts/PsiBuilder.h"
#include "gateway/ts/PsiParser.h"
#include "gateway/ts/TsPacket.h"

using namespace liveqx::gateway;
using ts::kTsPacketSize;

namespace {

// Build a synthetic SPTS input: PAT + PMT + a few ES packets on H.264 (0x1B)
// + AAC (0x0F) PIDs. Returns a UDP datagram (concatenated 188-byte packets).
std::vector<std::uint8_t> makeSptsDatagram(std::uint16_t v_pid = 0x200,
                                           std::uint16_t a_pid = 0x201) {
    ts::PatBuildInput pat;
    pat.transport_stream_id = 1;
    pat.programs.push_back({1, 0x100});
    auto pat_sec = ts::buildPatSection(pat);
    std::uint8_t cc_pat = 0;
    auto pat_pkts = ts::packetizeSection(0x0000, pat_sec, cc_pat);

    ts::PmtBuildInput pmt;
    pmt.program_number = 1;
    pmt.pcr_pid = v_pid;
    {
        ts::PmtStream s; s.stream_type = 0x1B; s.elementary_pid = v_pid;
        pmt.streams.push_back(s);
    }
    {
        ts::PmtStream s; s.stream_type = 0x0F; s.elementary_pid = a_pid;
        pmt.streams.push_back(s);
    }
    auto pmt_sec = ts::buildPmtSection(pmt);
    std::uint8_t cc_pmt = 0;
    auto pmt_pkts = ts::packetizeSection(0x100, pmt_sec, cc_pmt);

    auto makeEs = [](std::uint16_t pid, std::uint8_t cc) {
        std::array<std::uint8_t, kTsPacketSize> p{};
        p[0] = ts::kTsSyncByte;
        p[1] = static_cast<std::uint8_t>(((pid >> 8) & 0x1F));
        p[2] = static_cast<std::uint8_t>(pid & 0xFF);
        p[3] = static_cast<std::uint8_t>(0x10 | (cc & 0x0F));
        for (std::size_t i = 4; i < kTsPacketSize; ++i)
            p[i] = static_cast<std::uint8_t>(pid + i);
        return p;
    };

    std::vector<std::uint8_t> datagram;
    auto append = [&](std::span<const std::uint8_t, kTsPacketSize> p) {
        datagram.insert(datagram.end(), p.data(), p.data() + kTsPacketSize);
    };
    for (const auto& p : pat_pkts) append(std::span<const std::uint8_t, kTsPacketSize>(p.data(), kTsPacketSize));
    for (const auto& p : pmt_pkts) append(std::span<const std::uint8_t, kTsPacketSize>(p.data(), kTsPacketSize));
    auto v0 = makeEs(v_pid, 0); append(std::span<const std::uint8_t, kTsPacketSize>(v0.data(), kTsPacketSize));
    auto v1 = makeEs(v_pid, 1); append(std::span<const std::uint8_t, kTsPacketSize>(v1.data(), kTsPacketSize));
    auto v2 = makeEs(v_pid, 2); append(std::span<const std::uint8_t, kTsPacketSize>(v2.data(), kTsPacketSize));
    auto a0 = makeEs(a_pid, 0); append(std::span<const std::uint8_t, kTsPacketSize>(a0.data(), kTsPacketSize));
    auto a1 = makeEs(a_pid, 1); append(std::span<const std::uint8_t, kTsPacketSize>(a1.data(), kTsPacketSize));
    return datagram;
}

GatewayCfg makeTranscodeCfg() {
    GatewayCfg cfg;
    cfg.mode = GatewayMode::Transcode;
    cfg.input.address = "239.0.0.1"; cfg.input.port = 1234;
    OutputCfg o; o.id = "spts"; o.address = "239.1.0.1"; o.port = 5000;
    cfg.outputs.push_back(o);
    cfg.transcode.transport_stream_id = 42;
    cfg.transcode.service_id          = 7;
    cfg.transcode.service_name        = "Hello";
    cfg.transcode.provider_name       = "Provider";
    cfg.transcode.pmt_pid             = 0x300;
    cfg.transcode.video_pid           = 0x301;
    cfg.transcode.audio_pid           = 0x302;
    return cfg;
}

}  // namespace

// ─── Construction ────────────────────────────────────────────────────────────

TEST(TranscodeGatewayTest, ConstructsWithValidCfg) {
    TranscodeGateway g(1, "tc", makeTranscodeCfg());
    EXPECT_EQ(g.id(), 1);
    EXPECT_EQ(g.name(), "tc");
    EXPECT_FALSE(g.isRunning());
}

TEST(TranscodeGatewayTest, ConstructionThrowsOnZeroOutputs) {
    GatewayCfg cfg = makeTranscodeCfg();
    cfg.outputs.clear();
    EXPECT_THROW(TranscodeGateway(1, "tc", cfg), std::invalid_argument);
}

TEST(TranscodeGatewayTest, ConstructionThrowsOnMultipleOutputs) {
    GatewayCfg cfg = makeTranscodeCfg();
    OutputCfg extra; extra.id = "x"; extra.address = "239.2.0.1"; extra.port = 6000;
    cfg.outputs.push_back(extra);
    EXPECT_THROW(TranscodeGateway(1, "tc", cfg), std::invalid_argument);
}

// ─── PSI ingest ──────────────────────────────────────────────────────────────

TEST(TranscodeGatewayTest, PsiIngestDiscoversInputProgram) {
    TranscodeGateway g(1, "tc", makeTranscodeCfg());
    g.testFeedDatagram(makeSptsDatagram(0x200, 0x201));
    auto snap = g.inputPrograms();
    ASSERT_NE(snap, nullptr);
    ASSERT_EQ(snap->programs.size(), 1u);
    EXPECT_TRUE(snap->programs[0].discovered);
    EXPECT_EQ(snap->programs[0].service_id, 1u);
    EXPECT_EQ(snap->programs[0].streams.size(), 2u);
}

// ─── ES pass-through ────────────────────────────────────────────────────────

TEST(TranscodeGatewayTest, EsPacketsPassThroughWithRemappedPids) {
    TranscodeGateway g(1, "tc", makeTranscodeCfg());
    g.testFeedDatagram(makeSptsDatagram(0x200, 0x201));
    auto out = g.testDrainOutput();
    ASSERT_FALSE(out.empty());

    std::vector<std::uint8_t> all;
    for (const auto& dg : out) all.insert(all.end(), dg.begin(), dg.end());

    bool saw_video = false, saw_audio = false;
    bool saw_input_pid = false;
    for (std::size_t i = 0; i + kTsPacketSize <= all.size(); i += kTsPacketSize) {
        ts::TsPacketView v(std::span<const std::uint8_t, kTsPacketSize>(
            all.data() + i, kTsPacketSize));
        if (!v.isValidSync()) continue;
        if (v.pid() == 0x301) saw_video = true;
        if (v.pid() == 0x302) saw_audio = true;
        if (v.pid() == 0x200 || v.pid() == 0x201) saw_input_pid = true;
    }
    EXPECT_TRUE(saw_video);
    EXPECT_TRUE(saw_audio);
    EXPECT_FALSE(saw_input_pid);   // input PIDs are remapped, never leaked
}

TEST(TranscodeGatewayTest, CcMonotonicPerOutputPid) {
    TranscodeGateway g(1, "tc", makeTranscodeCfg());
    g.testFeedDatagram(makeSptsDatagram(0x200, 0x201));
    g.testFeedDatagram(makeSptsDatagram(0x200, 0x201));
    auto out = g.testDrainOutput();

    std::vector<std::uint8_t> ccs_v;
    for (const auto& dg : out) {
        for (std::size_t i = 0; i + kTsPacketSize <= dg.size(); i += kTsPacketSize) {
            ts::TsPacketView v(std::span<const std::uint8_t, kTsPacketSize>(
                dg.data() + i, kTsPacketSize));
            if (v.pid() == 0x301) ccs_v.push_back(v.continuityCounter());
        }
    }
    ASSERT_GE(ccs_v.size(), 4u);   // 3 + 3 video packets fed
    for (std::size_t i = 1; i < ccs_v.size(); ++i) {
        const std::uint8_t expected = static_cast<std::uint8_t>((ccs_v[i-1] + 1) & 0x0F);
        EXPECT_EQ(ccs_v[i], expected) << "video CC not monotonic at index " << i;
    }
}

// ─── Self-generated PSI emission ────────────────────────────────────────────

TEST(TranscodeGatewayTest, EmitsSelfGeneratedPatPmtSdt) {
    TranscodeGateway g(1, "tc", makeTranscodeCfg());
    // Drive one input cycle so PMT info is discovered (helps SDT/PMT carry
    // sensible stream_type values).
    g.testFeedDatagram(makeSptsDatagram(0x200, 0x201));
    g.testDrainOutput();      // drop ES output

    g.testEmitPsi();
    auto out = g.testDrainOutput();
    ASSERT_FALSE(out.empty());

    // Walk packets, find PAT/PMT/SDT by PID. For PSI tables we use
    // PsiSectionAssembler which correctly handles pointer_field + AF +
    // multi-packet sections, so the round-trip is robust.
    bool saw_pat = false, saw_pmt = false, saw_sdt = false;
    std::optional<ts::ParsedPat> seen_pat;
    std::optional<ts::ParsedPmt> seen_pmt;
    ts::PsiSectionAssembler asm_pat, asm_pmt, asm_sdt;

    for (const auto& dg : out) {
        for (std::size_t i = 0; i + kTsPacketSize <= dg.size(); i += kTsPacketSize) {
            ts::TsPacketView v(std::span<const std::uint8_t, kTsPacketSize>(
                dg.data() + i, kTsPacketSize));
            if (!v.isValidSync()) continue;
            const std::uint16_t pid = v.pid();
            if (pid == 0x0000) {
                saw_pat = true;
                asm_pat.feed(v, [&](std::uint16_t, std::span<const std::uint8_t> sec) {
                    if (auto p = ts::parsePat(sec)) seen_pat = std::move(*p);
                });
            } else if (pid == 0x300) {
                saw_pmt = true;
                asm_pmt.feed(v, [&](std::uint16_t, std::span<const std::uint8_t> sec) {
                    if (auto p = ts::parsePmt(sec)) seen_pmt = std::move(*p);
                });
            } else if (pid == 0x0011) {
                saw_sdt = true;
                // SDT presence alone is sufficient for the skeleton test.
                (void)asm_sdt;
            }
        }
    }
    EXPECT_TRUE(saw_pat);
    EXPECT_TRUE(saw_pmt);
    EXPECT_TRUE(saw_sdt);

    ASSERT_TRUE(seen_pat.has_value());
    EXPECT_EQ(seen_pat->transport_stream_id, 42);
    ASSERT_EQ(seen_pat->programs.size(), 1u);
    EXPECT_EQ(seen_pat->programs[0].program_number, 7);
    EXPECT_EQ(seen_pat->programs[0].pmt_pid, 0x300);

    ASSERT_TRUE(seen_pmt.has_value());
    EXPECT_EQ(seen_pmt->program_number, 7);
    ASSERT_EQ(seen_pmt->streams.size(), 2u);
    bool seen_v = false, seen_a = false;
    for (const auto& es : seen_pmt->streams) {
        if (es.elementary_pid == 0x301) seen_v = true;
        if (es.elementary_pid == 0x302) seen_a = true;
    }
    EXPECT_TRUE(seen_v);
    EXPECT_TRUE(seen_a);
}

// ─── Factory dispatch ──────────────────────────────────────────────────────

TEST(TranscodeGatewayTest, FactoryDispatchesTranscodeMode) {
    auto g = makeGateway(1, "tc", makeTranscodeCfg());
    ASSERT_NE(g, nullptr);
    auto* tc = dynamic_cast<TranscodeGateway*>(g.get());
    EXPECT_NE(tc, nullptr);
    EXPECT_EQ(g->id(), 1);
    EXPECT_FALSE(g->isRunning());
}

// ─── Status JSON ────────────────────────────────────────────────────────────

TEST(TranscodeGatewayTest, StatusJsonExposesModeAndConfig) {
    TranscodeGateway g(1, "tc", makeTranscodeCfg());
    g.testFeedDatagram(makeSptsDatagram());
    auto j = g.statusJson();
    EXPECT_EQ(j.at("mode"), "transcode");
    EXPECT_EQ(j.at("id"),   1);
    EXPECT_EQ(j.at("name"), "tc");
    EXPECT_FALSE(j.at("running").get<bool>());
    EXPECT_TRUE(j.contains("transcode"));
    EXPECT_EQ(j.at("input_video_pid"), 0x200);
    EXPECT_EQ(j.at("input_audio_pid"), 0x201);
}
