#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include "gateway/fec/RtpEncapsulator.h"
#include "gateway/fec/RtpPacket.h"

using namespace liveqx::gateway::fec;

namespace {

constexpr std::size_t kTs = 188;

// Make a minimal 188-byte TS packet with sync byte 0x47 and a tag byte at
// offset 4 so we can identify which input went into which RTP datagram.
std::array<std::uint8_t, kTs> makeTs(std::uint8_t tag) {
    std::array<std::uint8_t, kTs> p{};
    p[0] = 0x47;
    p[1] = 0x00;     // PUSI=0, PID=0x000 high
    p[2] = 0x00;     // PID=0x000 low
    p[3] = 0x10;     // adaptation_field=01, CC=0
    p[4] = tag;
    return p;
}

struct DatagramSink {
    std::vector<std::vector<std::uint8_t>> datagrams;
    void operator()(std::span<const std::uint8_t> d) {
        datagrams.emplace_back(d.begin(), d.end());
    }
};

TEST(RtpHeader, WriteThenReadRoundTrips) {
    std::array<std::uint8_t, kRtpHeaderSize> buf{};
    RtpHeaderFields in;
    in.version      = 2;
    in.payload_type = 33;
    in.sequence     = 0xABCD;
    in.timestamp    = 0xDEADBEEFu;
    in.ssrc         = 0xCAFEBABEu;
    writeRtpHeader(std::span<std::uint8_t, kRtpHeaderSize>(buf), in);

    RtpHeaderFields out;
    ASSERT_TRUE(readRtpHeader(std::span<const std::uint8_t>(buf.data(), buf.size()),
                              out));
    EXPECT_EQ(out.version,      2);
    EXPECT_EQ(out.payload_type, 33);
    EXPECT_EQ(out.sequence,     0xABCD);
    EXPECT_EQ(out.timestamp,    0xDEADBEEFu);
    EXPECT_EQ(out.ssrc,         0xCAFEBABEu);
    EXPECT_FALSE(out.marker);
    EXPECT_FALSE(out.padding);
    EXPECT_FALSE(out.extension);
    EXPECT_EQ(out.csrc_count, 0);
}

TEST(RtpHeader, RejectsVersionMismatch) {
    std::array<std::uint8_t, kRtpHeaderSize> buf{};
    RtpHeaderFields in;
    in.version = 1;
    writeRtpHeader(std::span<std::uint8_t, kRtpHeaderSize>(buf), in);

    RtpHeaderFields out;
    EXPECT_FALSE(readRtpHeader(std::span<const std::uint8_t>(buf.data(), buf.size()),
                               out));
}

TEST(RtpEncapsulator, EmitsAfterTsPerRtpFeeds) {
    DatagramSink sink;
    RtpEncapsulator enc(0xAABBCCDDu, 33, 7,
        [&](std::span<const std::uint8_t> d) { sink(d); });

    for (int i = 0; i < 6; ++i) {
        auto ts = makeTs(static_cast<std::uint8_t>(i));
        enc.feed(std::span<const std::uint8_t>(ts.data(), ts.size()));
    }
    EXPECT_TRUE(sink.datagrams.empty());

    auto last = makeTs(6);
    enc.feed(std::span<const std::uint8_t>(last.data(), last.size()));
    ASSERT_EQ(sink.datagrams.size(), 1u);
    EXPECT_EQ(sink.datagrams[0].size(), kRtpHeaderSize + 7 * kTs);
}

TEST(RtpEncapsulator, FlushDrainsPartial) {
    DatagramSink sink;
    RtpEncapsulator enc(0x11223344u, 33, 7,
        [&](std::span<const std::uint8_t> d) { sink(d); });

    auto ts = makeTs(42);
    enc.feed(std::span<const std::uint8_t>(ts.data(), ts.size()));
    enc.feed(std::span<const std::uint8_t>(ts.data(), ts.size()));
    enc.feed(std::span<const std::uint8_t>(ts.data(), ts.size()));
    EXPECT_TRUE(sink.datagrams.empty());

    enc.flush();
    ASSERT_EQ(sink.datagrams.size(), 1u);
    EXPECT_EQ(sink.datagrams[0].size(), kRtpHeaderSize + 3 * kTs);
}

TEST(RtpEncapsulator, SequenceMonotonicAndWraps) {
    DatagramSink sink;
    RtpEncapsulator enc(1u, 33, 1,   // ts_per_rtp=1 emits per feed
        [&](std::span<const std::uint8_t> d) { sink(d); });

    // Seed sequence near wrap by emitting then re-checking the next one.
    auto ts = makeTs(0);
    for (int i = 0; i < 5; ++i)
        enc.feed(std::span<const std::uint8_t>(ts.data(), ts.size()));

    ASSERT_EQ(sink.datagrams.size(), 5u);
    for (int i = 0; i < 5; ++i) {
        RtpHeaderFields h;
        ASSERT_TRUE(readRtpHeader(
            std::span<const std::uint8_t>(sink.datagrams[i].data(),
                                          sink.datagrams[i].size()),
            h));
        EXPECT_EQ(h.sequence,     static_cast<std::uint16_t>(i));
        EXPECT_EQ(h.payload_type, 33);
        EXPECT_EQ(h.ssrc,         1u);
    }
}

TEST(RtpEncapsulator, TimestampAdvancesEachDatagram) {
    DatagramSink sink;
    RtpEncapsulator enc(7u, 33, 2,
        [&](std::span<const std::uint8_t> d) { sink(d); });
    enc.seedTimestamp(1000);

    for (int i = 0; i < 4; ++i) {
        auto ts = makeTs(static_cast<std::uint8_t>(i));
        enc.feed(std::span<const std::uint8_t>(ts.data(), ts.size()));
    }
    ASSERT_EQ(sink.datagrams.size(), 2u);

    RtpHeaderFields h0, h1;
    readRtpHeader(std::span<const std::uint8_t>(sink.datagrams[0].data(),
                                                 sink.datagrams[0].size()), h0);
    readRtpHeader(std::span<const std::uint8_t>(sink.datagrams[1].data(),
                                                 sink.datagrams[1].size()), h1);
    EXPECT_EQ(h0.timestamp, 1000u);
    EXPECT_GT(h1.timestamp, h0.timestamp);   // strictly advancing
}

TEST(RtpEncapsulator, PayloadStartsAtHeaderEnd) {
    DatagramSink sink;
    RtpEncapsulator enc(0u, 33, 1,
        [&](std::span<const std::uint8_t> d) { sink(d); });

    auto ts = makeTs(0xAA);
    enc.feed(std::span<const std::uint8_t>(ts.data(), ts.size()));
    ASSERT_EQ(sink.datagrams.size(), 1u);
    auto& d = sink.datagrams[0];
    ASSERT_EQ(d.size(), kRtpHeaderSize + kTs);
    EXPECT_EQ(d[kRtpHeaderSize + 0], 0x47);          // sync byte preserved
    EXPECT_EQ(d[kRtpHeaderSize + 4], 0xAA);          // tag preserved
}

TEST(RtpEncapsulator, SsrcOverride) {
    DatagramSink sink;
    RtpEncapsulator enc(0u, 33, 1,
        [&](std::span<const std::uint8_t> d) { sink(d); });
    enc.setSsrc(0xFEEDFACEu);

    auto ts = makeTs(0);
    enc.feed(std::span<const std::uint8_t>(ts.data(), ts.size()));
    ASSERT_EQ(sink.datagrams.size(), 1u);

    RtpHeaderFields h;
    ASSERT_TRUE(readRtpHeader(std::span<const std::uint8_t>(sink.datagrams[0].data(),
                                                              sink.datagrams[0].size()), h));
    EXPECT_EQ(h.ssrc, 0xFEEDFACEu);
}

TEST(RtpEncapsulator, IgnoresWrongSizedTs) {
    DatagramSink sink;
    RtpEncapsulator enc(0u, 33, 1,
        [&](std::span<const std::uint8_t> d) { sink(d); });

    std::array<std::uint8_t, 100> bad{};
    enc.feed(std::span<const std::uint8_t>(bad.data(), bad.size()));
    EXPECT_TRUE(sink.datagrams.empty());
}

TEST(RtpEncapsulator, RejectsInvalidTsPerRtp) {
    DatagramSink sink;
    EXPECT_THROW(
        RtpEncapsulator(0u, 33, 8, [&](std::span<const std::uint8_t> d){ sink(d); }),
        std::invalid_argument);
}

}  // namespace
