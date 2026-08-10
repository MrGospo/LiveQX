#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <vector>

#include "gateway/ts/Crc32.h"
#include "gateway/ts/PsiBuilder.h"
#include "gateway/ts/PsiParser.h"
#include "gateway/ts/TsPacket.h"

using namespace liveqx::gateway::ts;

namespace {

// Re-parse a section through PsiSectionAssembler given a stream of TS packets.
// Returns the first complete section payload delivered to the assembler.
std::vector<std::uint8_t> assembleFirstSection(
    std::span<const std::array<std::uint8_t, kTsPacketSize>> pkts,
    std::uint16_t expected_pid) {
    PsiSectionAssembler asm_;
    std::vector<std::uint8_t> out;
    bool got = false;
    auto cb = [&](std::uint16_t pid, std::span<const std::uint8_t> sec) {
        if (got) return;
        EXPECT_EQ(pid, expected_pid);
        out.assign(sec.begin(), sec.end());
        got = true;
    };
    for (const auto& p : pkts) {
        asm_.feed(TsPacketView(std::span<const std::uint8_t, kTsPacketSize>(p.data(), kTsPacketSize)),
                  cb);
        if (got) break;
    }
    return out;
}

}  // namespace

// ─── PAT ────────────────────────────────────────────────────────────────────

TEST(PsiBuilderTest, PatSingleProgramRoundTrips) {
    PatBuildInput in;
    in.transport_stream_id = 0x1234;
    in.version_number = 5;
    in.programs.push_back({1, 0x100});

    auto sec = buildPatSection(in);
    auto parsed = parsePat(sec);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->transport_stream_id, 0x1234u);
    EXPECT_EQ(parsed->version_number, 5u);
    ASSERT_EQ(parsed->programs.size(), 1u);
    EXPECT_EQ(parsed->programs[0].program_number, 1u);
    EXPECT_EQ(parsed->programs[0].pmt_pid, 0x100u);
    EXPECT_EQ(parsed->network_pid, 0u);
}

TEST(PsiBuilderTest, PatMultipleProgramsAndNetwork) {
    PatBuildInput in;
    in.transport_stream_id = 0x4242;
    in.version_number = 0;
    in.network_pid = 0x10;
    in.programs.push_back({1, 0x100});
    in.programs.push_back({2, 0x200});
    in.programs.push_back({99, 0x3FF});

    auto sec = buildPatSection(in);
    auto parsed = parsePat(sec);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->network_pid, 0x10u);
    ASSERT_EQ(parsed->programs.size(), 3u);
    EXPECT_EQ(parsed->programs[2].program_number, 99u);
    EXPECT_EQ(parsed->programs[2].pmt_pid, 0x3FFu);
}

TEST(PsiBuilderTest, PatSectionLengthIsCorrect) {
    PatBuildInput in;
    in.transport_stream_id = 1;
    in.programs.push_back({1, 0x100});
    auto sec = buildPatSection(in);
    // Section length is encoded in low 12 bits of bytes 1..2.
    const std::size_t section_length = ((sec[1] & 0x0F) << 8) | sec[2];
    EXPECT_EQ(section_length + 3u, sec.size());
}

// ─── PMT ────────────────────────────────────────────────────────────────────

TEST(PsiBuilderTest, PmtSingleVideoStreamRoundTrips) {
    PmtBuildInput in;
    in.program_number = 1;
    in.version_number = 3;
    in.pcr_pid = 0x100;
    PmtStream s;
    s.stream_type = 0x1B;       // H.264
    s.elementary_pid = 0x100;
    in.streams.push_back(s);

    auto sec = buildPmtSection(in);
    auto parsed = parsePmt(sec);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->program_number, 1u);
    EXPECT_EQ(parsed->version_number, 3u);
    EXPECT_EQ(parsed->pcr_pid, 0x100u);
    ASSERT_EQ(parsed->streams.size(), 1u);
    EXPECT_EQ(parsed->streams[0].stream_type, 0x1Bu);
    EXPECT_EQ(parsed->streams[0].elementary_pid, 0x100u);
}

TEST(PsiBuilderTest, PmtPreservesEsDescriptors) {
    PmtBuildInput in;
    in.program_number = 1;
    in.pcr_pid = 0x100;
    PmtStream s;
    s.stream_type = 0x06;
    s.elementary_pid = 0x110;
    // Inject a 0x59 subtitling descriptor (8 bytes per language entry).
    RawDescriptor sub;
    sub.tag = 0x59;
    sub.body = {'r','u','s', 0x10, 0x00, 0x01, 0x00, 0x02};
    s.es_descriptors.push_back(sub);
    in.streams.push_back(s);

    auto sec = buildPmtSection(in);
    auto parsed = parsePmt(sec);
    ASSERT_TRUE(parsed.has_value());
    ASSERT_EQ(parsed->streams.size(), 1u);
    ASSERT_EQ(parsed->streams[0].es_descriptors.size(), 1u);
    EXPECT_EQ(parsed->streams[0].es_descriptors[0].tag, 0x59u);
    EXPECT_EQ(parsed->streams[0].es_descriptors[0].body.size(), 8u);
}

TEST(PsiBuilderTest, PmtPreservesProgramDescriptors) {
    PmtBuildInput in;
    in.program_number = 7;
    in.pcr_pid = 0x100;
    RawDescriptor reg;
    reg.tag = 0x05;
    reg.body = {'H','D','M','V'};
    in.program_descriptors.push_back(reg);
    PmtStream s; s.stream_type = 0x1B; s.elementary_pid = 0x100;
    in.streams.push_back(s);

    auto sec = buildPmtSection(in);
    auto parsed = parsePmt(sec);
    ASSERT_TRUE(parsed.has_value());
    ASSERT_EQ(parsed->program_descriptors.size(), 1u);
    EXPECT_EQ(parsed->program_descriptors[0].tag, 0x05u);
    EXPECT_EQ(parsed->program_descriptors[0].body.size(), 4u);
}

TEST(PsiBuilderTest, PmtMultipleStreamsKeepOrder) {
    PmtBuildInput in;
    in.program_number = 1;
    in.pcr_pid = 0x100;
    PmtStream v; v.stream_type = 0x1B; v.elementary_pid = 0x100; in.streams.push_back(v);
    PmtStream a; a.stream_type = 0x0F; a.elementary_pid = 0x101; in.streams.push_back(a);
    PmtStream sub; sub.stream_type = 0x06; sub.elementary_pid = 0x102; in.streams.push_back(sub);

    auto sec = buildPmtSection(in);
    auto parsed = parsePmt(sec);
    ASSERT_TRUE(parsed.has_value());
    ASSERT_EQ(parsed->streams.size(), 3u);
    EXPECT_EQ(parsed->streams[0].elementary_pid, 0x100u);
    EXPECT_EQ(parsed->streams[1].elementary_pid, 0x101u);
    EXPECT_EQ(parsed->streams[2].elementary_pid, 0x102u);
}

// ─── SDT ────────────────────────────────────────────────────────────────────

TEST(PsiBuilderTest, SdtRoundTripsServiceMetadata) {
    SdtBuildInput in;
    in.transport_stream_id = 0x1234;
    in.original_network_id = 0xABCD;
    in.version_number = 1;
    SdtService svc;
    svc.service_id = 1;
    svc.eit_present_following_flag = true;
    svc.running_status = 4;
    svc.descriptors.push_back(makeServiceDescriptor(0x01, "BBC", "BBC News"));
    in.services.push_back(svc);

    auto sec = buildSdtSection(in);
    auto parsed = parseSdt(sec);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->transport_stream_id, 0x1234u);
    EXPECT_EQ(parsed->original_network_id, 0xABCDu);
    ASSERT_EQ(parsed->services.size(), 1u);
    EXPECT_EQ(parsed->services[0].service_id, 1u);
    EXPECT_EQ(parsed->services[0].running_status, 4u);
    EXPECT_TRUE(parsed->services[0].eit_present_following_flag);
    EXPECT_EQ(parsed->services[0].provider_name, "BBC");
    EXPECT_EQ(parsed->services[0].service_name, "BBC News");
}

TEST(PsiBuilderTest, SdtOtherTransportUsesTableId46) {
    SdtBuildInput in;
    in.actual = false;
    in.transport_stream_id = 0x4567;
    in.original_network_id = 0x10;
    in.services.push_back({});
    in.services.back().service_id = 42;
    auto sec = buildSdtSection(in);
    EXPECT_EQ(sec[0], 0x46u);
    auto parsed = parseSdt(sec);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->services[0].service_id, 42u);
}

// ─── Service descriptor helper ─────────────────────────────────────────────

TEST(PsiBuilderTest, ServiceDescriptorAsciiHasNoSelector) {
    auto d = makeServiceDescriptor(0x01, "BBC", "BBC News");
    EXPECT_EQ(d.tag, 0x48u);
    // body[0] = service_type, [1] = provider_name_length, then bytes.
    EXPECT_EQ(d.body[0], 0x01u);
    EXPECT_EQ(d.body[1], 3u);                                      // "BBC"
    EXPECT_EQ(d.body[2], 'B');
    // 0x01..0x1F selectors must NOT appear for plain ASCII.
    EXPECT_NE(d.body[2], 0x15u);
}

TEST(PsiBuilderTest, ServiceDescriptorUtf8AddsSelector) {
    auto d = makeServiceDescriptor(0x01, "Россия", "Россия 1");
    // First name byte after length should be 0x15 selector when non-ASCII.
    const std::uint8_t prov_len = d.body[1];
    EXPECT_EQ(d.body[2], 0x15u);
    EXPECT_GT(prov_len, 0u);
    // sanity: section round-trips through SDT decode
    SdtBuildInput in;
    in.transport_stream_id = 1;
    SdtService svc;
    svc.service_id = 5;
    svc.descriptors.push_back(d);
    in.services.push_back(svc);
    auto sec = buildSdtSection(in);
    auto parsed = parseSdt(sec);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->services[0].service_name, "Россия 1");
    EXPECT_EQ(parsed->services[0].provider_name, "Россия");
}

// ─── Packetization ──────────────────────────────────────────────────────────

TEST(PsiBuilderTest, PacketizeSmallSectionOnePacket) {
    PatBuildInput in;
    in.transport_stream_id = 1;
    in.programs.push_back({1, 0x100});
    auto sec = buildPatSection(in);

    std::uint8_t cc = 0;
    auto pkts = packetizeSection(0x0000, sec, cc);
    ASSERT_EQ(pkts.size(), 1u);
    TsPacketView v(std::span<const std::uint8_t, kTsPacketSize>(pkts[0].data(), kTsPacketSize));
    EXPECT_TRUE(v.isValidSync());
    EXPECT_TRUE(v.pusi());
    EXPECT_EQ(v.pid(), 0x0000u);
    EXPECT_EQ(v.continuityCounter(), 0u);
    EXPECT_EQ(cc, 1u);                 // CC advanced
}

TEST(PsiBuilderTest, PacketizeAdvancesCcAcrossSections) {
    PatBuildInput in;
    in.transport_stream_id = 1;
    in.programs.push_back({1, 0x100});
    auto sec = buildPatSection(in);
    std::uint8_t cc = 0;
    auto a = packetizeSection(0x0000, sec, cc);
    auto b = packetizeSection(0x0000, sec, cc);
    ASSERT_EQ(a.size(), 1u);
    ASSERT_EQ(b.size(), 1u);
    EXPECT_EQ(a[0][3] & 0x0F, 0u);     // first packet CC=0
    EXPECT_EQ(b[0][3] & 0x0F, 1u);     // next CC=1
    EXPECT_EQ(cc, 2u);
}

TEST(PsiBuilderTest, PacketizeCcWrapsAt15) {
    PmtBuildInput in;
    in.program_number = 1;
    in.pcr_pid = 0x100;
    PmtStream s; s.stream_type = 0x1B; s.elementary_pid = 0x100;
    in.streams.push_back(s);
    auto sec = buildPmtSection(in);

    std::uint8_t cc = 14;
    auto a = packetizeSection(0x100, sec, cc);
    auto b = packetizeSection(0x100, sec, cc);
    auto c2 = packetizeSection(0x100, sec, cc);
    EXPECT_EQ(a[0][3] & 0x0F, 14u);
    EXPECT_EQ(b[0][3] & 0x0F, 15u);
    EXPECT_EQ(c2[0][3] & 0x0F, 0u);
    EXPECT_EQ(cc, 1u);
}

TEST(PsiBuilderTest, PacketizeStuffsTailWithFf) {
    PatBuildInput in;
    in.transport_stream_id = 1;
    in.programs.push_back({1, 0x100});
    auto sec = buildPatSection(in);
    std::uint8_t cc = 0;
    auto pkts = packetizeSection(0x0000, sec, cc);
    ASSERT_EQ(pkts.size(), 1u);
    // Last byte of a small packet must be 0xFF stuffing.
    EXPECT_EQ(pkts[0][kTsPacketSize - 1], 0xFFu);
}

TEST(PsiBuilderTest, PacketizeLargePmtSpansMultiplePackets) {
    // Build a PMT with many streams to exceed one TS payload (≈ 183 bytes).
    PmtBuildInput in;
    in.program_number = 1;
    in.pcr_pid = 0x100;
    for (std::uint16_t i = 0; i < 40; ++i) {
        PmtStream s;
        s.stream_type = 0x1B;
        s.elementary_pid = static_cast<std::uint16_t>(0x100 + i);
        // Pad with a 0x52 stream_identifier descriptor (1 byte body).
        RawDescriptor d; d.tag = 0x52; d.body = {static_cast<std::uint8_t>(i)};
        s.es_descriptors.push_back(d);
        in.streams.push_back(s);
    }
    auto sec = buildPmtSection(in);
    EXPECT_GT(sec.size(), 183u);
    std::uint8_t cc = 0;
    auto pkts = packetizeSection(0x100, sec, cc);
    EXPECT_GE(pkts.size(), 2u);
    // First packet has PUSI=1, subsequent ones have PUSI=0.
    TsPacketView v0(std::span<const std::uint8_t, kTsPacketSize>(pkts[0].data(), kTsPacketSize));
    TsPacketView v1(std::span<const std::uint8_t, kTsPacketSize>(pkts[1].data(), kTsPacketSize));
    EXPECT_TRUE(v0.pusi());
    EXPECT_FALSE(v1.pusi());
    EXPECT_EQ(v1.continuityCounter(),
              static_cast<std::uint8_t>((v0.continuityCounter() + 1) & 0x0F));
}

// ─── Round-trip through assembler ──────────────────────────────────────────

TEST(PsiBuilderTest, BuiltPmtRoundTripsThroughAssembler) {
    // End-to-end: build → packetize → assembler → parse.
    PmtBuildInput in;
    in.program_number = 7;
    in.pcr_pid = 0x100;
    PmtStream v; v.stream_type = 0x1B; v.elementary_pid = 0x100; in.streams.push_back(v);
    PmtStream a; a.stream_type = 0x0F; a.elementary_pid = 0x101; in.streams.push_back(a);
    auto sec = buildPmtSection(in);

    std::uint8_t cc = 0;
    auto pkts = packetizeSection(0x200, sec, cc);
    auto assembled = assembleFirstSection(
        std::span<const std::array<std::uint8_t, kTsPacketSize>>(pkts.data(), pkts.size()),
        0x200);
    ASSERT_FALSE(assembled.empty());
    auto parsed = parsePmt(assembled);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->program_number, 7u);
    ASSERT_EQ(parsed->streams.size(), 2u);
    EXPECT_EQ(parsed->streams[0].elementary_pid, 0x100u);
    EXPECT_EQ(parsed->streams[1].elementary_pid, 0x101u);
}

TEST(PsiBuilderTest, BuiltLargePmtRoundTripsThroughAssembler) {
    PmtBuildInput in;
    in.program_number = 1;
    in.pcr_pid = 0x100;
    // 40 streams * ~7 bytes (5 + 1-byte 0x52 descriptor with 2-byte tag/length)
    // exceeds 183 bytes one-packet PMT capacity, forcing multi-packet section.
    for (std::uint16_t i = 0; i < 40; ++i) {
        PmtStream s; s.stream_type = 0x1B;
        s.elementary_pid = static_cast<std::uint16_t>(0x100 + i);
        RawDescriptor d; d.tag = 0x52; d.body = {static_cast<std::uint8_t>(i)};
        s.es_descriptors.push_back(d);
        in.streams.push_back(s);
    }
    auto sec = buildPmtSection(in);
    std::uint8_t cc = 0;
    auto pkts = packetizeSection(0x200, sec, cc);
    EXPECT_GE(pkts.size(), 2u);
    auto assembled = assembleFirstSection(
        std::span<const std::array<std::uint8_t, kTsPacketSize>>(pkts.data(), pkts.size()),
        0x200);
    auto parsed = parsePmt(assembled);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->streams.size(), 40u);
}

TEST(PsiBuilderTest, CrcIsConsistentEndToEnd) {
    PatBuildInput in;
    in.transport_stream_id = 0xCAFE;
    in.programs.push_back({1, 0x100});
    auto sec = buildPatSection(in);
    // Per MPEG-2 trick, CRC over (data + appended CRC) must equal zero.
    EXPECT_EQ(crc32Mpeg2(std::span<const std::uint8_t>(sec.data(), sec.size())), 0u);
}

// ─── EIT (fix40 A4) ─────────────────────────────────────────────────────────

TEST(PsiBuilderTest, EncodeMjdUtcRoundTripsKnownDate) {
    // 2026-05-10 12:34:56 UTC = unix 1778416496
    constexpr std::uint64_t kUnix = 1778416496;
    std::array<std::uint8_t, 5> raw{};
    encodeMjdUtc(kUnix, std::span<std::uint8_t, 5>(raw.data(), 5));
    EXPECT_EQ(decodeMjdUtc(std::span<const std::uint8_t, 5>(raw.data(), 5)), kUnix);
}

TEST(PsiBuilderTest, EncodeMjdUtcZeroIsAllZeros) {
    std::array<std::uint8_t, 5> raw{0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    encodeMjdUtc(0, std::span<std::uint8_t, 5>(raw.data(), 5));
    for (auto b : raw) EXPECT_EQ(b, 0u);
    // decoder reciprocally returns 0 for a zero-MJD bogus date.
    EXPECT_EQ(decodeMjdUtc(std::span<const std::uint8_t, 5>(raw.data(), 5)), 0u);
}

TEST(PsiBuilderTest, EncodeMjdUtcMjdEpochBoundary) {
    // 1858-11-17 00:00:00 UTC = MJD 0; outside Annex-C decoder's safe range.
    // Use 1900-01-01 00:00:00 UTC = unix -2208988800. Skip negative range.
    // Pick 1970-01-01 12:00:00 = unix 43200, MJD 40587.
    constexpr std::uint64_t kUnix = 43200;
    std::array<std::uint8_t, 5> raw{};
    encodeMjdUtc(kUnix, std::span<std::uint8_t, 5>(raw.data(), 5));
    EXPECT_EQ(((raw[0] << 8) | raw[1]), 40587);
    EXPECT_EQ(decodeMjdUtc(std::span<const std::uint8_t, 5>(raw.data(), 5)), kUnix);
}

TEST(PsiBuilderTest, EncodeBcdDurationRoundTrips) {
    std::array<std::uint8_t, 3> raw{};
    encodeBcdDuration(3 * 3600 + 25 * 60 + 17,
                      std::span<std::uint8_t, 3>(raw.data(), 3));
    EXPECT_EQ(raw[0], 0x03u);
    EXPECT_EQ(raw[1], 0x25u);
    EXPECT_EQ(raw[2], 0x17u);
    EXPECT_EQ(decodeBcdDuration(std::span<const std::uint8_t, 3>(raw.data(), 3)),
              3u * 3600 + 25 * 60 + 17);
}

TEST(PsiBuilderTest, EncodeBcdDurationClampsTo24h) {
    std::array<std::uint8_t, 3> raw{};
    encodeBcdDuration(99 * 3600,
                      std::span<std::uint8_t, 3>(raw.data(), 3));
    // Clamped to 23:59:59 = 86399 seconds.
    EXPECT_EQ(raw[0], 0x23u);
    EXPECT_EQ(raw[1], 0x59u);
    EXPECT_EQ(raw[2], 0x59u);
}

TEST(PsiBuilderTest, EitPresentFollowingActualRoundTrips) {
    EitBuildInput in;
    in.table_id = 0x4E;
    in.service_id = 0x1234;
    in.transport_stream_id = 0xCAFE;
    in.original_network_id = 0x5678;
    in.version_number = 7;

    EitEvent ev;
    ev.event_id = 42;
    ev.start_time_utc = 1778416496;        // 2026-05-10 12:34:56 UTC
    ev.duration_sec = 3600;                // 01:00:00
    ev.running_status = 4;                 // running
    ev.free_ca_mode = false;

    // Short event descriptor 0x4D: lang(3) + name_len(1) + name + text_len(1) + text
    RawDescriptor desc;
    desc.tag = 0x4D;
    desc.body = {'r','u','s', 0x04, 'N','e','w','s', 0x00};
    ev.descriptors.push_back(desc);
    in.events.push_back(ev);

    auto sec = buildEitSection(in);
    EXPECT_EQ(sec[0], 0x4Eu);
    auto parsed = parseEit(sec);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->table_id, 0x4Eu);
    EXPECT_TRUE(parsed->actual);
    EXPECT_TRUE(parsed->present_following);
    EXPECT_EQ(parsed->service_id, 0x1234u);
    EXPECT_EQ(parsed->transport_stream_id, 0xCAFEu);
    EXPECT_EQ(parsed->original_network_id, 0x5678u);
    EXPECT_EQ(parsed->version_number, 7u);
    ASSERT_EQ(parsed->events.size(), 1u);
    EXPECT_EQ(parsed->events[0].event_id, 42u);
    EXPECT_EQ(parsed->events[0].start_time_utc, 1778416496u);
    EXPECT_EQ(parsed->events[0].duration_sec, 3600u);
    EXPECT_EQ(parsed->events[0].running_status, 4u);
    ASSERT_EQ(parsed->events[0].descriptors.size(), 1u);
    EXPECT_EQ(parsed->events[0].descriptors[0].tag, 0x4Du);
}

TEST(PsiBuilderTest, EitOtherScheduleTableIdRoundTrips) {
    EitBuildInput in;
    in.table_id = 0x60;                    // EIT other schedule (first segment)
    in.service_id = 1;
    in.transport_stream_id = 1;
    in.original_network_id = 1;
    EitEvent ev;
    ev.event_id = 1;
    ev.start_time_utc = 1778416496;
    ev.duration_sec = 1800;
    in.events.push_back(ev);

    auto sec = buildEitSection(in);
    EXPECT_EQ(sec[0], 0x60u);
    auto parsed = parseEit(sec);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->table_id, 0x60u);
    EXPECT_FALSE(parsed->actual);
    EXPECT_FALSE(parsed->present_following);
}

TEST(PsiBuilderTest, EitMultipleEventsKeepOrder) {
    EitBuildInput in;
    in.service_id = 5;
    for (std::uint16_t i = 0; i < 4; ++i) {
        EitEvent ev;
        ev.event_id = static_cast<std::uint16_t>(100 + i);
        ev.start_time_utc = 1778416496ull + i * 1800;
        ev.duration_sec = 1800;
        in.events.push_back(ev);
    }
    auto sec = buildEitSection(in);
    auto parsed = parseEit(sec);
    ASSERT_TRUE(parsed.has_value());
    ASSERT_EQ(parsed->events.size(), 4u);
    for (std::size_t i = 0; i < 4; ++i) {
        EXPECT_EQ(parsed->events[i].event_id, 100 + i);
        EXPECT_EQ(parsed->events[i].start_time_utc, 1778416496ull + i * 1800);
    }
}

TEST(PsiBuilderTest, EitEmptyEventListProducesValidSection) {
    EitBuildInput in;
    in.service_id = 1;
    auto sec = buildEitSection(in);
    EXPECT_EQ(crc32Mpeg2(std::span<const std::uint8_t>(sec.data(), sec.size())), 0u);
    auto parsed = parseEit(sec);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_TRUE(parsed->events.empty());
}
