#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include "gateway/GatewayCfg.h"
#include "gateway/fec/FecEncoder.h"
#include "gateway/fec/FecHeader.h"
#include "gateway/fec/RtpPacket.h"

using namespace liveqx::gateway::fec;
using liveqx::gateway::FecCfg;

namespace {

constexpr std::size_t kTs = 188;

std::array<std::uint8_t, kTs> makeTs(std::uint8_t tag) {
    std::array<std::uint8_t, kTs> p{};
    p[0] = 0x47;
    p[4] = tag;
    return p;
}

struct Sink {
    std::vector<std::vector<std::uint8_t>> packets;
    void operator()(std::span<const std::uint8_t> p) {
        packets.emplace_back(p.begin(), p.end());
    }
};

FecCfg make1DCfg(std::uint8_t L, std::uint8_t D) {
    FecCfg c;
    c.enabled      = true;
    c.mode         = FecCfg::Mode::OneD;
    c.L            = L;
    c.D            = D;
    c.payload_type = 33;
    c.ts_per_rtp   = 7;
    return c;
}

FecCfg make2DCfg(std::uint8_t L, std::uint8_t D) {
    auto c = make1DCfg(L, D);
    c.mode = FecCfg::Mode::TwoD;
    return c;
}

}  // namespace

TEST(FecEncoder, RtpWrapMediaSinkAndNoColumnUntilFull) {
    auto cfg = make1DCfg(/*L=*/4, /*D=*/4);
    Sink media, col, row;
    FecEncoder enc(cfg, 0xAAu, 0xCCu, 0xDDu,
                   [&](std::span<const std::uint8_t> p) { media(p); },
                   [&](std::span<const std::uint8_t> p) { col(p);   },
                   [&](std::span<const std::uint8_t> p) { row(p);   });

    // Feed 7 TS = exactly one media RTP, no FEC yet (need L*D = 16 RTP).
    for (int i = 0; i < 7; ++i) {
        auto ts = makeTs(static_cast<std::uint8_t>(i));
        enc.feedTsPacket(std::span<const std::uint8_t>(ts.data(), ts.size()));
    }
    EXPECT_EQ(media.packets.size(), 1u);
    EXPECT_EQ(media.packets[0].size(), kRtpHeaderSize + 7 * kTs);
    EXPECT_TRUE(col.packets.empty());
    EXPECT_TRUE(row.packets.empty());
}

TEST(FecEncoder, OneDEmitsLColumnFecAfterFullMatrix) {
    constexpr std::uint8_t L = 4, D = 4;
    auto cfg = make1DCfg(L, D);
    Sink media, col, row;
    FecEncoder enc(cfg, 1u, 2u, 3u,
                   [&](std::span<const std::uint8_t> p) { media(p); },
                   [&](std::span<const std::uint8_t> p) { col(p);   },
                   [&](std::span<const std::uint8_t> p) { row(p);   });

    // Feed L*D = 16 media RTP datagrams = 16 * 7 = 112 TS packets.
    for (int i = 0; i < L * D * 7; ++i) {
        auto ts = makeTs(static_cast<std::uint8_t>(i));
        enc.feedTsPacket(std::span<const std::uint8_t>(ts.data(), ts.size()));
    }
    EXPECT_EQ(media.packets.size(), static_cast<std::size_t>(L * D));
    EXPECT_EQ(col.packets.size(),   L);
    EXPECT_EQ(row.packets.size(),   0u);
    EXPECT_EQ(enc.columnFecEmitted(), L);
    EXPECT_EQ(enc.rowFecEmitted(),    0u);
    EXPECT_EQ(enc.mediaRtpEmitted(),  static_cast<std::uint64_t>(L * D));
}

TEST(FecEncoder, TwoDEmitsRowAndColumn) {
    constexpr std::uint8_t L = 4, D = 4;
    auto cfg = make2DCfg(L, D);
    Sink media, col, row;
    FecEncoder enc(cfg, 1u, 2u, 3u,
                   [&](std::span<const std::uint8_t> p) { media(p); },
                   [&](std::span<const std::uint8_t> p) { col(p);   },
                   [&](std::span<const std::uint8_t> p) { row(p);   });

    for (int i = 0; i < L * D * 7; ++i) {
        auto ts = makeTs(static_cast<std::uint8_t>(i));
        enc.feedTsPacket(std::span<const std::uint8_t>(ts.data(), ts.size()));
    }
    EXPECT_EQ(media.packets.size(), static_cast<std::size_t>(L * D));
    EXPECT_EQ(col.packets.size(),   L);
    EXPECT_EQ(row.packets.size(),   D);
}

TEST(FecEncoder, MediaPtAndSsrcPropagate) {
    auto cfg = make1DCfg(/*L=*/2, /*D=*/4);
    cfg.payload_type = 96;
    Sink media, col, row;
    FecEncoder enc(cfg, 0xCAFEBABEu, 0xC0Fu, 0xD0Du,
                   [&](std::span<const std::uint8_t> p) { media(p); },
                   [&](std::span<const std::uint8_t> p) { col(p);   },
                   [&](std::span<const std::uint8_t> p) { row(p);   });

    auto ts = makeTs(0);
    for (int i = 0; i < 7; ++i)
        enc.feedTsPacket(std::span<const std::uint8_t>(ts.data(), ts.size()));

    ASSERT_EQ(media.packets.size(), 1u);
    RtpHeaderFields h;
    ASSERT_TRUE(readRtpHeader(
        std::span<const std::uint8_t>(media.packets[0].data(),
                                       media.packets[0].size()), h));
    EXPECT_EQ(h.payload_type, 96);
    EXPECT_EQ(h.ssrc,         0xCAFEBABEu);
}

TEST(FecEncoder, ColumnFecHeaderUsesIndependentSsrc) {
    auto cfg = make1DCfg(/*L=*/2, /*D=*/4);
    Sink media, col, row;
    FecEncoder enc(cfg, 1u, 0xC0Cu, 0xC0Cu,
                   [&](std::span<const std::uint8_t> p) { media(p); },
                   [&](std::span<const std::uint8_t> p) { col(p);   },
                   [&](std::span<const std::uint8_t> p) { row(p);   });

    for (int i = 0; i < 2 * 4 * 7; ++i) {
        auto ts = makeTs(static_cast<std::uint8_t>(i));
        enc.feedTsPacket(std::span<const std::uint8_t>(ts.data(), ts.size()));
    }
    ASSERT_EQ(col.packets.size(), 2u);

    RtpHeaderFields h;
    ASSERT_TRUE(readRtpHeader(
        std::span<const std::uint8_t>(col.packets[0].data(),
                                       col.packets[0].size()), h));
    EXPECT_EQ(h.ssrc, 0xC0Cu);
    EXPECT_EQ(h.payload_type, 33);
}

TEST(FecEncoder, FlushDrainsPartialMediaRtp) {
    auto cfg = make1DCfg(/*L=*/2, /*D=*/4);
    Sink media, col, row;
    FecEncoder enc(cfg, 1u, 2u, 3u,
                   [&](std::span<const std::uint8_t> p) { media(p); },
                   [&](std::span<const std::uint8_t> p) { col(p);   },
                   [&](std::span<const std::uint8_t> p) { row(p);   });

    auto ts = makeTs(0);
    enc.feedTsPacket(std::span<const std::uint8_t>(ts.data(), ts.size()));
    enc.feedTsPacket(std::span<const std::uint8_t>(ts.data(), ts.size()));
    EXPECT_TRUE(media.packets.empty());

    enc.flush();
    ASSERT_EQ(media.packets.size(), 1u);
    EXPECT_EQ(media.packets[0].size(), kRtpHeaderSize + 2 * kTs);
}
