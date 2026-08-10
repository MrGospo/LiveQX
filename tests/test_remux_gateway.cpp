#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <string>
#include <vector>

#include "gateway/GatewayCfg.h"
#include "gateway/RemuxGateway.h"
#include "gateway/ts/PsiBuilder.h"
#include "gateway/ts/PsiParser.h"
#include "gateway/ts/TsPacket.h"

using namespace liveqx::gateway;
using ts::kTsPacketSize;

namespace {

// Build a synthetic SPTS UDP datagram: PAT + PMT + 2 ES packets, with
// caller-controlled service_id, PMT PID, video PID, audio PID.
std::vector<std::uint8_t> makeSpts(std::uint16_t service_id,
                                   std::uint16_t pmt_pid,
                                   std::uint16_t video_pid,
                                   std::uint16_t audio_pid,
                                   std::uint16_t ts_id = 1) {
    ts::PatBuildInput pat;
    pat.transport_stream_id = ts_id;
    pat.programs.push_back({service_id, pmt_pid});
    auto pat_sec = ts::buildPatSection(pat);
    std::uint8_t cc_pat = 0;
    auto pat_pkts = ts::packetizeSection(0x0000, pat_sec, cc_pat);

    ts::PmtBuildInput pmt;
    pmt.program_number = service_id;
    pmt.pcr_pid        = video_pid;
    {
        ts::PmtStream s; s.stream_type = 0x1B; s.elementary_pid = video_pid;
        pmt.streams.push_back(s);
    }
    {
        ts::PmtStream s; s.stream_type = 0x0F; s.elementary_pid = audio_pid;
        pmt.streams.push_back(s);
    }
    auto pmt_sec = ts::buildPmtSection(pmt);
    std::uint8_t cc_pmt = 0;
    auto pmt_pkts = ts::packetizeSection(pmt_pid, pmt_sec, cc_pmt);

    auto makeEs = [](std::uint16_t pid, std::uint8_t cc) {
        std::array<std::uint8_t, kTsPacketSize> p{};
        p[0] = ts::kTsSyncByte;
        p[1] = static_cast<std::uint8_t>((pid >> 8) & 0x1F);
        p[2] = static_cast<std::uint8_t>(pid & 0xFF);
        p[3] = static_cast<std::uint8_t>(0x10 | (cc & 0x0F));   // payload only
        for (std::size_t i = 4; i < kTsPacketSize; ++i)
            p[i] = static_cast<std::uint8_t>(pid + i);
        return p;
    };

    std::vector<std::uint8_t> dg;
    auto append = [&](const std::array<std::uint8_t, kTsPacketSize>& p) {
        dg.insert(dg.end(), p.data(), p.data() + kTsPacketSize);
    };
    for (const auto& p : pat_pkts) append(p);
    for (const auto& p : pmt_pkts) append(p);
    append(makeEs(video_pid, 0));
    append(makeEs(audio_pid, 0));
    return dg;
}

GatewayCfg makeRemuxCfg(std::uint16_t sid_a = 1, std::uint16_t sid_b = 2) {
    GatewayCfg cfg;
    cfg.mode = GatewayMode::Remux;
    cfg.input.address = "239.0.0.10"; cfg.input.port = 1234;
    InputCfg in1; in1.address = "239.0.0.11"; in1.port = 1235;
    cfg.extra_inputs.push_back(in1);
    OutputCfg o; o.id = "out0"; o.address = "239.1.0.1"; o.port = 5000;
    cfg.outputs.push_back(o);
    (void)sid_a; (void)sid_b;
    return cfg;
}

}  // namespace

// ─── Construction ────────────────────────────────────────────────────────────

TEST(RemuxGatewayTest, ConstructsWithValidCfg) {
    RemuxGateway g(1, "remux1", makeRemuxCfg());
    EXPECT_EQ(g.id(), 1);
    EXPECT_EQ(g.name(), "remux1");
    EXPECT_FALSE(g.isRunning());
}

TEST(RemuxGatewayTest, ConstructionThrowsOnSingleInput) {
    GatewayCfg cfg = makeRemuxCfg();
    cfg.extra_inputs.clear();   // ↓ to single input
    EXPECT_THROW(RemuxGateway g(1, "x", cfg), std::invalid_argument);
}

TEST(RemuxGatewayTest, ConstructionThrowsOnMultipleOutputs) {
    GatewayCfg cfg = makeRemuxCfg();
    OutputCfg extra; extra.id = "out1"; extra.address = "239.1.0.2"; extra.port = 5001;
    cfg.outputs.push_back(extra);
    EXPECT_THROW(RemuxGateway g(1, "x", cfg), std::invalid_argument);
}

// ─── PSI parsing ─────────────────────────────────────────────────────────────

TEST(RemuxGatewayTest, FeedDatagramPopulatesPerInputProgramTable) {
    RemuxGateway g(1, "remux", makeRemuxCfg());
    g.testFeedDatagram(0, makeSpts(1, 0x100, 0x200, 0x201));
    g.testFeedDatagram(1, makeSpts(2, 0x100, 0x200, 0x201, /*ts_id=*/2));

    auto snap0 = g.inputPrograms(0);
    auto snap1 = g.inputPrograms(1);
    ASSERT_NE(snap0, nullptr);
    ASSERT_NE(snap1, nullptr);
    ASSERT_EQ(snap0->programs.size(), 1u);
    ASSERT_EQ(snap1->programs.size(), 1u);
    EXPECT_EQ(snap0->programs.front().service_id, 1u);
    EXPECT_EQ(snap1->programs.front().service_id, 2u);
    EXPECT_TRUE(snap0->programs.front().discovered);
    EXPECT_TRUE(snap1->programs.front().discovered);
}

TEST(RemuxGatewayTest, OutputProgramTableMergesInputs) {
    RemuxGateway g(1, "remux", makeRemuxCfg());
    g.testFeedDatagram(0, makeSpts(1, 0x100, 0x200, 0x201));
    g.testFeedDatagram(1, makeSpts(2, 0x100, 0x200, 0x201, 2));

    auto out = g.outputPrograms();
    ASSERT_NE(out, nullptr);
    EXPECT_EQ(out->programs.size(), 2u);
    // service_ids preserved (1, 2 — no collision).
    bool saw_sid1 = false, saw_sid2 = false;
    for (const auto& p : out->programs) {
        if (p.service_id == 1u) saw_sid1 = true;
        if (p.service_id == 2u) saw_sid2 = true;
    }
    EXPECT_TRUE(saw_sid1);
    EXPECT_TRUE(saw_sid2);
}

// ─── PID conflict resolution ─────────────────────────────────────────────────

TEST(RemuxGatewayTest, PidConflictAutoResolved) {
    // Both inputs use video PID 0x200, audio PID 0x201, PMT 0x100.
    RemuxGateway g(1, "remux", makeRemuxCfg());
    g.testFeedDatagram(0, makeSpts(1, 0x100, 0x200, 0x201));
    g.testFeedDatagram(1, makeSpts(2, 0x100, 0x200, 0x201, 2));

    auto out = g.outputPrograms();
    ASSERT_NE(out, nullptr);
    ASSERT_EQ(out->programs.size(), 2u);

    // All ES PIDs across both programs must be unique on output.
    std::vector<std::uint16_t> all_pids;
    std::vector<std::uint16_t> all_pmt_pids;
    for (const auto& p : out->programs) {
        all_pmt_pids.push_back(p.pmt_pid);
        for (const auto& s : p.streams) all_pids.push_back(s.elementary_pid);
    }
    // PMT PIDs differ.
    ASSERT_EQ(all_pmt_pids.size(), 2u);
    EXPECT_NE(all_pmt_pids[0], all_pmt_pids[1]);
    // ES PIDs all distinct.
    std::sort(all_pids.begin(), all_pids.end());
    EXPECT_EQ(std::adjacent_find(all_pids.begin(), all_pids.end()),
              all_pids.end());
}

TEST(RemuxGatewayTest, PidConflictHonorsPinnedRemap) {
    GatewayCfg cfg = makeRemuxCfg();
    // Pin input 1's video → 0x301, audio → 0x302.
    cfg.remux.pid_remap.push_back({1, 0x200, 0x301});
    cfg.remux.pid_remap.push_back({1, 0x201, 0x302});
    RemuxGateway g(1, "remux", cfg);

    g.testFeedDatagram(0, makeSpts(1, 0x100, 0x200, 0x201));
    g.testFeedDatagram(1, makeSpts(2, 0x100, 0x200, 0x201, 2));

    auto out = g.outputPrograms();
    ASSERT_NE(out, nullptr);
    ASSERT_EQ(out->programs.size(), 2u);

    // Find program for service_id=2 (input 1) and verify pinned PIDs landed.
    const ts::ProgramInfo* p2 = nullptr;
    for (const auto& p : out->programs)
        if (p.service_id == 2u) { p2 = &p; break; }
    ASSERT_NE(p2, nullptr);
    bool saw_video = false, saw_audio = false;
    for (const auto& s : p2->streams) {
        if (s.elementary_pid == 0x301) saw_video = true;
        if (s.elementary_pid == 0x302) saw_audio = true;
    }
    EXPECT_TRUE(saw_video);
    EXPECT_TRUE(saw_audio);
}

// ─── service_id collision policy ─────────────────────────────────────────────

TEST(RemuxGatewayTest, ServiceIdCollisionAutoRenumber) {
    GatewayCfg cfg = makeRemuxCfg();
    cfg.remux.service_id_policy = RemuxCfg::ServiceIdPolicy::AutoRenumber;
    RemuxGateway g(1, "remux", cfg);

    // Both inputs use service_id=1.
    g.testFeedDatagram(0, makeSpts(1, 0x100, 0x200, 0x201));
    g.testFeedDatagram(1, makeSpts(1, 0x100, 0x200, 0x201, 2));

    auto out = g.outputPrograms();
    ASSERT_NE(out, nullptr);
    ASSERT_EQ(out->programs.size(), 2u);
    // First wins keeps 1; second is auto-renumbered to next free → 2.
    bool saw1 = false, saw2 = false;
    for (const auto& p : out->programs) {
        if (p.service_id == 1u) saw1 = true;
        if (p.service_id == 2u) saw2 = true;
    }
    EXPECT_TRUE(saw1);
    EXPECT_TRUE(saw2);
}

// ─── ES forwarding ───────────────────────────────────────────────────────────

TEST(RemuxGatewayTest, EsPacketsForwardedWithRemappedPids) {
    GatewayCfg cfg = makeRemuxCfg();
    cfg.remux.pid_remap.push_back({1, 0x200, 0x301});
    cfg.remux.pid_remap.push_back({1, 0x201, 0x302});
    RemuxGateway g(1, "remux", cfg);

    g.testFeedDatagram(0, makeSpts(1, 0x100, 0x200, 0x201));
    g.testFeedDatagram(1, makeSpts(2, 0x100, 0x200, 0x201, 2));

    auto out = g.testDrainOutput();
    ASSERT_FALSE(out.empty());

    bool saw_in0_video = false, saw_in1_video_remapped = false;
    bool saw_input_pmt_pid_on_wire = false;
    for (const auto& dg : out) {
        for (std::size_t i = 0; i + kTsPacketSize <= dg.size(); i += kTsPacketSize) {
            ts::TsPacketView v(std::span<const std::uint8_t, kTsPacketSize>(
                dg.data() + i, kTsPacketSize));
            if (!v.isValidSync()) continue;
            const auto pid = v.pid();
            if (pid == 0x200) saw_in0_video = true;       // input 0 identity
            if (pid == 0x301) saw_in1_video_remapped = true; // input 1 pinned
            if (pid == 0x100) saw_input_pmt_pid_on_wire = true; // src PMT PID — must NOT leak
            if (pid == 0x0000 || pid == 0x0011) {
                FAIL() << "source PSI leaked to output";
            }
        }
    }
    EXPECT_TRUE(saw_in0_video);
    EXPECT_TRUE(saw_in1_video_remapped);
    EXPECT_FALSE(saw_input_pmt_pid_on_wire);
}

TEST(RemuxGatewayTest, CcMonotonicPerOutputPid) {
    RemuxGateway g(1, "remux", makeRemuxCfg());
    g.testFeedDatagram(0, makeSpts(1, 0x100, 0x200, 0x201));
    g.testFeedDatagram(1, makeSpts(2, 0x100, 0x200, 0x201, 2));
    // Feed 17 more ES packets per input to exercise CC wrap (4-bit field).
    for (int n = 0; n < 17; ++n) {
        g.testFeedDatagram(0, makeSpts(1, 0x100, 0x200, 0x201));
    }
    auto out = g.testDrainOutput();
    ASSERT_FALSE(out.empty());

    // Track CC sequences per output PID; each must increment monotonically
    // (mod 16).
    std::unordered_map<std::uint16_t, int> last_cc;   // -1 means none yet
    for (const auto& dg : out) {
        for (std::size_t i = 0; i + kTsPacketSize <= dg.size(); i += kTsPacketSize) {
            ts::TsPacketView v(std::span<const std::uint8_t, kTsPacketSize>(
                dg.data() + i, kTsPacketSize));
            if (!v.isValidSync()) continue;
            const auto pid = v.pid();
            if (pid == 0x1FFF) continue;
            const int cc = v.continuityCounter();
            auto it = last_cc.find(pid);
            if (it == last_cc.end()) { last_cc[pid] = cc; continue; }
            const int expected = (it->second + 1) & 0x0F;
            EXPECT_EQ(cc, expected) << "PID " << pid;
            it->second = cc;
        }
    }
    // We must have seen at least one ES PID stream on output.
    EXPECT_FALSE(last_cc.empty());
}

// ─── PSI emission ────────────────────────────────────────────────────────────

namespace {

// Walk a drained datagram set and reassemble all PSI sections seen on the
// given PID. Returns the concatenation of every fully-payload-only TS packet
// after stripping the pointer_field. Only valid when the section fits in one
// payload (true for our small test PSI).
std::vector<std::uint8_t> grabPsiSection(
    const std::vector<std::vector<std::uint8_t>>& datagrams,
    std::uint16_t pid)
{
    for (const auto& dg : datagrams) {
        for (std::size_t i = 0; i + kTsPacketSize <= dg.size(); i += kTsPacketSize) {
            ts::TsPacketView v(std::span<const std::uint8_t, kTsPacketSize>(
                dg.data() + i, kTsPacketSize));
            if (!v.isValidSync()) continue;
            if (v.pid() != pid) continue;
            if (!v.pusi()) continue;
            const auto payload = v.payload();
            if (payload.empty()) continue;
            const std::uint8_t pointer = payload[0];
            const std::size_t off = 1u + pointer;
            if (off >= payload.size()) continue;
            // Section length is in bytes 1..2 of the section after the table_id;
            // section_length covers everything after the length field including CRC.
            const std::uint8_t  table_id = payload[off];
            const std::uint16_t sec_len = static_cast<std::uint16_t>(
                ((payload[off + 1] & 0x0F) << 8) | payload[off + 2]);
            const std::size_t total = 3u + sec_len;
            if (off + total > payload.size()) continue;
            (void)table_id;
            return std::vector<std::uint8_t>(
                payload.data() + off, payload.data() + off + total);
        }
    }
    return {};
}

}  // namespace

TEST(RemuxGatewayTest, EmitPsiPublishesPatPmtSdt) {
    GatewayCfg cfg = makeRemuxCfg();
    cfg.remux.transport_stream_id = 7;
    cfg.remux.original_network_id = 9;
    RemuxGateway g(1, "remux", cfg);

    // Seed both inputs so the synthesized program table is ready.
    g.testFeedDatagram(0, makeSpts(1, 0x100, 0x200, 0x201));
    g.testFeedDatagram(1, makeSpts(2, 0x100, 0x200, 0x201, 2));

    // Drain ES output first to isolate PSI output below.
    auto pre = g.testDrainOutput();
    (void)pre;

    g.testEmitPsi();
    auto out = g.testDrainOutput();
    ASSERT_FALSE(out.empty());

    // PAT: parses, transport_stream_id matches cfg, lists both programs.
    auto pat_sec = grabPsiSection(out, /*PID=*/0x0000);
    ASSERT_FALSE(pat_sec.empty());
    auto pat = ts::parsePat(pat_sec);
    ASSERT_TRUE(pat.has_value());
    EXPECT_EQ(pat->transport_stream_id, 7u);
    EXPECT_EQ(pat->programs.size(), 2u);

    // SDT: parses, both services present, names propagated when known.
    auto sdt_sec = grabPsiSection(out, /*PID=*/0x0011);
    ASSERT_FALSE(sdt_sec.empty());
    auto sdt = ts::parseSdt(sdt_sec);
    ASSERT_TRUE(sdt.has_value());
    EXPECT_EQ(sdt->transport_stream_id, 7u);
    EXPECT_EQ(sdt->original_network_id, 9u);
    EXPECT_EQ(sdt->services.size(), 2u);

    // For each PMT mentioned in the PAT, a PMT section is emitted on its PID
    // and parses, with our two ES streams (video + audio) listed.
    for (const auto& pe : pat->programs) {
        auto pmt_sec = grabPsiSection(out, pe.pmt_pid);
        ASSERT_FALSE(pmt_sec.empty()) << "no PMT on PID 0x" << std::hex << pe.pmt_pid;
        auto pmt = ts::parsePmt(pmt_sec);
        ASSERT_TRUE(pmt.has_value());
        EXPECT_EQ(pmt->program_number, pe.program_number);
        EXPECT_EQ(pmt->streams.size(), 2u);
    }
}

// ─── Bitrate stuffing ────────────────────────────────────────────────────────

TEST(RemuxGatewayTest, NoStuffingWhenTargetBitrateZero) {
    RemuxGateway g(1, "remux", makeRemuxCfg());
    g.testFeedDatagram(0, makeSpts(1, 0x100, 0x200, 0x201));
    (void)g.testDrainOutput();

    using namespace std::chrono;
    const auto t0 = steady_clock::now();
    g.testTopUpStuffing(t0);
    g.testTopUpStuffing(t0 + milliseconds(100));
    auto out = g.testDrainOutput();
    // No NULL packets should be generated.
    for (const auto& dg : out) {
        for (std::size_t i = 0; i + kTsPacketSize <= dg.size(); i += kTsPacketSize) {
            ts::TsPacketView v(std::span<const std::uint8_t, kTsPacketSize>(
                dg.data() + i, kTsPacketSize));
            EXPECT_NE(v.pid(), 0x1FFFu);
        }
    }
}

TEST(RemuxGatewayTest, StuffingFillsDeficitWhenTargetBitrateSet) {
    GatewayCfg cfg = makeRemuxCfg();
    cfg.remux.target_bitrate_bps = 4'000'000;     // 4 Mbps target
    RemuxGateway g(1, "remux", cfg);
    g.testFeedDatagram(0, makeSpts(1, 0x100, 0x200, 0x201));
    g.testFeedDatagram(1, makeSpts(2, 0x100, 0x200, 0x201, 2));

    using namespace std::chrono;
    const auto t0 = steady_clock::now();
    // Prime: first call seeds the window.
    g.testTopUpStuffing(t0);
    (void)g.testDrainOutput();

    // 100 ms of 4 Mbps == 50000 bytes ≈ 266 TS packets. The window has only
    // a handful of ES packets emitted between primes, so most must be NULLs.
    g.testTopUpStuffing(t0 + milliseconds(100));
    auto out = g.testDrainOutput();
    ASSERT_FALSE(out.empty());

    std::size_t null_count = 0;
    std::size_t total      = 0;
    for (const auto& dg : out) {
        for (std::size_t i = 0; i + kTsPacketSize <= dg.size(); i += kTsPacketSize) {
            ts::TsPacketView v(std::span<const std::uint8_t, kTsPacketSize>(
                dg.data() + i, kTsPacketSize));
            if (!v.isValidSync()) continue;
            ++total;
            if (v.pid() == 0x1FFFu) ++null_count;
        }
    }
    // Expected ≈ (4e6 * 0.1)/8/188 ≈ 266 packets. Allow ±10 packets slack.
    EXPECT_GE(null_count, 250u);
    EXPECT_LE(total, 280u);
}

TEST(RemuxGatewayTest, EmitPsiSdtSuppressedWhenDisabled) {
    GatewayCfg cfg = makeRemuxCfg();
    cfg.remux.emit_sdt = false;
    RemuxGateway g(1, "remux", cfg);

    g.testFeedDatagram(0, makeSpts(1, 0x100, 0x200, 0x201));
    g.testFeedDatagram(1, makeSpts(2, 0x100, 0x200, 0x201, 2));
    (void)g.testDrainOutput();

    g.testEmitPsi();
    auto out = g.testDrainOutput();
    auto sdt_sec = grabPsiSection(out, /*PID=*/0x0011);
    EXPECT_TRUE(sdt_sec.empty());
}

// ─── EIT forward (fix-A4) ────────────────────────────────────────────────────

namespace {

std::vector<std::uint8_t> makeRemuxEitDatagram(std::uint16_t service_id,
                                               std::uint16_t event_id,
                                               std::uint8_t  table_id = 0x4E) {
    ts::EitBuildInput in;
    in.table_id            = table_id;
    in.service_id          = service_id;
    in.transport_stream_id = 1;
    in.original_network_id = 1;
    ts::EitEvent ev;
    ev.event_id       = event_id;
    ev.start_time_utc = 1778416496;
    ev.duration_sec   = 1800;
    in.events.push_back(ev);
    auto sec = ts::buildEitSection(in);
    std::uint8_t cc = 0;
    auto pkts = ts::packetizeSection(ts::kPidEit, sec, cc);
    std::vector<std::uint8_t> out;
    for (const auto& p : pkts)
        out.insert(out.end(), p.data(), p.data() + kTsPacketSize);
    return out;
}

}  // namespace

TEST(RemuxGatewayTest, EitFromInputForwardsToOutputWithRemuxTsid) {
    GatewayCfg cfg = makeRemuxCfg();
    cfg.remux.transport_stream_id = 9;
    cfg.remux.original_network_id = 0xABCD;
    RemuxGateway g(1, "remux", cfg);
    // Populate per-input PAT so sid_remap is built (sid=1 from input 0).
    g.testFeedDatagram(0, makeSpts(1, 0x100, 0x200, 0x201));
    g.testFeedDatagram(1, makeSpts(2, 0x100, 0x200, 0x201, 2));
    // Send EIT from input 0 referencing service_id=1.
    g.testFeedDatagram(0, makeRemuxEitDatagram(1, 42));

    auto out = g.testDrainOutput();
    auto sec = grabPsiSection(out, ts::kPidEit);
    ASSERT_FALSE(sec.empty());
    auto parsed = ts::parseEit(sec);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->table_id, 0x4Eu);
    EXPECT_EQ(parsed->transport_stream_id, 9u);     // re-stamped from cfg
    EXPECT_EQ(parsed->original_network_id, 0xABCDu);
    EXPECT_EQ(parsed->service_id, 1u);              // sid_remap identity (no collision)
    ASSERT_EQ(parsed->events.size(), 1u);
    EXPECT_EQ(parsed->events[0].event_id, 42u);
}

TEST(RemuxGatewayTest, EitServiceIdRenumberAppliedToForward) {
    GatewayCfg cfg = makeRemuxCfg();
    RemuxGateway g(1, "remux", cfg);
    // Both inputs claim service_id=1 → input 1 will be renumbered to sid=2.
    g.testFeedDatagram(0, makeSpts(1, 0x100, 0x200, 0x201));
    g.testFeedDatagram(1, makeSpts(1, 0x100, 0x300, 0x301, 2));

    // EIT from input 1 carries the *original* service_id=1; the gateway must
    // rewrite it to the renumbered output sid (2) so receivers can match it
    // against the output PAT.
    g.testFeedDatagram(1, makeRemuxEitDatagram(1, 99));

    auto out = g.testDrainOutput();
    auto sec = grabPsiSection(out, ts::kPidEit);
    ASSERT_FALSE(sec.empty());
    auto parsed = ts::parseEit(sec);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->service_id, 2u);              // renumbered
    ASSERT_EQ(parsed->events.size(), 1u);
    EXPECT_EQ(parsed->events[0].event_id, 99u);
}

TEST(RemuxGatewayTest, EitForUnknownServiceIdIsDropped) {
    RemuxGateway g(1, "remux", makeRemuxCfg());
    g.testFeedDatagram(0, makeSpts(1, 0x100, 0x200, 0x201));
    g.testFeedDatagram(1, makeSpts(2, 0x100, 0x200, 0x201, 2));
    // service_id=42 is not in either input's PAT — drop.
    g.testFeedDatagram(0, makeRemuxEitDatagram(42, 7));
    auto out = g.testDrainOutput();
    auto sec = grabPsiSection(out, ts::kPidEit);
    EXPECT_TRUE(sec.empty());
}

// ─── Status JSON ─────────────────────────────────────────────────────────────

// ─── PCR restamping (fix40 A5) ───────────────────────────────────────────────

namespace {

// Build a single TS packet on `pid` with adaptation_field carrying PCR. The
// remainder of the AF is 0xFF stuffing so the packet is exactly 188 bytes.
std::array<std::uint8_t, kTsPacketSize>
makeRemuxPcrPacket(std::uint16_t pid, std::uint64_t pcr_27mhz, std::uint8_t cc = 0) {
    std::array<std::uint8_t, kTsPacketSize> p{};
    p[0] = ts::kTsSyncByte;
    p[1] = static_cast<std::uint8_t>((pid >> 8) & 0x1F);
    p[2] = static_cast<std::uint8_t>(pid & 0xFF);
    p[3] = 0x20 | (cc & 0x0F);   // adaptation_field_only
    p[4] = 183;                   // af length covers rest of packet
    p[5] = 0x10;                  // PCR_flag
    ts::TsPacketMut m{std::span<std::uint8_t, kTsPacketSize>(p.data(), kTsPacketSize)};
    m.setPcr27Mhz(pcr_27mhz);
    for (std::size_t i = 12; i < kTsPacketSize; ++i) p[i] = 0xFF;
    return p;
}

std::vector<std::uint8_t> wrapDatagram(
    const std::array<std::uint8_t, kTsPacketSize>& pkt) {
    return std::vector<std::uint8_t>(pkt.data(), pkt.data() + kTsPacketSize);
}

}  // namespace

TEST(RemuxGatewayTest, FirstPcrOnRegisteredPidPreservedAsAnchor) {
    GatewayCfg cfg = makeRemuxCfg();
    cfg.remux.target_bitrate_bps = 10'000'000;
    RemuxGateway g(1, "remux", cfg);
    // Register pcr_pid for input 0 (video 0x200) via PSI.
    g.testFeedDatagram(0, makeSpts(1, 0x100, 0x200, 0x201));
    g.testFeedDatagram(1, makeSpts(2, 0x100, 0x300, 0x301, 2));   // distinct PIDs to avoid collision
    g.testDrainOutput();   // discard PSI/ES from PSI feed

    // Feed an anchor PCR packet on input 0's video PID (0x200, identity-mapped).
    constexpr std::uint64_t kAnchor = 5'000'000;
    auto pcr_pkt = makeRemuxPcrPacket(0x200, kAnchor);
    g.testFeedDatagram(0, wrapDatagram(pcr_pkt));

    auto out = g.testDrainOutput();
    bool saw_anchor = false;
    for (const auto& dg : out) {
        for (std::size_t i = 0; i + kTsPacketSize <= dg.size(); i += kTsPacketSize) {
            ts::TsPacketView v(std::span<const std::uint8_t, kTsPacketSize>(
                dg.data() + i, kTsPacketSize));
            if (!v.isValidSync()) continue;
            if (v.pid() != 0x200) continue;
            if (v.pcr27Mhz() == ts::TsPacketView::kPcrNone) continue;
            EXPECT_EQ(v.pcr27Mhz(), kAnchor);   // anchor PCR preserved
            saw_anchor = true;
        }
    }
    EXPECT_TRUE(saw_anchor);
}

TEST(RemuxGatewayTest, SubsequentPcrIsRestampedFromByteCounter) {
    GatewayCfg cfg = makeRemuxCfg();
    cfg.remux.target_bitrate_bps = 10'000'000;
    RemuxGateway g(1, "remux", cfg);
    g.testFeedDatagram(0, makeSpts(1, 0x100, 0x200, 0x201));
    g.testFeedDatagram(1, makeSpts(2, 0x100, 0x300, 0x301, 2));
    g.testDrainOutput();

    g.testFeedDatagram(0, wrapDatagram(makeRemuxPcrPacket(0x200, 5'000'000, 0)));
    // Bursty ES packets to advance bytes_emitted significantly.
    for (int n = 0; n < 5; ++n) {
        g.testFeedDatagram(0, makeSpts(1, 0x100, 0x200, 0x201));
    }
    // Bogus high input PCR — must be rewritten by the restamper.
    g.testFeedDatagram(0, wrapDatagram(makeRemuxPcrPacket(0x200, 999'999'999, 1)));

    auto out = g.testDrainOutput();
    std::vector<std::uint64_t> pcrs;
    for (const auto& dg : out) {
        for (std::size_t i = 0; i + kTsPacketSize <= dg.size(); i += kTsPacketSize) {
            ts::TsPacketView v(std::span<const std::uint8_t, kTsPacketSize>(
                dg.data() + i, kTsPacketSize));
            if (!v.isValidSync()) continue;
            if (v.pid() != 0x200) continue;
            if (v.pcr27Mhz() == ts::TsPacketView::kPcrNone) continue;
            pcrs.push_back(v.pcr27Mhz());
        }
    }
    ASSERT_GE(pcrs.size(), 2u);
    EXPECT_EQ(pcrs.front(), 5'000'000u);          // anchor preserved
    // Restamped PCR must NOT carry the bogus input value, and must be > anchor
    // (we sent more bytes after the anchor). It must also be far below the
    // bogus input: input was 999'999'999, restamp at 10 Mbps over a few KB
    // can't reach that.
    EXPECT_NE(pcrs.back(), 999'999'999u);
    EXPECT_GT(pcrs.back(), 5'000'000u);
    EXPECT_LT(pcrs.back(), 999'999'999u);
}

TEST(RemuxGatewayTest, PcrPassthroughWhenBitrateZero) {
    // Default cfg has target_bitrate_bps == 0 ⇒ pass-through. Anchor still set,
    // subsequent PCRs are NOT rewritten.
    RemuxGateway g(1, "remux", makeRemuxCfg());
    g.testFeedDatagram(0, makeSpts(1, 0x100, 0x200, 0x201));
    g.testFeedDatagram(1, makeSpts(2, 0x100, 0x300, 0x301, 2));
    g.testDrainOutput();

    g.testFeedDatagram(0, wrapDatagram(makeRemuxPcrPacket(0x200, 1'000'000, 0)));
    g.testFeedDatagram(0, wrapDatagram(makeRemuxPcrPacket(0x200, 7'777'777, 1)));

    auto out = g.testDrainOutput();
    std::vector<std::uint64_t> pcrs;
    for (const auto& dg : out) {
        for (std::size_t i = 0; i + kTsPacketSize <= dg.size(); i += kTsPacketSize) {
            ts::TsPacketView v(std::span<const std::uint8_t, kTsPacketSize>(
                dg.data() + i, kTsPacketSize));
            if (!v.isValidSync() || v.pid() != 0x200) continue;
            if (v.pcr27Mhz() == ts::TsPacketView::kPcrNone) continue;
            pcrs.push_back(v.pcr27Mhz());
        }
    }
    ASSERT_EQ(pcrs.size(), 2u);
    EXPECT_EQ(pcrs[0], 1'000'000u);
    EXPECT_EQ(pcrs[1], 7'777'777u);   // unmodified
}

// ─── Status JSON ─────────────────────────────────────────────────────────────

TEST(RemuxGatewayTest, StatusJsonExposesInputsAndPrograms) {
    RemuxGateway g(1, "remux", makeRemuxCfg());
    g.testFeedDatagram(0, makeSpts(1, 0x100, 0x200, 0x201));
    g.testFeedDatagram(1, makeSpts(2, 0x100, 0x200, 0x201, 2));

    auto j = g.statusJson();
    EXPECT_EQ(j.at("mode").get<std::string>(), "remux");
    ASSERT_TRUE(j.contains("inputs"));
    EXPECT_EQ(j.at("inputs").size(), 2u);
    ASSERT_TRUE(j.contains("programs"));
    EXPECT_EQ(j.at("programs").size(), 2u);
}
