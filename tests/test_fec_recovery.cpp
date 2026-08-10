// fix40 A7 step 5 — RFC 2733 / SMPTE 2022-1 recovery vectors.
//
// Bookend tests for the encoder: walk a synthetic media stream through the
// FecMatrix, simulate a receiver that has lost a known media packet, and
// confirm we can reconstruct the original payload + RTP fields from the
// surviving packets and the column or row FEC packet exactly per
// RFC 2733 §6 (XOR-based receiver) and Pro-MPEG COP3 r2 §9.
//
// We don't compile a full FEC decoder for production yet (the gateway is the
// sender side); these tests stand in as a conformance fixture that proves
// our emitter's headers + XOR aggregates obey the spec well enough that any
// COP3 r2-compliant receiver can recover a single loss per row / column.
//
// Loss patterns covered:
//   - 1D L=4 D=3:        single loss recovered via column FEC.
//   - 1D L=10 D=10:      single loss in a large matrix (max L*D=100 boundary).
//   - 2D L=4 D=4:        single loss via row FEC.
//   - 2D L=4 D=4:        single loss recoverable by column when row also lost.
//   - Variable payload:  every protected media payload is a different size →
//                        decoder must trust length_recovery.
//   - SNBase wrap:       seq counter wraps past 0xFFFF; SNBase ext bumps.
//   - Header semantics:  E=1, X=0, type=0, mask=0, offset/NA matches geometry.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

#include "gateway/GatewayCfg.h"
#include "gateway/fec/FecHeader.h"
#include "gateway/fec/FecMatrix.h"
#include "gateway/fec/RtpPacket.h"

using namespace liveqx::gateway::fec;
using liveqx::gateway::FecCfg;

namespace {

struct MediaPacket {
    std::uint16_t            seq = 0;
    std::uint32_t            ts  = 0;
    std::uint8_t             pt  = 33;
    std::uint32_t            ssrc = 0;
    std::vector<std::uint8_t> payload;     // RTP payload only (no header)

    std::vector<std::uint8_t> bytes() const {
        std::vector<std::uint8_t> p(kRtpHeaderSize + payload.size());
        RtpHeaderFields h{};
        h.payload_type = pt;
        h.sequence     = seq;
        h.timestamp    = ts;
        h.ssrc         = ssrc;
        writeRtpHeader(
            std::span<std::uint8_t, kRtpHeaderSize>(p.data(), kRtpHeaderSize),
            h);
        if (!payload.empty()) {
            std::memcpy(p.data() + kRtpHeaderSize, payload.data(), payload.size());
        }
        return p;
    }
};

struct Sink {
    std::vector<std::vector<std::uint8_t>> packets;
    void operator()(std::span<const std::uint8_t> p) {
        packets.emplace_back(p.begin(), p.end());
    }
};

// Returns a deterministic payload byte pattern unique to (seq, byte_index),
// so XOR aggregate identities are easy to assert.
std::vector<std::uint8_t> makePayload(std::uint16_t seq, std::size_t len,
                                      std::uint32_t salt = 0) {
    std::vector<std::uint8_t> out(len);
    for (std::size_t i = 0; i < len; ++i) {
        out[i] = static_cast<std::uint8_t>(
            (seq * 17u) ^ (static_cast<std::uint32_t>(i) * 31u) ^ salt);
    }
    return out;
}

// Receiver-side single-loss column reconstruction, RFC 2733 §6.
//
// Input:
//   fec_pkt    — full column FEC RTP packet (RTP header + FEC header + payload)
//   surviving  — D-1 surviving media packets in this column, in any order.
// Output:
//   recovered  — fully reconstructed media RTP packet for the missing slot,
//                including RTP header + payload (length per length_recovery).
std::vector<std::uint8_t> recoverFromXorFec(
    const std::vector<std::uint8_t>& fec_pkt,
    const std::vector<MediaPacket>& surviving,
    std::uint32_t expected_media_ssrc,
    std::uint8_t  protected_count_expected) {

    EXPECT_GE(fec_pkt.size(), kRtpHeaderSize + kFecHeaderSize);

    RtpHeaderFields fec_rtp;
    EXPECT_TRUE(readRtpHeader(
        std::span<const std::uint8_t>(fec_pkt.data(), fec_pkt.size()),
        fec_rtp));

    FecHeaderFields fec;
    EXPECT_TRUE(readFecHeader(
        std::span<const std::uint8_t>(fec_pkt.data() + kRtpHeaderSize,
                                       kFecHeaderSize),
        fec));
    EXPECT_TRUE(fec.e_bit);
    EXPECT_FALSE(fec.x_bit);
    EXPECT_EQ(fec.type, 0);
    EXPECT_EQ(fec.mask24, 0u);
    EXPECT_EQ(fec.na, protected_count_expected);

    // Step 1: rebuild the missing packet's RTP fields by XOR'ing the surviving
    // ones against the FEC's recovery aggregates.
    std::uint16_t missing_len = fec.length_recovery;
    std::uint8_t  missing_pt  = fec.pt_recovery;
    std::uint32_t missing_ts  = fec.ts_recovery;

    // Step 2: figure out the payload XOR aggregate, padded to max length.
    const std::size_t fec_payload_len = fec_pkt.size() - kRtpHeaderSize - kFecHeaderSize;
    std::vector<std::uint8_t> recovered_payload(
        fec_pkt.begin() + kRtpHeaderSize + kFecHeaderSize,
        fec_pkt.end());

    for (const auto& m : surviving) {
        missing_len = static_cast<std::uint16_t>(missing_len ^ m.payload.size());
        missing_pt  = static_cast<std::uint8_t>(missing_pt  ^ (m.pt & 0x7F));
        missing_ts ^= m.ts;
        for (std::size_t b = 0; b < m.payload.size() && b < recovered_payload.size(); ++b) {
            recovered_payload[b] = static_cast<std::uint8_t>(
                recovered_payload[b] ^ m.payload[b]);
        }
    }

    // FEC payload is zero-padded to max(payload_len) — trim back to the
    // recovered length so the rebuilt packet is byte-identical to the input.
    EXPECT_LE(missing_len, fec_payload_len);
    recovered_payload.resize(missing_len);

    // Step 3: figure out the missing seq from SNBase + position. Caller knows
    // which surviving packets they have, so they know which slot is missing.
    // We deduce it from: SNBase (group's lowest seq) + offset * row_index.
    // For a column: offset=L, slots have seq SNBase + L*row_idx for row_idx
    // 0..D-1; the missing one is the only seq in that arithmetic series not
    // present among `surviving`.
    std::uint16_t missing_seq = 0;
    {
        const std::uint16_t snbase_low = static_cast<std::uint16_t>(fec.snbase & 0xFFFF);
        const std::uint8_t  offset     = fec.offset;
        const std::uint8_t  count      = fec.na;
        std::vector<std::uint16_t> have;
        have.reserve(surviving.size());
        for (const auto& m : surviving) have.push_back(m.seq);
        std::sort(have.begin(), have.end());
        for (std::uint8_t k = 0; k < count; ++k) {
            const std::uint16_t s = static_cast<std::uint16_t>(snbase_low + k * offset);
            if (!std::binary_search(have.begin(), have.end(), s)) {
                missing_seq = s;
                break;
            }
        }
    }

    // Step 4: stitch together the rebuilt RTP packet.
    std::vector<std::uint8_t> rebuilt(kRtpHeaderSize + missing_len);
    RtpHeaderFields h{};
    h.payload_type = missing_pt;
    h.sequence     = missing_seq;
    h.timestamp    = missing_ts;
    h.ssrc         = expected_media_ssrc;   // FEC doesn't carry media SSRC,
                                            // receiver knows it from session.
    writeRtpHeader(
        std::span<std::uint8_t, kRtpHeaderSize>(rebuilt.data(), kRtpHeaderSize),
        h);
    if (missing_len) {
        std::memcpy(rebuilt.data() + kRtpHeaderSize,
                    recovered_payload.data(), missing_len);
    }
    return rebuilt;
}

}  // namespace

// 1D, L=4, D=3 — drop one slot at (col=2, row=1) and recover via column 2.
TEST(FecRecovery, OneDColumnRecoversSingleLoss) {
    constexpr std::uint8_t L = 4, D = 3;
    Sink col, row;
    FecMatrix m(L, D, FecCfg::Mode::OneD, 96, 0xCAFE0001u, 0xCAFE0002u,
                [&](auto p) { col(p); }, [&](auto p) { row(p); });

    std::vector<MediaPacket> media;
    media.reserve(L * D);
    for (std::uint16_t i = 0; i < L * D; ++i) {
        MediaPacket p;
        p.seq     = static_cast<std::uint16_t>(40000 + i);
        p.ts      = 90000u + i * 3600u;
        p.pt      = 33;
        p.ssrc    = 0xABCDEF01u;
        p.payload = makePayload(p.seq, 64);
        media.push_back(p);

        auto bytes = p.bytes();
        m.feedRtp(std::span<const std::uint8_t>(bytes.data(), bytes.size()));
    }
    ASSERT_EQ(col.packets.size(), L);
    ASSERT_TRUE(row.packets.empty());

    constexpr std::uint8_t target_col = 2;
    constexpr std::uint8_t target_row = 1;
    const std::size_t target_idx = target_row * L + target_col;
    const auto& fec_pkt = col.packets[target_col];

    std::vector<MediaPacket> surviving;
    for (std::uint8_t r = 0; r < D; ++r) {
        if (r == target_row) continue;
        surviving.push_back(media[r * L + target_col]);
    }

    auto rebuilt = recoverFromXorFec(fec_pkt, surviving, 0xABCDEF01u, D);

    auto expected = media[target_idx].bytes();
    EXPECT_EQ(rebuilt, expected);
}

// 1D L=10, D=10, payload=188 (real TS-over-RTP single-packet case): drop one
// slot at col=7, row=4, recover via column 7. Exercises the L*D=100 boundary
// and full-188-byte payload.
TEST(FecRecovery, OneDLargeMatrixRecoversSingleLoss) {
    constexpr std::uint8_t L = 10, D = 10;
    Sink col, row;
    FecMatrix m(L, D, FecCfg::Mode::OneD, 96, 0x10u, 0x20u,
                [&](auto p) { col(p); }, [&](auto p) { row(p); });

    std::vector<MediaPacket> media;
    media.reserve(L * D);
    for (std::uint16_t i = 0; i < L * D; ++i) {
        MediaPacket p;
        p.seq     = static_cast<std::uint16_t>(0x1000 + i);
        p.ts      = 1'000'000u + i * 3600u;
        p.pt      = 33;
        p.ssrc    = 0xC0FFEE00u;
        p.payload = makePayload(p.seq, 188);
        media.push_back(p);
        auto bytes = p.bytes();
        m.feedRtp(std::span<const std::uint8_t>(bytes.data(), bytes.size()));
    }
    ASSERT_EQ(col.packets.size(), L);

    constexpr std::uint8_t target_col = 7;
    constexpr std::uint8_t target_row = 4;
    const std::size_t target_idx = target_row * L + target_col;
    std::vector<MediaPacket> surviving;
    for (std::uint8_t r = 0; r < D; ++r) {
        if (r == target_row) continue;
        surviving.push_back(media[r * L + target_col]);
    }
    auto rebuilt = recoverFromXorFec(col.packets[target_col], surviving, 0xC0FFEE00u, D);
    EXPECT_EQ(rebuilt, media[target_idx].bytes());
}

// 2D L=4 D=4 — drop one slot at (col=1, row=2), recover via row 2.
TEST(FecRecovery, TwoDRowRecoversSingleLoss) {
    constexpr std::uint8_t L = 4, D = 4;
    Sink col, row;
    FecMatrix m(L, D, FecCfg::Mode::TwoD, 96, 0xC1u, 0xC2u,
                [&](auto p) { col(p); }, [&](auto p) { row(p); });

    std::vector<MediaPacket> media;
    media.reserve(L * D);
    for (std::uint16_t i = 0; i < L * D; ++i) {
        MediaPacket p;
        p.seq     = static_cast<std::uint16_t>(20000 + i);
        p.ts      = 5000u + i * 100u;
        p.pt      = 33;
        p.ssrc    = 0xCC00CC00u;
        p.payload = makePayload(p.seq, 50);
        media.push_back(p);
        auto bytes = p.bytes();
        m.feedRtp(std::span<const std::uint8_t>(bytes.data(), bytes.size()));
    }
    ASSERT_EQ(row.packets.size(), D);
    ASSERT_EQ(col.packets.size(), L);

    constexpr std::uint8_t target_row = 2;
    constexpr std::uint8_t target_col = 1;
    const std::size_t target_idx = target_row * L + target_col;
    const auto& fec_pkt = row.packets[target_row];

    std::vector<MediaPacket> surviving;
    for (std::uint8_t c = 0; c < L; ++c) {
        if (c == target_col) continue;
        surviving.push_back(media[target_row * L + c]);
    }
    auto rebuilt = recoverFromXorFec(fec_pkt, surviving, 0xCC00CC00u, L);
    EXPECT_EQ(rebuilt, media[target_idx].bytes());
}

// 2D — when both row and column protect a slot, either one alone can recover.
// Drop slot (col=2, row=2), recover via column 2.
TEST(FecRecovery, TwoDColumnAlsoRecovers) {
    constexpr std::uint8_t L = 4, D = 4;
    Sink col, row;
    FecMatrix m(L, D, FecCfg::Mode::TwoD, 96, 0xC1u, 0xC2u,
                [&](auto p) { col(p); }, [&](auto p) { row(p); });
    std::vector<MediaPacket> media;
    for (std::uint16_t i = 0; i < L * D; ++i) {
        MediaPacket p;
        p.seq     = static_cast<std::uint16_t>(8000 + i);
        p.ts      = 333u + i * 11u;
        p.payload = makePayload(p.seq, 100);
        media.push_back(p);
        auto bytes = p.bytes();
        m.feedRtp(std::span<const std::uint8_t>(bytes.data(), bytes.size()));
    }
    ASSERT_EQ(col.packets.size(), L);

    constexpr std::uint8_t target_col = 2;
    constexpr std::uint8_t target_row = 2;
    std::vector<MediaPacket> surviving;
    for (std::uint8_t r = 0; r < D; ++r) {
        if (r == target_row) continue;
        surviving.push_back(media[r * L + target_col]);
    }
    auto rebuilt = recoverFromXorFec(col.packets[target_col], surviving, 0u, D);
    EXPECT_EQ(rebuilt, media[target_row * L + target_col].bytes());
}

// length_recovery is non-trivial when payloads differ. The receiver must be
// able to recover the exact original payload length even if the lost packet
// was longer or shorter than every survivor.
TEST(FecRecovery, VariablePayloadLengthRecovers) {
    constexpr std::uint8_t L = 4, D = 3;
    Sink col, row;
    FecMatrix m(L, D, FecCfg::Mode::OneD, 96, 1u, 2u,
                [&](auto p) { col(p); }, [&](auto p) { row(p); });

    std::array<std::size_t, L * D> lens = {
        // row 0:
        100, 50, 188, 47,
        // row 1:
        13, 200, 75, 188,
        // row 2:
        180, 188, 9, 156,
    };
    std::vector<MediaPacket> media;
    media.reserve(L * D);
    for (std::uint16_t i = 0; i < L * D; ++i) {
        MediaPacket p;
        p.seq     = static_cast<std::uint16_t>(60000 + i);
        p.ts      = 10u + i;
        p.payload = makePayload(p.seq, lens[i]);
        media.push_back(p);
        auto bytes = p.bytes();
        m.feedRtp(std::span<const std::uint8_t>(bytes.data(), bytes.size()));
    }
    ASSERT_EQ(col.packets.size(), L);

    // Drop the longest payload in column 1 (slot (col=1,row=1) = 200 bytes).
    constexpr std::uint8_t target_col = 1;
    constexpr std::uint8_t target_row = 1;
    std::vector<MediaPacket> surviving;
    for (std::uint8_t r = 0; r < D; ++r) {
        if (r == target_row) continue;
        surviving.push_back(media[r * L + target_col]);
    }
    auto rebuilt = recoverFromXorFec(col.packets[target_col], surviving, 0u, D);
    EXPECT_EQ(rebuilt, media[target_row * L + target_col].bytes());
}

// Sequence-number wrap: feed a matrix whose first seq is 0xFFF8 so SNBase
// crosses 0xFFFF inside the matrix. The encoder uses 16-bit SNBase only
// (ext=0); receiver-side tracking of the wrap is the receiver's job, but
// the seq deduction in our recovery harness still works because RTP seqs
// wrap modulo 2^16 and arithmetic SNBase + offset*k wraps the same way.
TEST(FecRecovery, SequenceWrapStillRecovers) {
    constexpr std::uint8_t L = 4, D = 3;
    Sink col, row;
    FecMatrix m(L, D, FecCfg::Mode::OneD, 96, 1u, 2u,
                [&](auto p) { col(p); }, [&](auto p) { row(p); });

    std::vector<MediaPacket> media;
    for (std::uint16_t i = 0; i < L * D; ++i) {
        MediaPacket p;
        p.seq     = static_cast<std::uint16_t>(0xFFF8 + i);  // wraps at i=8
        p.ts      = i;
        p.payload = makePayload(p.seq, 32);
        media.push_back(p);
        auto bytes = p.bytes();
        m.feedRtp(std::span<const std::uint8_t>(bytes.data(), bytes.size()));
    }
    ASSERT_EQ(col.packets.size(), L);

    // Drop slot in column 0, row 2 (post-wrap region).
    constexpr std::uint8_t target_col = 0;
    constexpr std::uint8_t target_row = 2;
    std::vector<MediaPacket> surviving;
    for (std::uint8_t r = 0; r < D; ++r) {
        if (r == target_row) continue;
        surviving.push_back(media[r * L + target_col]);
    }
    auto rebuilt = recoverFromXorFec(col.packets[target_col], surviving, 0u, D);
    EXPECT_EQ(rebuilt, media[target_row * L + target_col].bytes());
}

// SMPTE 2022-1 §5: The FEC packet's RTP timestamp = the timestamp of the
// first protected media packet. (NOT a XOR aggregate; that's ts_recovery in
// the FEC header.)
TEST(FecRecovery, FecRtpTimestampIsFirstProtected) {
    constexpr std::uint8_t L = 4, D = 3;
    Sink col, row;
    FecMatrix m(L, D, FecCfg::Mode::TwoD, 96, 1u, 2u,
                [&](auto p) { col(p); }, [&](auto p) { row(p); });

    std::vector<MediaPacket> media;
    for (std::uint16_t i = 0; i < L * D; ++i) {
        MediaPacket p;
        p.seq     = static_cast<std::uint16_t>(900 + i);
        p.ts      = 7000u + i * 1000u;
        p.payload = makePayload(p.seq, 16);
        media.push_back(p);
        auto bytes = p.bytes();
        m.feedRtp(std::span<const std::uint8_t>(bytes.data(), bytes.size()));
    }
    ASSERT_EQ(row.packets.size(), D);
    ASSERT_EQ(col.packets.size(), L);

    // Row 0 protects slots 0..L-1 → first ts = 7000.
    {
        RtpHeaderFields h;
        ASSERT_TRUE(readRtpHeader(
            std::span<const std::uint8_t>(row.packets[0].data(),
                                           row.packets[0].size()), h));
        EXPECT_EQ(h.timestamp, 7000u);
    }
    // Column 0 protects slots 0, L, 2L → first ts = 7000 (same).
    {
        RtpHeaderFields h;
        ASSERT_TRUE(readRtpHeader(
            std::span<const std::uint8_t>(col.packets[0].data(),
                                           col.packets[0].size()), h));
        EXPECT_EQ(h.timestamp, 7000u);
    }
    // Column 2 protects slots 2, L+2, 2L+2 → first ts = 7000 + 2*1000 = 9000.
    {
        RtpHeaderFields h;
        ASSERT_TRUE(readRtpHeader(
            std::span<const std::uint8_t>(col.packets[2].data(),
                                           col.packets[2].size()), h));
        EXPECT_EQ(h.timestamp, 9000u);
    }
}

// Pro-MPEG COP3 r2 §8 fixed bits in the FEC header.
TEST(FecRecovery, ColumnHeaderConformsToProMpegCop3) {
    constexpr std::uint8_t L = 5, D = 6;
    Sink col, row;
    FecMatrix m(L, D, FecCfg::Mode::OneD, 96, 1u, 2u,
                [&](auto p) { col(p); }, [&](auto p) { row(p); });
    for (std::uint16_t i = 0; i < L * D; ++i) {
        MediaPacket p;
        p.seq     = static_cast<std::uint16_t>(i);
        p.ts      = i;
        p.payload = makePayload(p.seq, 32);
        auto bytes = p.bytes();
        m.feedRtp(std::span<const std::uint8_t>(bytes.data(), bytes.size()));
    }
    ASSERT_EQ(col.packets.size(), L);
    for (const auto& p : col.packets) {
        FecHeaderFields f;
        ASSERT_TRUE(readFecHeader(
            std::span<const std::uint8_t>(p.data() + kRtpHeaderSize, kFecHeaderSize),
            f));
        EXPECT_TRUE(f.e_bit);          // E always 1 in COP3 r2
        EXPECT_FALSE(f.x_bit);         // X always 0
        EXPECT_FALSE(f.d_row);         // column FEC: D=0
        EXPECT_EQ(f.type,  0);         // XOR only
        EXPECT_EQ(f.index, 0);
        EXPECT_EQ(f.mask24, 0u);
        EXPECT_EQ(f.offset, L);
        EXPECT_EQ(f.na,     D);
    }
}

TEST(FecRecovery, RowHeaderConformsToProMpegCop3) {
    constexpr std::uint8_t L = 5, D = 6;
    Sink col, row;
    FecMatrix m(L, D, FecCfg::Mode::TwoD, 96, 1u, 2u,
                [&](auto p) { col(p); }, [&](auto p) { row(p); });
    for (std::uint16_t i = 0; i < L * D; ++i) {
        MediaPacket p;
        p.seq     = static_cast<std::uint16_t>(i);
        p.ts      = i;
        p.payload = makePayload(p.seq, 32);
        auto bytes = p.bytes();
        m.feedRtp(std::span<const std::uint8_t>(bytes.data(), bytes.size()));
    }
    ASSERT_EQ(row.packets.size(), D);
    for (const auto& p : row.packets) {
        FecHeaderFields f;
        ASSERT_TRUE(readFecHeader(
            std::span<const std::uint8_t>(p.data() + kRtpHeaderSize, kFecHeaderSize),
            f));
        EXPECT_TRUE(f.e_bit);
        EXPECT_FALSE(f.x_bit);
        EXPECT_TRUE(f.d_row);          // row FEC: D=1
        EXPECT_EQ(f.type,  0);
        EXPECT_EQ(f.index, 0);
        EXPECT_EQ(f.mask24, 0u);
        EXPECT_EQ(f.offset, 1);
        EXPECT_EQ(f.na,     L);
    }
}

// Random walk: 1D L=8 D=5, drop one packet per matrix at random column/row,
// and confirm recovery succeeds for 32 successive matrices. Catches off-by-one
// errors in column indexing across wraps.
TEST(FecRecovery, RandomLossEachMatrixRecovers) {
    constexpr std::uint8_t L = 8, D = 5;
    constexpr std::size_t  matrices = 32;
    constexpr std::size_t  cap = L * D;

    Sink col, row;
    FecMatrix m(L, D, FecCfg::Mode::OneD, 96, 1u, 2u,
                [&](auto p) { col(p); }, [&](auto p) { row(p); });

    std::mt19937 rng(0x5eed5eedu);
    std::uniform_int_distribution<std::uint8_t> col_dist(0, L - 1);
    std::uniform_int_distribution<std::uint8_t> row_dist(0, D - 1);

    for (std::size_t mi = 0; mi < matrices; ++mi) {
        std::vector<MediaPacket> media;
        media.reserve(cap);
        for (std::uint16_t i = 0; i < cap; ++i) {
            MediaPacket p;
            p.seq     = static_cast<std::uint16_t>(mi * cap + i);
            p.ts      = static_cast<std::uint32_t>(mi * 1'000'000 + i);
            p.payload = makePayload(p.seq,
                                    32 + (i % 16),  // varied lengths
                                    static_cast<std::uint32_t>(mi));
            media.push_back(p);
            auto bytes = p.bytes();
            m.feedRtp(std::span<const std::uint8_t>(bytes.data(), bytes.size()));
        }
        ASSERT_EQ(col.packets.size(), L * (mi + 1));

        const std::uint8_t target_col = col_dist(rng);
        const std::uint8_t target_row = row_dist(rng);

        std::vector<MediaPacket> surviving;
        for (std::uint8_t r = 0; r < D; ++r) {
            if (r == target_row) continue;
            surviving.push_back(media[r * L + target_col]);
        }
        const std::size_t fec_idx = mi * L + target_col;
        auto rebuilt = recoverFromXorFec(col.packets[fec_idx], surviving, 0u, D);
        EXPECT_EQ(rebuilt, media[target_row * L + target_col].bytes())
            << "matrix=" << mi
            << " col=" << static_cast<int>(target_col)
            << " row=" << static_cast<int>(target_row);
    }
}
