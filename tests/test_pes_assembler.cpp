// Unit tests for PesAssembler / PesPacketizer / PTS codec.

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

#include "gateway/ts/PesAssembler.h"
#include "gateway/ts/TsPacket.h"

namespace ts = liveqx::gateway::ts;

namespace {

// Build a single TS packet with PUSI flag, CC, and payload <=184 bytes. AF
// stuffing is applied when payload is shorter than 184.
std::array<std::uint8_t, ts::kTsPacketSize>
makeTsPacket(std::uint16_t pid, bool pusi, std::uint8_t cc,
             std::span<const std::uint8_t> payload) {
    EXPECT_LE(payload.size(), 184u);
    std::array<std::uint8_t, ts::kTsPacketSize> p{};
    p[0] = ts::kTsSyncByte;
    p[1] = static_cast<std::uint8_t>((pusi ? 0x40 : 0x00) | ((pid >> 8) & 0x1F));
    p[2] = static_cast<std::uint8_t>(pid & 0xFF);
    if (payload.size() < 184u) {
        const std::size_t af_total = 184u - payload.size();
        const std::uint8_t af_len  = static_cast<std::uint8_t>(af_total - 1u);
        p[3] = static_cast<std::uint8_t>(0x30 | (cc & 0x0F));
        p[4] = af_len;
        if (af_len >= 1) {
            p[5] = 0x00;
            std::memset(&p[6], 0xFF, af_len - 1u);
        }
        std::memcpy(&p[5u + af_len], payload.data(), payload.size());
    } else {
        p[3] = static_cast<std::uint8_t>(0x10 | (cc & 0x0F));
        std::memcpy(&p[4], payload.data(), 184u);
    }
    return p;
}

// Build a PES packet (header + ES) for video/audio stream_ids that carry the
// optional PES header. PTS only or PTS+DTS supported.
std::vector<std::uint8_t>
makePesPacket(std::uint8_t stream_id,
              std::optional<std::int64_t> pts,
              std::optional<std::int64_t> dts,
              std::span<const std::uint8_t> es) {
    std::vector<std::uint8_t> v;
    v.reserve(es.size() + 19);
    v.push_back(0x00);
    v.push_back(0x00);
    v.push_back(0x01);
    v.push_back(stream_id);
    // length placeholder (filled below)
    v.push_back(0x00);
    v.push_back(0x00);

    std::uint8_t pts_dts_flags = 0;
    std::size_t  pts_dts_bytes = 0;
    if (pts.has_value() && dts.has_value()) { pts_dts_flags = 0b11; pts_dts_bytes = 10; }
    else if (pts.has_value())                { pts_dts_flags = 0b10; pts_dts_bytes = 5; }

    v.push_back(0x80);
    v.push_back(static_cast<std::uint8_t>(pts_dts_flags << 6));
    v.push_back(static_cast<std::uint8_t>(pts_dts_bytes));
    if (pts_dts_flags == 0b10) {
        std::uint8_t buf[5];
        ts::encodePts33(buf, *pts, 0x2);
        v.insert(v.end(), buf, buf + 5);
    } else if (pts_dts_flags == 0b11) {
        std::uint8_t buf[5];
        ts::encodePts33(buf, *pts, 0x3);
        v.insert(v.end(), buf, buf + 5);
        ts::encodePts33(buf, *dts, 0x1);
        v.insert(v.end(), buf, buf + 5);
    }
    v.insert(v.end(), es.begin(), es.end());

    // Fill PES_packet_length: bytes after the 6-byte fixed header.
    const std::size_t after_fixed = v.size() - 6u;
    if (stream_id >= 0xE0 && stream_id <= 0xEF) {
        // video: leave as 0 (unbounded)
        v[4] = 0;
        v[5] = 0;
    } else if (after_fixed <= 0xFFFFu) {
        v[4] = static_cast<std::uint8_t>((after_fixed >> 8) & 0xFF);
        v[5] = static_cast<std::uint8_t>(after_fixed & 0xFF);
    } else {
        v[4] = 0;
        v[5] = 0;
    }
    return v;
}

// Slice a PES packet into TS packets on the given PID. Returns the packets in
// emission order, with PUSI set on the first.
std::vector<std::array<std::uint8_t, ts::kTsPacketSize>>
splitIntoTs(std::uint16_t pid, std::uint8_t start_cc, std::span<const std::uint8_t> pes) {
    std::vector<std::array<std::uint8_t, ts::kTsPacketSize>> out;
    std::size_t pos = 0;
    std::uint8_t cc = start_cc;
    bool first = true;
    while (pos < pes.size()) {
        const std::size_t chunk = std::min<std::size_t>(184u, pes.size() - pos);
        out.push_back(makeTsPacket(pid, first, cc,
                                   std::span<const std::uint8_t>(pes.data() + pos, chunk)));
        pos += chunk;
        cc = static_cast<std::uint8_t>((cc + 1) & 0x0F);
        first = false;
    }
    return out;
}

}  // namespace

// ─── PTS codec ───────────────────────────────────────────────────────────────

TEST(PesPtsCodec, RoundTripBoundaries) {
    const std::int64_t cases[] = {
        0, 1, 90'000, 33'000'000,
        (1LL << 33) - 1,  // max 33-bit
        0x123456789LL,
    };
    for (auto v : cases) {
        std::uint8_t buf[5];
        ts::encodePts33(buf, v, 0x2);
        EXPECT_EQ(ts::decodePts33(buf), v) << "value=" << v;
    }
}

TEST(PesPtsCodec, PrefixAndMarkerBits) {
    std::uint8_t buf[5];
    ts::encodePts33(buf, 0, 0x3);
    EXPECT_EQ(buf[0] >> 4, 0x3);     // prefix nibble
    EXPECT_EQ(buf[0] & 0x01, 0x01);  // marker bit
    EXPECT_EQ(buf[2] & 0x01, 0x01);
    EXPECT_EQ(buf[4] & 0x01, 0x01);
}

// ─── PesAssembler basics ─────────────────────────────────────────────────────

TEST(PesAssembler, SinglePacketPesWithPts) {
    std::vector<ts::PesPacket> got;
    ts::PesAssembler a([&](ts::PesPacket&& p) { got.push_back(std::move(p)); });

    const std::vector<std::uint8_t> es{0x00, 0x00, 0x00, 0x01, 0x65, 'A', 'B'};
    const auto pes = makePesPacket(0xE0, 90'000, std::nullopt, es);
    const auto pkts = splitIntoTs(0x100, /*cc=*/0, pes);
    ASSERT_EQ(pkts.size(), 1u);

    a.feed(ts::TsPacketView(pkts[0]));
    a.flush();

    ASSERT_EQ(got.size(), 1u);
    EXPECT_EQ(got[0].stream_id, 0xE0);
    ASSERT_TRUE(got[0].pts_90khz.has_value());
    EXPECT_EQ(*got[0].pts_90khz, 90'000);
    EXPECT_FALSE(got[0].dts_90khz.has_value());
    EXPECT_EQ(got[0].es, es);
}

TEST(PesAssembler, MultiPacketPesReassembles) {
    std::vector<ts::PesPacket> got;
    ts::PesAssembler a([&](ts::PesPacket&& p) { got.push_back(std::move(p)); });

    std::vector<std::uint8_t> big_es(2'000, 0xAB);
    for (std::size_t i = 0; i < big_es.size(); ++i) big_es[i] = static_cast<std::uint8_t>(i & 0xFF);

    const auto pes = makePesPacket(0xE0, 1'234'567, 1'234'500, big_es);
    auto pkts = splitIntoTs(0x100, /*cc=*/3, pes);
    ASSERT_GT(pkts.size(), 1u);

    for (auto& p : pkts) a.feed(ts::TsPacketView(p));
    a.flush();

    ASSERT_EQ(got.size(), 1u);
    ASSERT_TRUE(got[0].pts_90khz.has_value());
    ASSERT_TRUE(got[0].dts_90khz.has_value());
    EXPECT_EQ(*got[0].pts_90khz, 1'234'567);
    EXPECT_EQ(*got[0].dts_90khz, 1'234'500);
    EXPECT_EQ(got[0].es, big_es);
}

TEST(PesAssembler, JoinedMidStreamDropsBeforeFirstPusi) {
    std::vector<ts::PesPacket> got;
    ts::PesAssembler a([&](ts::PesPacket&& p) { got.push_back(std::move(p)); });

    // Feed a non-PUSI packet first — should be ignored.
    std::vector<std::uint8_t> junk(184, 0x55);
    auto p0 = makeTsPacket(0x100, /*pusi=*/false, 0, junk);
    a.feed(ts::TsPacketView(p0));

    // Now a normal PES — should arrive intact.
    const std::vector<std::uint8_t> es{1, 2, 3, 4, 5};
    auto pes = makePesPacket(0xC0, 200, std::nullopt, es);
    auto pkts = splitIntoTs(0x100, 1, pes);
    for (auto& p : pkts) a.feed(ts::TsPacketView(p));
    a.flush();

    ASSERT_EQ(got.size(), 1u);
    EXPECT_EQ(got[0].es, es);
}

TEST(PesAssembler, CcDiscontinuityDropsInProgressPes) {
    std::vector<ts::PesPacket> got;
    ts::PesAssembler a([&](ts::PesPacket&& p) { got.push_back(std::move(p)); });

    std::vector<std::uint8_t> es(500, 0x77);
    auto pes = makePesPacket(0xE0, 50'000, std::nullopt, es);
    auto pkts = splitIntoTs(0x100, /*cc=*/0, pes);
    ASSERT_GE(pkts.size(), 3u);

    // Feed first packet (PUSI=1, cc=0).
    a.feed(ts::TsPacketView(pkts[0]));

    // Skip the second packet (cc=1 missing) — feed packet with cc=2, simulating
    // a CC discontinuity. The assembler should drop the in-progress PES.
    a.feed(ts::TsPacketView(pkts[2]));

    a.flush();
    EXPECT_EQ(got.size(), 0u);
    EXPECT_GE(a.ccDiscontinuities(), 1u);
}

TEST(PesAssembler, MalformedStartCodeCounts) {
    std::vector<ts::PesPacket> got;
    ts::PesAssembler a([&](ts::PesPacket&& p) { got.push_back(std::move(p)); });

    std::vector<std::uint8_t> bad{0x12, 0x34, 0x56, 0xE0, 0x00, 0x00, 'X', 'Y', 'Z'};
    bad.resize(184, 0xFF);
    auto p = makeTsPacket(0x100, /*pusi=*/true, 0, bad);
    a.feed(ts::TsPacketView(p));
    a.flush();

    EXPECT_EQ(got.size(), 0u);
    EXPECT_GE(a.malformedPes(), 1u);
}

TEST(PesAssembler, FlushDeliversUnboundedVideoPes) {
    std::vector<ts::PesPacket> got;
    ts::PesAssembler a([&](ts::PesPacket&& p) { got.push_back(std::move(p)); });

    // Video PES (stream_id 0xE0) — length field is 0 (unbounded). Without
    // a follow-on PUSI=1 packet, only flush() can finalise.
    std::vector<std::uint8_t> es(800, 0x42);
    auto pes = makePesPacket(0xE0, 12345, std::nullopt, es);
    auto pkts = splitIntoTs(0x100, 0, pes);
    for (auto& p : pkts) a.feed(ts::TsPacketView(p));

    EXPECT_EQ(got.size(), 0u);  // not delivered yet — unbounded
    a.flush();
    ASSERT_EQ(got.size(), 1u);
    EXPECT_EQ(got[0].es, es);
}

TEST(PesAssembler, ResetClearsState) {
    std::vector<ts::PesPacket> got;
    ts::PesAssembler a([&](ts::PesPacket&& p) { got.push_back(std::move(p)); });

    auto pes = makePesPacket(0xE0, 1, std::nullopt, std::vector<std::uint8_t>(20, 0x11));
    auto pkts = splitIntoTs(0x100, 0, pes);
    a.feed(ts::TsPacketView(pkts[0]));
    a.reset();
    a.flush();
    EXPECT_EQ(got.size(), 0u);
}

// ─── PesPacketizer ───────────────────────────────────────────────────────────

TEST(PesPacketizer, SmallPesProducesOneTsPacketWithPusi) {
    std::vector<std::array<std::uint8_t, ts::kTsPacketSize>> pkts;
    std::uint8_t cc = 0;
    ts::PesPacketizer pz(0x200, cc, [&](const std::uint8_t* p, std::size_t n) {
        std::array<std::uint8_t, ts::kTsPacketSize> out{};
        std::memcpy(out.data(), p, n);
        pkts.push_back(out);
    });

    ts::PesPacket pes;
    pes.stream_id = 0xC0;
    pes.pts_90khz = 5000;
    pes.es = std::vector<std::uint8_t>{0xFF, 0xF1, 0x4C, 0x80, 'a', 'a', 'c'};
    pz.emit(pes);

    ASSERT_EQ(pkts.size(), 1u);
    ts::TsPacketView v(pkts[0]);
    EXPECT_EQ(v.pid(), 0x200);
    EXPECT_TRUE(v.pusi());
    EXPECT_EQ(v.continuityCounter(), 0);
    EXPECT_EQ(cc, 1);
}

TEST(PesPacketizer, LargePesSpansMultipleTsPackets) {
    std::vector<std::array<std::uint8_t, ts::kTsPacketSize>> pkts;
    std::uint8_t cc = 5;
    ts::PesPacketizer pz(0x200, cc, [&](const std::uint8_t* p, std::size_t n) {
        std::array<std::uint8_t, ts::kTsPacketSize> out{};
        std::memcpy(out.data(), p, n);
        pkts.push_back(out);
    });

    ts::PesPacket pes;
    pes.stream_id = 0xE0;
    pes.pts_90khz = 1'000'000;
    pes.dts_90khz =   999'000;
    pes.es.resize(2'500);
    for (std::size_t i = 0; i < pes.es.size(); ++i) pes.es[i] = static_cast<std::uint8_t>(i & 0xFF);
    pz.emit(pes);

    ASSERT_GT(pkts.size(), 1u);
    ts::TsPacketView first(pkts[0]);
    EXPECT_TRUE(first.pusi());
    EXPECT_EQ(first.continuityCounter(), 5);
    for (std::size_t i = 1; i < pkts.size(); ++i) {
        ts::TsPacketView v(pkts[i]);
        EXPECT_FALSE(v.pusi()) << "packet #" << i;
        EXPECT_EQ(v.continuityCounter(), static_cast<std::uint8_t>((5 + i) & 0x0F));
    }
}

TEST(PesPacketizer, RoundTripThroughAssembler) {
    std::vector<std::array<std::uint8_t, ts::kTsPacketSize>> pkts;
    std::uint8_t cc = 0;
    ts::PesPacketizer pz(0x300, cc, [&](const std::uint8_t* p, std::size_t n) {
        std::array<std::uint8_t, ts::kTsPacketSize> out{};
        std::memcpy(out.data(), p, n);
        pkts.push_back(out);
    });

    ts::PesPacket in;
    in.stream_id = 0xE0;
    in.pts_90khz = 8'888'888;
    in.dts_90khz = 8'888'000;
    in.es.resize(1'500);
    for (std::size_t i = 0; i < in.es.size(); ++i) in.es[i] = static_cast<std::uint8_t>((i * 7) & 0xFF);
    pz.emit(in);

    std::vector<ts::PesPacket> got;
    ts::PesAssembler a([&](ts::PesPacket&& p) { got.push_back(std::move(p)); });
    for (const auto& pk : pkts) a.feed(ts::TsPacketView(pk));
    a.flush();

    ASSERT_EQ(got.size(), 1u);
    EXPECT_EQ(got[0].stream_id, in.stream_id);
    ASSERT_TRUE(got[0].pts_90khz.has_value());
    ASSERT_TRUE(got[0].dts_90khz.has_value());
    EXPECT_EQ(*got[0].pts_90khz, *in.pts_90khz);
    EXPECT_EQ(*got[0].dts_90khz, *in.dts_90khz);
    EXPECT_EQ(got[0].es, in.es);
}
