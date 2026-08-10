#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <vector>

#include "gateway/DemuxGateway.h"
#include "gateway/GatewayCfg.h"
#include "gateway/ts/Crc32.h"
#include "gateway/ts/PsiBuilder.h"
#include "gateway/ts/PsiParser.h"
#include "gateway/ts/TsPacket.h"

using namespace liveqx::gateway;
using ts::kTsPacketSize;

namespace {

// Build a synthetic input MPTS containing PAT + PMT + a few ES packets.
// Returns a UDP-style datagram (concatenation of 188-byte packets).
std::vector<std::uint8_t> makeMptsDatagram() {
    // PAT: TS_id=1, single program {service_id=1, pmt_pid=0x100}
    ts::PatBuildInput pat;
    pat.transport_stream_id = 1;
    pat.programs.push_back({1, 0x100});
    auto pat_sec = ts::buildPatSection(pat);
    std::uint8_t cc_pat = 0;
    auto pat_pkts = ts::packetizeSection(0x0000, pat_sec, cc_pat);

    // PMT: program_number=1, pcr_pid=0x200, video=0x200(H.264), audio=0x201(AAC)
    ts::PmtBuildInput pmt;
    pmt.program_number = 1;
    pmt.pcr_pid = 0x200;
    {
        ts::PmtStream s; s.stream_type = 0x1B; s.elementary_pid = 0x200;
        pmt.streams.push_back(s);
    }
    {
        ts::PmtStream s; s.stream_type = 0x0F; s.elementary_pid = 0x201;
        pmt.streams.push_back(s);
    }
    auto pmt_sec = ts::buildPmtSection(pmt);
    std::uint8_t cc_pmt = 0;
    auto pmt_pkts = ts::packetizeSection(0x100, pmt_sec, cc_pmt);

    // Make one ES video packet (PID 0x200) and one ES audio packet (PID 0x201).
    auto makeEs = [](std::uint16_t pid, std::uint8_t cc) {
        std::array<std::uint8_t, kTsPacketSize> p{};
        p[0] = ts::kTsSyncByte;
        p[1] = static_cast<std::uint8_t>(((pid >> 8) & 0x1F));
        p[2] = static_cast<std::uint8_t>(pid & 0xFF);
        p[3] = static_cast<std::uint8_t>(0x10 | (cc & 0x0F));
        // Fill payload with deterministic bytes for assertions.
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
    auto v0 = makeEs(0x200, 0); append(std::span<const std::uint8_t, kTsPacketSize>(v0.data(), kTsPacketSize));
    auto v1 = makeEs(0x200, 1); append(std::span<const std::uint8_t, kTsPacketSize>(v1.data(), kTsPacketSize));
    auto a0 = makeEs(0x201, 0); append(std::span<const std::uint8_t, kTsPacketSize>(a0.data(), kTsPacketSize));
    return datagram;
}

GatewayCfg makeDemuxCfg() {
    GatewayCfg cfg;
    cfg.mode = GatewayMode::Demux;
    cfg.input.address = "239.0.0.1"; cfg.input.port = 1234;
    OutputCfg o; o.id = "out0"; o.address = "239.1.0.1"; o.port = 5000;
    cfg.outputs.push_back(o);
    DemuxRule r; r.service_id = 1; r.output_id = "out0";
    cfg.demux.routes.push_back(r);
    return cfg;
}

}  // namespace

// ─── Construction ────────────────────────────────────────────────────────────

TEST(DemuxGatewayTest, ConstructsWithValidCfg) {
    DemuxGateway g(1, "demux1", makeDemuxCfg());
    EXPECT_EQ(g.id(), 1);
    EXPECT_EQ(g.name(), "demux1");
    EXPECT_FALSE(g.isRunning());
}

TEST(DemuxGatewayTest, ConstructionThrowsOnUnknownOutputId) {
    GatewayCfg cfg = makeDemuxCfg();
    cfg.demux.routes[0].output_id = "ghost";
    EXPECT_THROW(DemuxGateway g(1, "x", cfg), std::invalid_argument);
}

// ─── PSI parsing & ProgramTable ──────────────────────────────────────────────

TEST(DemuxGatewayTest, FeedDatagramPopulatesProgramTable) {
    DemuxGateway g(1, "demux", makeDemuxCfg());
    auto datagram = makeMptsDatagram();
    g.testFeedDatagram(datagram);
    auto snap = g.programs();
    ASSERT_NE(snap, nullptr);
    ASSERT_EQ(snap->programs.size(), 1u);
    EXPECT_EQ(snap->programs[0].service_id, 1u);
    EXPECT_TRUE(snap->programs[0].discovered);
    ASSERT_EQ(snap->programs[0].streams.size(), 2u);
}

// ─── ES forwarding ──────────────────────────────────────────────────────────

TEST(DemuxGatewayTest, EsPacketsForwardedToBoundOutput) {
    DemuxGateway g(1, "demux", makeDemuxCfg());
    auto datagram = makeMptsDatagram();
    g.testFeedDatagram(datagram);
    auto out = g.testDrainOutput(0);
    ASSERT_FALSE(out.empty());
    // Concatenate all bytes and check we see ES PIDs (0x200, 0x201) flowing.
    std::vector<std::uint8_t> all;
    for (const auto& dg : out) all.insert(all.end(), dg.begin(), dg.end());
    bool saw_video = false, saw_audio = false;
    for (std::size_t i = 0; i + kTsPacketSize <= all.size(); i += kTsPacketSize) {
        ts::TsPacketView v(std::span<const std::uint8_t, kTsPacketSize>(all.data() + i, kTsPacketSize));
        if (!v.isValidSync()) continue;
        if (v.pid() == 0x200) saw_video = true;
        if (v.pid() == 0x201) saw_audio = true;
    }
    EXPECT_TRUE(saw_video);
    EXPECT_TRUE(saw_audio);
}

TEST(DemuxGatewayTest, UnroutedServiceDropsEsPackets) {
    GatewayCfg cfg = makeDemuxCfg();
    cfg.demux.routes[0].service_id = 99;        // route to a non-existent service
    DemuxGateway g(1, "demux", cfg);
    g.testFeedDatagram(makeMptsDatagram());
    auto out = g.testDrainOutput(0);
    // No ES packets should appear (only PSI, but we haven't called testEmitPsi).
    EXPECT_TRUE(out.empty());
}

TEST(DemuxGatewayTest, PidRemapRewritesEsPid) {
    GatewayCfg cfg = makeDemuxCfg();
    cfg.demux.routes[0].pid_remap.push_back({0x200, 0x101});  // video → 0x101
    cfg.demux.routes[0].pid_remap.push_back({0x201, 0x102});  // audio → 0x102
    DemuxGateway g(1, "demux", cfg);
    g.testFeedDatagram(makeMptsDatagram());
    auto out = g.testDrainOutput(0);
    ASSERT_FALSE(out.empty());
    bool saw_remapped_video = false, saw_remapped_audio = false;
    bool saw_original_pid = false;
    for (const auto& dg : out) {
        for (std::size_t i = 0; i + kTsPacketSize <= dg.size(); i += kTsPacketSize) {
            ts::TsPacketView v(std::span<const std::uint8_t, kTsPacketSize>(dg.data() + i, kTsPacketSize));
            if (v.pid() == 0x101) saw_remapped_video = true;
            if (v.pid() == 0x102) saw_remapped_audio = true;
            // PMT PID is 0x100 on output (default first-output assignment); ES
            // PIDs 0x200/0x201 should NOT appear after remap.
            if (v.pid() == 0x200 || v.pid() == 0x201) saw_original_pid = true;
        }
    }
    EXPECT_TRUE(saw_remapped_video);
    EXPECT_TRUE(saw_remapped_audio);
    EXPECT_FALSE(saw_original_pid);
}

TEST(DemuxGatewayTest, CcRewriteIsMonotonicPerOutputPid) {
    DemuxGateway g(1, "demux", makeDemuxCfg());
    g.testFeedDatagram(makeMptsDatagram());
    g.testFeedDatagram(makeMptsDatagram());
    auto out = g.testDrainOutput(0);
    // Walk all video packets and check CC strictly monotonic mod 16.
    std::vector<std::uint8_t> ccs;
    for (const auto& dg : out) {
        for (std::size_t i = 0; i + kTsPacketSize <= dg.size(); i += kTsPacketSize) {
            ts::TsPacketView v(std::span<const std::uint8_t, kTsPacketSize>(dg.data() + i, kTsPacketSize));
            if (v.pid() == 0x200) ccs.push_back(v.continuityCounter());
        }
    }
    ASSERT_GE(ccs.size(), 4u);
    for (std::size_t i = 1; i < ccs.size(); ++i) {
        EXPECT_EQ(ccs[i], static_cast<std::uint8_t>((ccs[i - 1] + 1) & 0x0F));
    }
}

// ─── PSI emission ────────────────────────────────────────────────────────────

TEST(DemuxGatewayTest, EmitPsiProducesPatPmtSdtPackets) {
    DemuxGateway g(1, "demux", makeDemuxCfg());
    g.testFeedDatagram(makeMptsDatagram());
    g.testEmitPsi();
    auto out = g.testDrainOutput(0);
    ASSERT_FALSE(out.empty());
    bool saw_pat = false, saw_pmt = false, saw_sdt = false;
    for (const auto& dg : out) {
        for (std::size_t i = 0; i + kTsPacketSize <= dg.size(); i += kTsPacketSize) {
            ts::TsPacketView v(std::span<const std::uint8_t, kTsPacketSize>(dg.data() + i, kTsPacketSize));
            if (v.pid() == 0x0000) saw_pat = true;
            if (v.pid() == 0x0100) saw_pmt = true;          // default output PMT
            if (v.pid() == 0x0011) saw_sdt = true;
        }
    }
    EXPECT_TRUE(saw_pat);
    EXPECT_TRUE(saw_pmt);
    EXPECT_TRUE(saw_sdt);
}

TEST(DemuxGatewayTest, EmittedPatIsParsable) {
    DemuxGateway g(1, "demux", makeDemuxCfg());
    g.testFeedDatagram(makeMptsDatagram());
    g.testEmitPsi();
    auto out = g.testDrainOutput(0);

    // Find the PAT packet, run it through PsiSectionAssembler, then parsePat.
    ts::PsiSectionAssembler asm_;
    bool parsed_ok = false;
    auto cb = [&](std::uint16_t pid, std::span<const std::uint8_t> sec) {
        if (pid == 0x0000) {
            auto pat = ts::parsePat(sec);
            if (pat && pat->programs.size() == 1 &&
                pat->programs[0].program_number == 1 &&
                pat->programs[0].pmt_pid == 0x100) parsed_ok = true;
        }
    };
    for (const auto& dg : out) {
        for (std::size_t i = 0; i + kTsPacketSize <= dg.size(); i += kTsPacketSize) {
            ts::TsPacketView v(std::span<const std::uint8_t, kTsPacketSize>(dg.data() + i, kTsPacketSize));
            if (v.pid() == 0x0000) {
                asm_.feed(v, cb);
            }
        }
    }
    EXPECT_TRUE(parsed_ok);
}

TEST(DemuxGatewayTest, EmittedPmtReflectsInputStreams) {
    DemuxGateway g(1, "demux", makeDemuxCfg());
    g.testFeedDatagram(makeMptsDatagram());
    g.testEmitPsi();
    auto out = g.testDrainOutput(0);
    ts::PsiSectionAssembler asm_;
    std::optional<ts::ParsedPmt> parsed;
    auto cb = [&](std::uint16_t pid, std::span<const std::uint8_t> sec) {
        if (pid == 0x100) parsed = ts::parsePmt(sec);
    };
    for (const auto& dg : out) {
        for (std::size_t i = 0; i + kTsPacketSize <= dg.size(); i += kTsPacketSize) {
            ts::TsPacketView v(std::span<const std::uint8_t, kTsPacketSize>(dg.data() + i, kTsPacketSize));
            if (v.pid() == 0x100) asm_.feed(v, cb);
        }
    }
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->program_number, 1u);
    ASSERT_EQ(parsed->streams.size(), 2u);
    EXPECT_EQ(parsed->streams[0].stream_type, 0x1Bu);
    EXPECT_EQ(parsed->streams[0].elementary_pid, 0x200u);
    EXPECT_EQ(parsed->streams[1].elementary_pid, 0x201u);
}

TEST(DemuxGatewayTest, EmittedPmtAfterPidRemapMatchesNewPids) {
    GatewayCfg cfg = makeDemuxCfg();
    cfg.demux.routes[0].pid_remap.push_back({0x200, 0x111});
    cfg.demux.routes[0].pid_remap.push_back({0x201, 0x112});
    DemuxGateway g(1, "demux", cfg);
    g.testFeedDatagram(makeMptsDatagram());
    g.testEmitPsi();
    auto out = g.testDrainOutput(0);
    ts::PsiSectionAssembler asm_;
    std::optional<ts::ParsedPmt> parsed;
    auto cb = [&](std::uint16_t pid, std::span<const std::uint8_t> sec) {
        if (pid == 0x100) parsed = ts::parsePmt(sec);
    };
    for (const auto& dg : out) {
        for (std::size_t i = 0; i + kTsPacketSize <= dg.size(); i += kTsPacketSize) {
            ts::TsPacketView v(std::span<const std::uint8_t, kTsPacketSize>(dg.data() + i, kTsPacketSize));
            if (v.pid() == 0x100) asm_.feed(v, cb);
        }
    }
    ASSERT_TRUE(parsed.has_value());
    ASSERT_EQ(parsed->streams.size(), 2u);
    EXPECT_EQ(parsed->streams[0].elementary_pid, 0x111u);
    EXPECT_EQ(parsed->streams[1].elementary_pid, 0x112u);
}

TEST(DemuxGatewayTest, EmitPsiDisabledSdtSuppressesSdt) {
    GatewayCfg cfg = makeDemuxCfg();
    cfg.demux.emit_sdt = false;
    DemuxGateway g(1, "demux", cfg);
    g.testFeedDatagram(makeMptsDatagram());
    g.testEmitPsi();
    auto out = g.testDrainOutput(0);
    bool saw_sdt = false;
    for (const auto& dg : out) {
        for (std::size_t i = 0; i + kTsPacketSize <= dg.size(); i += kTsPacketSize) {
            ts::TsPacketView v(std::span<const std::uint8_t, kTsPacketSize>(dg.data() + i, kTsPacketSize));
            if (v.pid() == 0x0011) saw_sdt = true;
        }
    }
    EXPECT_FALSE(saw_sdt);
}

// ─── PSI input is dropped, never forwarded ───────────────────────────────────

TEST(DemuxGatewayTest, InputPatPidIsNotForwarded) {
    DemuxGateway g(1, "demux", makeDemuxCfg());
    g.testFeedDatagram(makeMptsDatagram());
    auto out = g.testDrainOutput(0);
    // We did NOT call testEmitPsi(); only ES forwarding ran. There must be
    // no PAT (PID 0) packets — the demux gateway re-emits PSI itself.
    for (const auto& dg : out) {
        for (std::size_t i = 0; i + kTsPacketSize <= dg.size(); i += kTsPacketSize) {
            ts::TsPacketView v(std::span<const std::uint8_t, kTsPacketSize>(dg.data() + i, kTsPacketSize));
            EXPECT_NE(v.pid(), 0x0000u);
        }
    }
}

// ─── Multi-output demux (two services to two outputs) ────────────────────────

TEST(DemuxGatewayTest, TwoServicesRouteToTwoOutputs) {
    // Build an MPTS with two programs.
    ts::PatBuildInput pat;
    pat.transport_stream_id = 1;
    pat.programs.push_back({1, 0x100});
    pat.programs.push_back({2, 0x101});
    auto pat_sec = ts::buildPatSection(pat);
    std::uint8_t cc = 0;
    auto pat_pkts = ts::packetizeSection(0x0000, pat_sec, cc);

    auto buildPmt = [](std::uint16_t prog, std::uint16_t es_pid) {
        ts::PmtBuildInput pmt;
        pmt.program_number = prog; pmt.pcr_pid = es_pid;
        ts::PmtStream s; s.stream_type = 0x1B; s.elementary_pid = es_pid;
        pmt.streams.push_back(s);
        return ts::buildPmtSection(pmt);
    };
    std::uint8_t cc100 = 0, cc101 = 0;
    auto pmt1 = ts::packetizeSection(0x100, buildPmt(1, 0x200), cc100);
    auto pmt2 = ts::packetizeSection(0x101, buildPmt(2, 0x300), cc101);

    auto makeEs = [](std::uint16_t pid) {
        std::array<std::uint8_t, kTsPacketSize> p{};
        p[0] = ts::kTsSyncByte;
        p[1] = static_cast<std::uint8_t>(((pid >> 8) & 0x1F));
        p[2] = static_cast<std::uint8_t>(pid & 0xFF);
        p[3] = 0x10;
        return p;
    };

    std::vector<std::uint8_t> dg;
    auto append = [&](std::span<const std::uint8_t, kTsPacketSize> p) {
        dg.insert(dg.end(), p.data(), p.data() + kTsPacketSize);
    };
    for (auto& p : pat_pkts) append(std::span<const std::uint8_t, kTsPacketSize>(p.data(), kTsPacketSize));
    for (auto& p : pmt1) append(std::span<const std::uint8_t, kTsPacketSize>(p.data(), kTsPacketSize));
    for (auto& p : pmt2) append(std::span<const std::uint8_t, kTsPacketSize>(p.data(), kTsPacketSize));
    {
        auto p = makeEs(0x200);
        append(std::span<const std::uint8_t, kTsPacketSize>(p.data(), kTsPacketSize));
    }
    {
        auto p = makeEs(0x300);
        append(std::span<const std::uint8_t, kTsPacketSize>(p.data(), kTsPacketSize));
    }

    GatewayCfg cfg;
    cfg.mode = GatewayMode::Demux;
    cfg.input.address = "239.0.0.1"; cfg.input.port = 1234;
    cfg.outputs.push_back({"out0", "239.1.0.1", 5000, "", "", 16, 256});
    cfg.outputs.push_back({"out1", "239.1.0.2", 5001, "", "", 16, 256});
    DemuxRule r1; r1.service_id = 1; r1.output_id = "out0";
    DemuxRule r2; r2.service_id = 2; r2.output_id = "out1";
    cfg.demux.routes = {r1, r2};

    DemuxGateway g(1, "demux", cfg);
    g.testFeedDatagram(dg);

    auto out0 = g.testDrainOutput(0);
    auto out1 = g.testDrainOutput(1);

    auto sawPid = [&](const std::vector<std::vector<std::uint8_t>>& v, std::uint16_t pid) {
        for (const auto& d : v)
            for (std::size_t i = 0; i + kTsPacketSize <= d.size(); i += kTsPacketSize) {
                ts::TsPacketView vv(std::span<const std::uint8_t, kTsPacketSize>(d.data() + i, kTsPacketSize));
                if (vv.pid() == pid) return true;
            }
        return false;
    };

    EXPECT_TRUE(sawPid(out0, 0x200));
    EXPECT_FALSE(sawPid(out0, 0x300));
    EXPECT_TRUE(sawPid(out1, 0x300));
    EXPECT_FALSE(sawPid(out1, 0x200));
}

// ─── EIT pass-through (fix-A4) ───────────────────────────────────────────────

namespace {

// Build a UDP datagram carrying an EIT section for `service_id` packed into
// the standard 7-packet stride. table_id 0x4E = present/following actual.
std::vector<std::uint8_t> makeEitDatagram(std::uint16_t service_id,
                                          std::uint16_t event_id,
                                          std::uint8_t  table_id = 0x4E) {
    ts::EitBuildInput in;
    in.table_id            = table_id;
    in.service_id          = service_id;
    in.transport_stream_id = 1;
    in.original_network_id = 1;
    ts::EitEvent ev;
    ev.event_id       = event_id;
    ev.start_time_utc = 1778416496;       // 2026-05-10 12:34:56 UTC
    ev.duration_sec   = 3600;
    in.events.push_back(ev);
    auto sec = ts::buildEitSection(in);
    std::uint8_t cc = 0;
    auto pkts = ts::packetizeSection(ts::kPidEit, sec, cc);
    std::vector<std::uint8_t> out;
    for (const auto& p : pkts)
        out.insert(out.end(), p.data(), p.data() + kTsPacketSize);
    return out;
}

// Reassemble the first PSI section delivered on `pid` from a list of drained
// datagrams. Returns empty vector if not found.
std::vector<std::uint8_t> firstSectionOnPid(
    const std::vector<std::vector<std::uint8_t>>& drained,
    std::uint16_t pid) {
    ts::PsiSectionAssembler asm_;
    std::vector<std::uint8_t> sec;
    bool got = false;
    auto cb = [&](std::uint16_t p, std::span<const std::uint8_t> s) {
        if (got || p != pid) return;
        sec.assign(s.begin(), s.end());
        got = true;
    };
    for (const auto& dg : drained) {
        for (std::size_t i = 0; i + kTsPacketSize <= dg.size(); i += kTsPacketSize) {
            asm_.feed(ts::TsPacketView(std::span<const std::uint8_t, kTsPacketSize>(
                          dg.data() + i, kTsPacketSize)),
                      cb);
            if (got) return sec;
        }
    }
    return sec;
}

}  // namespace

TEST(DemuxGatewayTest, EitForRoutedServiceIsForwarded) {
    DemuxGateway g(1, "demux", makeDemuxCfg());
    g.testFeedDatagram(makeMptsDatagram());          // populate ProgramTable
    g.testFeedDatagram(makeEitDatagram(/*service_id=*/1, /*event_id=*/42));

    auto out = g.testDrainOutput(0);
    auto sec = firstSectionOnPid(out, ts::kPidEit);
    ASSERT_FALSE(sec.empty());
    auto parsed = ts::parseEit(sec);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->service_id, 1u);
    EXPECT_EQ(parsed->table_id, 0x4Eu);
    ASSERT_EQ(parsed->events.size(), 1u);
    EXPECT_EQ(parsed->events[0].event_id, 42u);
    EXPECT_EQ(parsed->events[0].duration_sec, 3600u);
}

TEST(DemuxGatewayTest, EitForUnroutedServiceIsDropped) {
    DemuxGateway g(1, "demux", makeDemuxCfg());
    g.testFeedDatagram(makeMptsDatagram());
    // service_id=99 is not routed to any output → must be dropped.
    g.testFeedDatagram(makeEitDatagram(/*service_id=*/99, /*event_id=*/7));
    auto out = g.testDrainOutput(0);
    auto sec = firstSectionOnPid(out, ts::kPidEit);
    EXPECT_TRUE(sec.empty());
}

TEST(DemuxGatewayTest, EitDroppedWhenPreserveEitFalse) {
    GatewayCfg cfg = makeDemuxCfg();
    cfg.demux.routes[0].preserve_eit = false;
    DemuxGateway g(1, "demux", cfg);
    g.testFeedDatagram(makeMptsDatagram());
    g.testFeedDatagram(makeEitDatagram(1, 42));
    auto out = g.testDrainOutput(0);
    auto sec = firstSectionOnPid(out, ts::kPidEit);
    EXPECT_TRUE(sec.empty());
}

TEST(DemuxGatewayTest, EitScheduleTableIdPreserved) {
    DemuxGateway g(1, "demux", makeDemuxCfg());
    g.testFeedDatagram(makeMptsDatagram());
    g.testFeedDatagram(makeEitDatagram(1, 7, /*table_id=*/0x50));
    auto out = g.testDrainOutput(0);
    auto sec = firstSectionOnPid(out, ts::kPidEit);
    ASSERT_FALSE(sec.empty());
    EXPECT_EQ(sec[0], 0x50u);
    auto parsed = ts::parseEit(sec);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_TRUE(parsed->actual);                    // 0x50 is actual schedule
    EXPECT_FALSE(parsed->present_following);
}

// ─── Stats ───────────────────────────────────────────────────────────────────

TEST(DemuxGatewayTest, StatsCountInputBytes) {
    DemuxGateway g(1, "demux", makeDemuxCfg());
    auto datagram = makeMptsDatagram();
    g.testFeedDatagram(datagram);
    g.testFeedDatagram(datagram);
    auto stats = g.getStats();
    EXPECT_EQ(stats.pkt_in, 2u);
    EXPECT_EQ(stats.bytes_in, 2u * datagram.size());
}

// ─── statusJson ──────────────────────────────────────────────────────────────

TEST(DemuxGatewayTest, StatusJsonExposesProgramTable) {
    DemuxGateway g(1, "demux", makeDemuxCfg());
    g.testFeedDatagram(makeMptsDatagram());
    auto j = g.statusJson();
    EXPECT_EQ(j["mode"], "demux");
    ASSERT_TRUE(j.contains("programs"));
    ASSERT_EQ(j["programs"].size(), 1u);
    EXPECT_EQ(j["programs"][0]["service_id"], 1);
    EXPECT_TRUE(j["programs"][0]["discovered"]);
}
