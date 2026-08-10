#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include "gateway/GatewayCfg.h"
#include "gateway/fec/FecHeader.h"
#include "gateway/fec/FecMatrix.h"
#include "gateway/fec/RtpPacket.h"

using namespace liveqx::gateway::fec;
using liveqx::gateway::FecCfg;

namespace {

// Build a media RTP packet: 12-byte header + payload of size `len`. Payload
// bytes carry the packet's sequence number in every byte so XOR tests can
// reason about the contents deterministically.
std::vector<std::uint8_t> makeRtp(std::uint16_t seq,
                                  std::uint32_t ts,
                                  std::uint8_t  pt,
                                  std::uint32_t ssrc,
                                  std::size_t   payload_len,
                                  std::uint8_t  byte_value) {
    std::vector<std::uint8_t> pkt(kRtpHeaderSize + payload_len);
    RtpHeaderFields h{};
    h.payload_type = pt;
    h.sequence     = seq;
    h.timestamp    = ts;
    h.ssrc         = ssrc;
    writeRtpHeader(std::span<std::uint8_t, kRtpHeaderSize>(pkt.data(),
                                                            kRtpHeaderSize),
                   h);
    std::fill(pkt.begin() + kRtpHeaderSize, pkt.end(), byte_value);
    return pkt;
}

struct FecSink {
    std::vector<std::vector<std::uint8_t>> packets;
    void operator()(std::span<const std::uint8_t> p) {
        packets.emplace_back(p.begin(), p.end());
    }
};

}  // namespace

TEST(FecMatrix, OneDEmitsLColumnPacketsAfterFullMatrix) {
    constexpr std::uint8_t L = 4, D = 5;
    FecSink col, row;
    FecMatrix m(L, D, FecCfg::Mode::OneD, 96, 0xAAAA0001u, 0xBBBB0001u,
                [&](std::span<const std::uint8_t> p) { col(p); },
                [&](std::span<const std::uint8_t> p) { row(p); });

    for (std::uint16_t i = 0; i < L * D; ++i) {
        auto pkt = makeRtp(static_cast<std::uint16_t>(1000 + i),
                           90000u + i * 3600u,
                           33, 0x12345678u, 100, static_cast<std::uint8_t>(i));
        m.feedRtp(std::span<const std::uint8_t>(pkt.data(), pkt.size()));
    }

    EXPECT_EQ(col.packets.size(), L);
    EXPECT_TRUE(row.packets.empty());
    EXPECT_EQ(m.columnFecEmitted(), L);
    EXPECT_EQ(m.rowFecEmitted(),    0u);
}

TEST(FecMatrix, TwoDEmitsRowAndColumnPackets) {
    constexpr std::uint8_t L = 4, D = 5;
    FecSink col, row;
    FecMatrix m(L, D, FecCfg::Mode::TwoD, 96, 1u, 2u,
                [&](std::span<const std::uint8_t> p) { col(p); },
                [&](std::span<const std::uint8_t> p) { row(p); });

    for (std::uint16_t i = 0; i < L * D; ++i) {
        auto pkt = makeRtp(static_cast<std::uint16_t>(7000 + i),
                           1000u + i, 33, 1u, 50,
                           static_cast<std::uint8_t>(i));
        m.feedRtp(std::span<const std::uint8_t>(pkt.data(), pkt.size()));
    }

    EXPECT_EQ(row.packets.size(), D);
    EXPECT_EQ(col.packets.size(), L);
    EXPECT_EQ(m.rowFecEmitted(),    D);
    EXPECT_EQ(m.columnFecEmitted(), L);
}

TEST(FecMatrix, ColumnFecHeaderFields) {
    constexpr std::uint8_t L = 3, D = 4;
    FecSink col, row;
    FecMatrix m(L, D, FecCfg::Mode::OneD, 96, 0xC0u, 0xD0u,
                [&](std::span<const std::uint8_t> p) { col(p); },
                [&](std::span<const std::uint8_t> p) { row(p); });

    for (std::uint16_t i = 0; i < L * D; ++i) {
        auto pkt = makeRtp(static_cast<std::uint16_t>(2000 + i),
                           500u, 33, 7u, 20,
                           static_cast<std::uint8_t>(0xA0 + i));
        m.feedRtp(std::span<const std::uint8_t>(pkt.data(), pkt.size()));
    }

    ASSERT_EQ(col.packets.size(), L);

    // First column FEC protects slots 0, L, 2L, 3L → seqs 2000, 2003, 2006, 2009.
    const auto& p0 = col.packets[0];
    ASSERT_GE(p0.size(), kRtpHeaderSize + kFecHeaderSize);

    RtpHeaderFields rtp;
    ASSERT_TRUE(readRtpHeader(
        std::span<const std::uint8_t>(p0.data(), p0.size()), rtp));
    EXPECT_EQ(rtp.payload_type, 96);
    EXPECT_EQ(rtp.ssrc,         0xC0u);
    EXPECT_EQ(rtp.timestamp,    500u);

    FecHeaderFields fec;
    ASSERT_TRUE(readFecHeader(
        std::span<const std::uint8_t>(p0.data() + kRtpHeaderSize,
                                       kFecHeaderSize),
        fec));
    EXPECT_EQ(fec.snbase,   2000u);
    EXPECT_TRUE(fec.e_bit);
    EXPECT_FALSE(fec.x_bit);
    EXPECT_FALSE(fec.d_row);          // column
    EXPECT_EQ(fec.type,     0);
    EXPECT_EQ(fec.offset,   L);
    EXPECT_EQ(fec.na,       D);
    EXPECT_EQ(fec.length_recovery, 0u);   // four 20s XOR = 0
    EXPECT_EQ(fec.pt_recovery,     0u);   // four 33s XOR = 0
    EXPECT_EQ(fec.ts_recovery,     0u);   // four 500s XOR = 0
}

TEST(FecMatrix, RowFecHeaderFields) {
    constexpr std::uint8_t L = 3, D = 4;
    FecSink col, row;
    FecMatrix m(L, D, FecCfg::Mode::TwoD, 96, 1u, 2u,
                [&](std::span<const std::uint8_t> p) { col(p); },
                [&](std::span<const std::uint8_t> p) { row(p); });

    for (std::uint16_t i = 0; i < L * D; ++i) {
        auto pkt = makeRtp(static_cast<std::uint16_t>(2000 + i),
                           600u + i, 33, 7u, 30,
                           static_cast<std::uint8_t>(i));
        m.feedRtp(std::span<const std::uint8_t>(pkt.data(), pkt.size()));
    }

    ASSERT_EQ(row.packets.size(), D);

    // First row FEC protects slots 0..L-1 → seqs 2000, 2001, 2002.
    const auto& p0 = row.packets[0];
    ASSERT_GE(p0.size(), kRtpHeaderSize + kFecHeaderSize);

    FecHeaderFields fec;
    ASSERT_TRUE(readFecHeader(
        std::span<const std::uint8_t>(p0.data() + kRtpHeaderSize,
                                       kFecHeaderSize),
        fec));
    EXPECT_EQ(fec.snbase, 2000u);
    EXPECT_TRUE(fec.d_row);            // row
    EXPECT_EQ(fec.offset, 1u);
    EXPECT_EQ(fec.na,     L);
    // ts_recovery = 600 XOR 601 XOR 602
    EXPECT_EQ(fec.ts_recovery, 600u ^ 601u ^ 602u);
}

TEST(FecMatrix, XorRecoversMissingPayload) {
    // 1D, L=4, D=3: drop one packet from a column and rebuild it.
    constexpr std::uint8_t L = 4, D = 3;
    FecSink col, row;
    FecMatrix m(L, D, FecCfg::Mode::OneD, 96, 0xC0u, 0xD0u,
                [&](std::span<const std::uint8_t> p) { col(p); },
                [&](std::span<const std::uint8_t> p) { row(p); });

    constexpr std::size_t kPayload = 16;
    std::vector<std::vector<std::uint8_t>> media;
    media.reserve(L * D);
    for (std::uint16_t i = 0; i < L * D; ++i) {
        auto pkt = makeRtp(static_cast<std::uint16_t>(5000 + i),
                           1000u + i, 33, 9u, kPayload,
                           static_cast<std::uint8_t>(0x10 + i));
        media.push_back(pkt);
        m.feedRtp(std::span<const std::uint8_t>(pkt.data(), pkt.size()));
    }
    ASSERT_EQ(col.packets.size(), L);

    // Recover slot at column 1, row 0 (index 1) using column 1 FEC and the
    // remaining D-1 = 2 packets in column 1.
    const std::uint8_t target_col = 1;
    const std::uint8_t target_row = 0;
    const std::size_t target_slot = target_row * L + target_col;

    const auto& fec_pkt = col.packets[target_col];
    ASSERT_GE(fec_pkt.size(), kRtpHeaderSize + kFecHeaderSize + kPayload);

    std::vector<std::uint8_t> reconstructed(
        fec_pkt.begin() + kRtpHeaderSize + kFecHeaderSize,
        fec_pkt.end());

    for (std::uint8_t r = 0; r < D; ++r) {
        if (r == target_row) continue;
        const std::size_t idx = r * L + target_col;
        const auto& m_pkt = media[idx];
        ASSERT_EQ(m_pkt.size(), kRtpHeaderSize + kPayload);
        for (std::size_t b = 0; b < kPayload; ++b) {
            reconstructed[b] = static_cast<std::uint8_t>(
                reconstructed[b] ^ m_pkt[kRtpHeaderSize + b]);
        }
    }

    const auto& target_pkt = media[target_slot];
    for (std::size_t b = 0; b < kPayload; ++b) {
        EXPECT_EQ(reconstructed[b], target_pkt[kRtpHeaderSize + b]) << "byte " << b;
    }
}

TEST(FecMatrix, WrapsAndEmitsAgain) {
    constexpr std::uint8_t L = 2, D = 4;
    FecSink col, row;
    FecMatrix m(L, D, FecCfg::Mode::TwoD, 96, 1u, 2u,
                [&](std::span<const std::uint8_t> p) { col(p); },
                [&](std::span<const std::uint8_t> p) { row(p); });

    for (std::uint16_t i = 0; i < 2 * L * D; ++i) {
        auto pkt = makeRtp(static_cast<std::uint16_t>(i),
                           i, 33, 1u, 12,
                           static_cast<std::uint8_t>(i));
        m.feedRtp(std::span<const std::uint8_t>(pkt.data(), pkt.size()));
    }
    EXPECT_EQ(col.packets.size(), 2 * L);
    EXPECT_EQ(row.packets.size(), 2 * D);
    EXPECT_EQ(m.matrixFill(), 0u);
}

TEST(FecMatrix, ColumnAndRowSequenceIndependent) {
    constexpr std::uint8_t L = 2, D = 4;
    FecSink col, row;
    FecMatrix m(L, D, FecCfg::Mode::TwoD, 96, 1u, 2u,
                [&](std::span<const std::uint8_t> p) { col(p); },
                [&](std::span<const std::uint8_t> p) { row(p); });

    for (std::uint16_t i = 0; i < L * D; ++i) {
        auto pkt = makeRtp(i, i, 33, 1u, 8, static_cast<std::uint8_t>(i));
        m.feedRtp(std::span<const std::uint8_t>(pkt.data(), pkt.size()));
    }

    auto seqOf = [](const std::vector<std::uint8_t>& p) -> std::uint16_t {
        RtpHeaderFields h;
        readRtpHeader(std::span<const std::uint8_t>(p.data(), p.size()), h);
        return h.sequence;
    };
    ASSERT_EQ(col.packets.size(), L);
    ASSERT_EQ(row.packets.size(), D);
    EXPECT_EQ(seqOf(col.packets[0]), 0);
    EXPECT_EQ(seqOf(col.packets[1]), 1);
    EXPECT_EQ(seqOf(row.packets[0]), 0);
    EXPECT_EQ(seqOf(row.packets[1]), 1);
}

TEST(FecMatrix, FillCounterTracksProgress) {
    constexpr std::uint8_t L = 3, D = 3;
    FecSink col, row;
    FecMatrix m(L, D, FecCfg::Mode::OneD, 96, 1u, 2u,
                [&](std::span<const std::uint8_t> p) { col(p); },
                [&](std::span<const std::uint8_t> p) { row(p); });

    EXPECT_EQ(m.matrixFill(), 0u);
    EXPECT_EQ(m.capacity(),   static_cast<std::size_t>(L * D));

    for (std::uint16_t i = 0; i < 5; ++i) {
        auto pkt = makeRtp(i, i, 33, 1u, 8, 0);
        m.feedRtp(std::span<const std::uint8_t>(pkt.data(), pkt.size()));
    }
    EXPECT_EQ(m.matrixFill(), 5u);
    EXPECT_TRUE(col.packets.empty());
}

TEST(FecMatrix, ResetMatrixClearsState) {
    constexpr std::uint8_t L = 3, D = 3;
    FecSink col, row;
    FecMatrix m(L, D, FecCfg::Mode::TwoD, 96, 1u, 2u,
                [&](std::span<const std::uint8_t> p) { col(p); },
                [&](std::span<const std::uint8_t> p) { row(p); });

    for (std::uint16_t i = 0; i < 4; ++i) {
        auto pkt = makeRtp(i, i, 33, 1u, 8, 0);
        m.feedRtp(std::span<const std::uint8_t>(pkt.data(), pkt.size()));
    }
    EXPECT_GT(m.matrixFill(), 0u);
    m.resetMatrix();
    EXPECT_EQ(m.matrixFill(), 0u);
}

TEST(FecMatrix, IgnoresShortPackets) {
    constexpr std::uint8_t L = 2, D = 4;
    FecSink col, row;
    FecMatrix m(L, D, FecCfg::Mode::OneD, 96, 1u, 2u,
                [&](std::span<const std::uint8_t> p) { col(p); },
                [&](std::span<const std::uint8_t> p) { row(p); });

    std::array<std::uint8_t, 4> bad{};
    m.feedRtp(std::span<const std::uint8_t>(bad.data(), bad.size()));
    EXPECT_EQ(m.matrixFill(), 0u);
}

TEST(FecMatrix, FecHeaderRoundTrips) {
    FecHeaderFields f;
    f.snbase          = 0x12'34'56;
    f.length_recovery = 0xABCD;
    f.e_bit           = true;
    f.pt_recovery     = 33;
    f.mask24          = 0;
    f.ts_recovery     = 0xDEADBEEFu;
    f.x_bit           = false;
    f.d_row           = true;
    f.type            = 0;
    f.index           = 0;
    f.offset          = 5;
    f.na              = 6;

    std::array<std::uint8_t, kFecHeaderSize> buf{};
    writeFecHeader(std::span<std::uint8_t, kFecHeaderSize>(buf), f);

    FecHeaderFields g;
    ASSERT_TRUE(readFecHeader(
        std::span<const std::uint8_t>(buf.data(), buf.size()), g));
    EXPECT_EQ(g.snbase,          f.snbase);
    EXPECT_EQ(g.length_recovery, f.length_recovery);
    EXPECT_EQ(g.e_bit,           f.e_bit);
    EXPECT_EQ(g.pt_recovery,     f.pt_recovery);
    EXPECT_EQ(g.ts_recovery,     f.ts_recovery);
    EXPECT_EQ(g.d_row,           f.d_row);
    EXPECT_EQ(g.offset,          f.offset);
    EXPECT_EQ(g.na,              f.na);
}
