#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <vector>

#include "gateway/ts/Crc32.h"
#include "gateway/ts/Descriptors.h"
#include "gateway/ts/PsiParser.h"
#include "gateway/ts/TsPacket.h"

using namespace liveqx::gateway::ts;

namespace {

// Append 4-byte big-endian MPEG-2 CRC32 over the buffer (computed over all
// preceding bytes) to make a self-consistent PSI section.
void appendCrc(std::vector<std::uint8_t>& buf) {
    const std::uint32_t c = crc32Mpeg2(std::span<const std::uint8_t>(buf.data(), buf.size()));
    buf.push_back(static_cast<std::uint8_t>((c >> 24) & 0xFF));
    buf.push_back(static_cast<std::uint8_t>((c >> 16) & 0xFF));
    buf.push_back(static_cast<std::uint8_t>((c >> 8)  & 0xFF));
    buf.push_back(static_cast<std::uint8_t>( c        & 0xFF));
}

// Build a section with the given table_id and body. section_length is filled
// in to be (body.size() + 4) — body bytes plus the 4-byte CRC.
std::vector<std::uint8_t> makeSection(std::uint8_t table_id,
                                      std::uint16_t tsid_or_program,
                                      std::uint8_t version,
                                      std::span<const std::uint8_t> body) {
    std::vector<std::uint8_t> sec;
    sec.reserve(8 + body.size() + 4);
    sec.push_back(table_id);
    // section_syntax_indicator=1, '0', reserved=11, section_length(12)
    const std::size_t section_length = 5 + body.size() + 4;  // 5 ext-header bytes + body + CRC
    sec.push_back(static_cast<std::uint8_t>(0x80 | 0x30 | ((section_length >> 8) & 0x0F)));
    sec.push_back(static_cast<std::uint8_t>(section_length & 0xFF));
    sec.push_back(static_cast<std::uint8_t>((tsid_or_program >> 8) & 0xFF));
    sec.push_back(static_cast<std::uint8_t>( tsid_or_program       & 0xFF));
    sec.push_back(static_cast<std::uint8_t>(0xC0 | ((version & 0x1F) << 1) | 0x01));  // current_next=1
    sec.push_back(0);  // section_number
    sec.push_back(0);  // last_section_number
    sec.insert(sec.end(), body.begin(), body.end());
    appendCrc(sec);
    return sec;
}

// Pack a complete section into one-or-more TS packets on `pid` with PUSI on
// the first packet, pointer_field=0, CC starting at `cc0`. Pads remainder
// with 0xFF stuffing.
std::vector<std::array<std::uint8_t, kTsPacketSize>>
sectionToPackets(std::uint16_t pid, std::span<const std::uint8_t> section, std::uint8_t cc0 = 0) {
    std::vector<std::array<std::uint8_t, kTsPacketSize>> out;
    std::size_t cursor = 0;
    bool first = true;
    std::uint8_t cc = cc0;
    while (cursor < section.size()) {
        std::array<std::uint8_t, kTsPacketSize> pkt{};
        pkt[0] = kTsSyncByte;
        pkt[1] = static_cast<std::uint8_t>(((first ? 1 : 0) << 6) | ((pid >> 8) & 0x1F));
        pkt[2] = static_cast<std::uint8_t>(pid & 0xFF);
        pkt[3] = static_cast<std::uint8_t>((static_cast<std::uint8_t>(AdaptationFieldControl::PayloadOnly) << 4) | (cc & 0x0F));
        std::size_t off = 4;
        if (first) {
            pkt[off++] = 0x00;  // pointer_field
        }
        const std::size_t avail = kTsPacketSize - off;
        const std::size_t take  = std::min(avail, section.size() - cursor);
        std::memcpy(pkt.data() + off, section.data() + cursor, take);
        for (std::size_t i = off + take; i < kTsPacketSize; ++i) pkt[i] = 0xFF;
        cursor += take;
        first = false;
        cc = (cc + 1) & 0x0F;
        out.push_back(pkt);
    }
    return out;
}

}  // namespace

// ─── CRC32 ──────────────────────────────────────────────────────────────────

TEST(Crc32Test, KnownVectorAllZeros) {
    // CRC32-MPEG2 of "123456789" is 0x0376E6E7 per published reference.
    const std::uint8_t input[] = {'1','2','3','4','5','6','7','8','9'};
    EXPECT_EQ(crc32Mpeg2(std::span<const std::uint8_t>(input, sizeof input)),
              0x0376E6E7u);
}

TEST(Crc32Test, AppendedCrcIsZeroOverWhole) {
    // The MPEG-2 trick: CRC over the data + its appended CRC equals zero.
    std::vector<std::uint8_t> buf = {0x12, 0x34, 0x56, 0x78, 0x9A};
    appendCrc(buf);
    EXPECT_EQ(crc32Mpeg2(buf), 0u);
}

// ─── Descriptors ────────────────────────────────────────────────────────────

TEST(DescriptorTest, ParseLoopWalksTagLengthBody) {
    const std::uint8_t buf[] = {
        0x0A, 3, 'r','u','s',          // ISO 639 language: rus
        0x52, 1, 0x07,                 // stream_identifier component_tag=7
        0x59, 8,  'e','n','g',0x10, 0,1, 0,2  // subtitling
    };
    auto descs = parseDescriptorLoop(std::span<const std::uint8_t>(buf, sizeof buf));
    ASSERT_EQ(descs.size(), 3u);
    EXPECT_EQ(descs[0].tag, 0x0A);
    EXPECT_EQ(descs[1].tag, 0x52);
    EXPECT_EQ(descs[2].tag, 0x59);
    EXPECT_EQ(descs[2].body.size(), 8u);
}

TEST(DescriptorTest, FindIso639Language) {
    const std::uint8_t lang_body[] = {'e','n','g', 0x00};
    std::vector<RawDescriptor> descs = {
        {0x52, {0x07}},
        {0x0A, std::vector<std::uint8_t>(lang_body, lang_body + sizeof lang_body)}
    };
    auto lc = findIso639Language(descs);
    ASSERT_TRUE(lc.has_value());
    EXPECT_EQ(lc->toString(), "eng");
}

TEST(DescriptorTest, ParseSubtitlingMultipleEntries) {
    // Two entries: rus and eng, 8 bytes each.
    const std::uint8_t body[] = {
        'r','u','s', 0x10, 0,1, 0,2,
        'e','n','g', 0x10, 0,3, 0,4,
    };
    auto entries = parseSubtitlingDescriptor(std::span<const std::uint8_t>(body, sizeof body));
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[0].language.toString(), "rus");
    EXPECT_EQ(entries[0].subtitling_type, 0x10);
    EXPECT_EQ(entries[0].composition_page_id, 1u);
    EXPECT_EQ(entries[0].ancillary_page_id, 2u);
    EXPECT_EQ(entries[1].language.toString(), "eng");
}

TEST(DescriptorTest, ParseTeletextEntries) {
    const std::uint8_t body[] = {
        'r','u','s', static_cast<std::uint8_t>((0x01 << 3) | 0x02), 0x05,
        'e','n','g', static_cast<std::uint8_t>((0x02 << 3) | 0x04), 0x10,
    };
    auto entries = parseTeletextDescriptor(std::span<const std::uint8_t>(body, sizeof body));
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[0].language.toString(), "rus");
    EXPECT_EQ(entries[0].teletext_type, 0x01);
    EXPECT_EQ(entries[0].magazine_number, 0x02);
    EXPECT_EQ(entries[0].page_number, 0x05);
    EXPECT_EQ(entries[1].magazine_number, 0x04);
}

// ─── PAT ────────────────────────────────────────────────────────────────────

TEST(PsiParserPatTest, ParsesTwoPrograms) {
    // Body of PAT: each entry is 4 bytes (program_number + reserved + pmt_pid).
    std::vector<std::uint8_t> body = {
        0x00, 0x01,  0xE0, 0x20,   // program 1 → PMT PID 0x0020
        0x00, 0x02,  0xE0, 0x21,   // program 2 → PMT PID 0x0021
    };
    auto sec = makeSection(/*table_id=*/0x00, /*tsid=*/0x1234, /*version=*/3, body);
    auto parsed = parsePat(sec);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->transport_stream_id, 0x1234);
    EXPECT_EQ(parsed->version_number, 3);
    EXPECT_TRUE(parsed->current_next_indicator);
    ASSERT_EQ(parsed->programs.size(), 2u);
    EXPECT_EQ(parsed->programs[0].program_number, 1u);
    EXPECT_EQ(parsed->programs[0].pmt_pid, 0x0020);
    EXPECT_EQ(parsed->programs[1].pmt_pid, 0x0021);
}

TEST(PsiParserPatTest, RejectsBadCrc) {
    std::vector<std::uint8_t> body = { 0x00, 0x01, 0xE0, 0x20 };
    auto sec = makeSection(0x00, 0x1234, 3, body);
    sec.back() ^= 0x01;  // corrupt CRC
    EXPECT_FALSE(parsePat(sec).has_value());
}

TEST(PsiParserPatTest, ExtractsNetworkPid) {
    std::vector<std::uint8_t> body = {
        0x00, 0x00,  0xE0, 0x10,   // program 0 = network_pid 0x0010
        0x00, 0x01,  0xE0, 0x20,
    };
    auto sec = makeSection(0x00, 0x4242, 0, body);
    auto parsed = parsePat(sec);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->network_pid, 0x0010);
    ASSERT_EQ(parsed->programs.size(), 1u);
}

// ─── PMT ────────────────────────────────────────────────────────────────────

TEST(PsiParserPmtTest, ParsesH264AndAacStreams) {
    // PMT body layout:
    //   PCR_PID (16) | reserved(4) | program_info_length(12) | program descriptors
    //   N × { stream_type | reserved | elementary_pid | reserved | es_info_length | es_descs }
    std::vector<std::uint8_t> body;
    // PCR_PID = 0x0100
    body.push_back(0xE1); body.push_back(0x00);
    // program_info_length = 0
    body.push_back(0xF0); body.push_back(0x00);
    // Stream 1: H.264, PID 0x0100
    body.push_back(0x1B);
    body.push_back(0xE1); body.push_back(0x00);
    body.push_back(0xF0); body.push_back(0x00);
    // Stream 2: AAC, PID 0x0101, with ISO 639 language descriptor
    body.push_back(0x0F);
    body.push_back(0xE1); body.push_back(0x01);
    body.push_back(0xF0); body.push_back(0x06);  // es_info_length = 6
    body.push_back(0x0A); body.push_back(4);     // ISO_639_language descriptor
    body.push_back('r');  body.push_back('u');  body.push_back('s');  body.push_back(0x00);

    auto sec = makeSection(/*table_id=*/0x02, /*program=*/1, /*version=*/0, body);
    auto parsed = parsePmt(sec);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->program_number, 1u);
    EXPECT_EQ(parsed->pcr_pid, 0x0100);
    ASSERT_EQ(parsed->streams.size(), 2u);
    EXPECT_EQ(parsed->streams[0].stream_type, 0x1B);
    EXPECT_EQ(parsed->streams[0].elementary_pid, 0x0100);
    EXPECT_EQ(parsed->streams[1].stream_type, 0x0F);
    EXPECT_EQ(parsed->streams[1].elementary_pid, 0x0101);
    auto lang = findIso639Language(parsed->streams[1].es_descriptors);
    ASSERT_TRUE(lang.has_value());
    EXPECT_EQ(lang->toString(), "rus");
}

TEST(PsiParserPmtTest, RejectsTruncatedSection) {
    std::vector<std::uint8_t> body;
    body.push_back(0xE1); body.push_back(0x00);
    body.push_back(0xF0); body.push_back(0x10);  // claim 16 bytes program info but provide none
    auto sec = makeSection(0x02, 1, 0, body);
    EXPECT_FALSE(parsePmt(sec).has_value());
}

// ─── SDT ────────────────────────────────────────────────────────────────────

TEST(PsiParserSdtTest, ExtractsServiceAndProviderName) {
    // Service descriptor (tag 0x48): service_type | provider_name_length | name | name_length | name
    std::vector<std::uint8_t> svc_desc = { 0x48, /*len*/ 1 + 4 + 1 + 7 };
    svc_desc.push_back(0x01);  // service_type = digital TV
    svc_desc.push_back(3);     // provider name length 3
    svc_desc.push_back('B'); svc_desc.push_back('B'); svc_desc.push_back('C');
    svc_desc.push_back(7);     // service name length 7
    svc_desc.push_back('N'); svc_desc.push_back('e'); svc_desc.push_back('w');
    svc_desc.push_back('s'); svc_desc.push_back(' '); svc_desc.push_back('H');
    svc_desc.push_back('D');
    // Adjust descriptor length now that we know body size.
    svc_desc[1] = static_cast<std::uint8_t>(svc_desc.size() - 2);

    // SDT body layout:
    //   original_network_id(16) | reserved(8)
    //   N × { service_id(16) | reserved(6)|EIT_sched|EIT_PF
    //         | running_status(3)|free_CA|descriptors_loop_length(12)
    //         | descriptors }
    std::vector<std::uint8_t> body;
    body.push_back(0x12); body.push_back(0x34);  // original_network_id
    body.push_back(0xFF);                         // reserved-future-use
    body.push_back(0x00); body.push_back(0x07);  // service_id 7
    body.push_back(0x01);                         // EIT_PF=1
    const std::size_t descs_len = svc_desc.size();
    body.push_back(static_cast<std::uint8_t>(0x80 | ((descs_len >> 8) & 0x0F)));  // running_status=4 (running) | descs_len high
    body.push_back(static_cast<std::uint8_t>(descs_len & 0xFF));
    body.insert(body.end(), svc_desc.begin(), svc_desc.end());

    auto sec = makeSection(/*table_id=*/0x42, /*tsid=*/0xCAFE, /*version=*/1, body);
    auto parsed = parseSdt(sec);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->transport_stream_id, 0xCAFE);
    EXPECT_EQ(parsed->original_network_id, 0x1234);
    ASSERT_EQ(parsed->services.size(), 1u);
    const auto& s = parsed->services[0];
    EXPECT_EQ(s.service_id, 7u);
    EXPECT_TRUE(s.eit_present_following_flag);
    EXPECT_EQ(s.provider_name, "BBC");
    EXPECT_EQ(s.service_name,  "News HD");
}

// ─── EIT ────────────────────────────────────────────────────────────────────

TEST(PsiParserEitTest, ParsesPresentFollowingEvent) {
    // EIT body (after 8-byte common header):
    //   transport_stream_id(16) | original_network_id(16)
    //   segment_last_section_number(8) | last_table_id(8)
    //   N × event entries
    std::vector<std::uint8_t> body;
    body.push_back(0xCA); body.push_back(0xFE);  // tsid
    body.push_back(0x12); body.push_back(0x34);  // original_network_id
    body.push_back(0x00);                         // segment_last_section_number
    body.push_back(0x4E);                         // last_table_id
    // Event:
    //   event_id(16) | start_time MJD+UTC (5) | duration BCD (3) | running_status(3)|free_CA|descs_len(12) | descs
    body.push_back(0xBE); body.push_back(0xEF);
    // MJD = 0 (start_time_undefined) → returns 0
    body.push_back(0xFF); body.push_back(0xFF); body.push_back(0); body.push_back(0); body.push_back(0);
    // Duration 01:30:00 BCD
    body.push_back(0x01); body.push_back(0x30); body.push_back(0x00);
    // running_status=4 (running), free_CA=0, descs_len=0
    body.push_back(0x80); body.push_back(0x00);

    auto sec = makeSection(/*table_id=*/0x4E, /*service_id=*/7, /*version=*/0, body);
    auto parsed = parseEit(sec);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->table_id, 0x4Eu);
    EXPECT_TRUE(parsed->actual);
    EXPECT_TRUE(parsed->present_following);
    EXPECT_EQ(parsed->service_id, 7u);
    EXPECT_EQ(parsed->transport_stream_id, 0xCAFE);
    EXPECT_EQ(parsed->original_network_id, 0x1234);
    ASSERT_EQ(parsed->events.size(), 1u);
    EXPECT_EQ(parsed->events[0].event_id, 0xBEEF);
    EXPECT_EQ(parsed->events[0].start_time_utc, 0u);
    EXPECT_EQ(parsed->events[0].duration_sec, 1u * 3600 + 30 * 60);
    EXPECT_EQ(parsed->events[0].running_status, 4u);
}

// ─── Section assembler ──────────────────────────────────────────────────────

TEST(PsiAssemblerTest, SmallSectionInOnePacket) {
    std::vector<std::uint8_t> body = { 0x00, 0x01, 0xE0, 0x20 };
    auto sec = makeSection(0x00, 0x1234, 0, body);
    auto pkts = sectionToPackets(kPidPat, sec);
    ASSERT_EQ(pkts.size(), 1u);

    PsiSectionAssembler asm_;
    int hits = 0;
    asm_.feed(TsPacketView{pkts[0]},
              [&](std::uint16_t pid, std::span<const std::uint8_t> s) {
                  ++hits;
                  EXPECT_EQ(pid, kPidPat);
                  ASSERT_EQ(s.size(), sec.size());
                  EXPECT_EQ(std::memcmp(s.data(), sec.data(), sec.size()), 0);
              });
    EXPECT_EQ(hits, 1);
}

TEST(PsiAssemblerTest, LargeSectionAcrossMultiplePackets) {
    // Build a PAT with enough programs to exceed one packet (each entry = 4B,
    // header overhead 8B + 4B CRC; one packet payload is 184 - 1 ptr = 183B).
    std::vector<std::uint8_t> body;
    for (int i = 0; i < 80; ++i) {
        const std::uint16_t pn  = static_cast<std::uint16_t>(i + 1);
        const std::uint16_t pid = static_cast<std::uint16_t>(0x100 + i);
        body.push_back(static_cast<std::uint8_t>(pn >> 8));
        body.push_back(static_cast<std::uint8_t>(pn & 0xFF));
        body.push_back(static_cast<std::uint8_t>(0xE0 | ((pid >> 8) & 0x1F)));
        body.push_back(static_cast<std::uint8_t>(pid & 0xFF));
    }
    auto sec = makeSection(0x00, 0x1234, 0, body);
    auto pkts = sectionToPackets(kPidPat, sec);
    ASSERT_GE(pkts.size(), 2u);

    PsiSectionAssembler asm_;
    int hits = 0;
    for (const auto& p : pkts) {
        asm_.feed(TsPacketView{p},
                  [&](std::uint16_t pid, std::span<const std::uint8_t> s) {
                      ++hits;
                      EXPECT_EQ(pid, kPidPat);
                      auto parsed = parsePat(s);
                      ASSERT_TRUE(parsed.has_value());
                      EXPECT_EQ(parsed->programs.size(), 80u);
                  });
    }
    EXPECT_EQ(hits, 1);
}

TEST(PsiAssemblerTest, CcDiscontinuityDropsInProgress) {
    std::vector<std::uint8_t> body;
    for (int i = 0; i < 80; ++i) {
        body.push_back(0); body.push_back(static_cast<std::uint8_t>(i + 1));
        body.push_back(0xE0); body.push_back(static_cast<std::uint8_t>(i));
    }
    auto sec = makeSection(0x00, 1, 0, body);
    auto pkts = sectionToPackets(kPidPat, sec, /*cc0=*/0);

    // Corrupt CC of second packet to simulate loss between packets.
    pkts[1][3] = static_cast<std::uint8_t>((pkts[1][3] & 0xF0) | 0x09);  // not (0+1)&0xF=1

    PsiSectionAssembler asm_;
    int hits = 0;
    for (const auto& p : pkts) {
        asm_.feed(TsPacketView{p},
                  [&](std::uint16_t, std::span<const std::uint8_t>) { ++hits; });
    }
    EXPECT_EQ(hits, 0);  // section dropped due to CC gap
}

TEST(PsiAssemblerTest, ResetForgetsPartial) {
    std::vector<std::uint8_t> body;
    for (int i = 0; i < 80; ++i) {
        body.push_back(0); body.push_back(static_cast<std::uint8_t>(i + 1));
        body.push_back(0xE0); body.push_back(0);
    }
    auto sec = makeSection(0x00, 1, 0, body);
    auto pkts = sectionToPackets(kPidPat, sec);

    PsiSectionAssembler asm_;
    asm_.feed(TsPacketView{pkts[0]}, [](auto, auto) {});
    asm_.reset(kPidPat);
    int hits = 0;
    for (std::size_t i = 1; i < pkts.size(); ++i) {
        asm_.feed(TsPacketView{pkts[i]},
                  [&](std::uint16_t, std::span<const std::uint8_t>) { ++hits; });
    }
    EXPECT_EQ(hits, 0);
}

TEST(PsiAssemblerTest, IgnoresScrambledPayload) {
    // A scrambled packet must not feed PSI assembly per spec.
    std::vector<std::uint8_t> body = { 0x00, 0x01, 0xE0, 0x20 };
    auto sec = makeSection(0x00, 1, 0, body);
    auto pkts = sectionToPackets(kPidPat, sec);
    pkts[0][3] |= 0x80;  // scrambling = 10b
    PsiSectionAssembler asm_;
    int hits = 0;
    asm_.feed(TsPacketView{pkts[0]},
              [&](auto, auto) { ++hits; });
    EXPECT_EQ(hits, 0);
}

TEST(PsiAssemblerTest, IgnoresTeiPacket) {
    std::vector<std::uint8_t> body = { 0x00, 0x01, 0xE0, 0x20 };
    auto sec = makeSection(0x00, 1, 0, body);
    auto pkts = sectionToPackets(kPidPat, sec);
    pkts[0][1] |= 0x80;  // TEI
    PsiSectionAssembler asm_;
    int hits = 0;
    asm_.feed(TsPacketView{pkts[0]}, [&](auto, auto) { ++hits; });
    EXPECT_EQ(hits, 0);
}

// ─── DVB text decoder ───────────────────────────────────────────────────────

TEST(DvbTextTest, AsciiNoSelector) {
    const std::uint8_t buf[] = {'H','e','l','l','o'};
    EXPECT_EQ(decodeDvbText(std::span<const std::uint8_t>(buf, sizeof buf)), "Hello");
}

TEST(DvbTextTest, Iso88595Cyrillic) {
    // 0x05 selector + ISO 8859-5 bytes for "Россия":
    //   Р=0xC0, о=0xDE, с=0xE1, с=0xE1, и=0xD8, я=0xEF
    const std::uint8_t buf[] = {0x05, 0xC0, 0xDE, 0xE1, 0xE1, 0xD8, 0xEF};
    auto s = decodeDvbText(std::span<const std::uint8_t>(buf, sizeof buf));
    EXPECT_EQ(s, std::string("\xD0\xA0\xD0\xBE\xD1\x81\xD1\x81\xD0\xB8\xD1\x8F"));
}

TEST(DvbTextTest, Utf8Selector) {
    const std::uint8_t buf[] = {0x15, 'h', 'i'};
    EXPECT_EQ(decodeDvbText(std::span<const std::uint8_t>(buf, sizeof buf)), "hi");
}

TEST(DvbTextTest, StripsEmphasisCodes) {
    const std::uint8_t buf[] = {'H', 0x86, 'e', 0x87, 'y'};
    EXPECT_EQ(decodeDvbText(std::span<const std::uint8_t>(buf, sizeof buf)), "Hey");
}

TEST(DvbTextTest, BcdDuration) {
    const std::uint8_t buf[] = {0x01, 0x30, 0x00};
    EXPECT_EQ(decodeBcdDuration(std::span<const std::uint8_t, 3>(buf, 3)),
              1u * 3600 + 30u * 60);
}

TEST(DvbTextTest, BcdDurationRejectsBadDigits) {
    const std::uint8_t buf[] = {0xFF, 0x00, 0x00};  // hour = "FF" BCD invalid
    EXPECT_EQ(decodeBcdDuration(std::span<const std::uint8_t, 3>(buf, 3)), 0u);
}

TEST(DvbTextTest, MjdUndefinedReturnsZero) {
    const std::uint8_t buf[] = {0xFF, 0xFF, 0, 0, 0};
    EXPECT_EQ(decodeMjdUtc(std::span<const std::uint8_t, 5>(buf, 5)), 0u);
}
