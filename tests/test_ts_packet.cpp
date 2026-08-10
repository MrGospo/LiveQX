#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <thread>
#include <vector>

#include "gateway/ts/TsPacket.h"
#include "gateway/ts/TsRingBuffer.h"

using namespace liveqx::gateway::ts;

namespace {

// Build a synthetic TS packet header with the given fields. Payload is filled
// with `fill` bytes. Used as a fixture for parser tests.
std::array<std::uint8_t, kTsPacketSize> makePacket(std::uint16_t pid,
                                                   bool pusi,
                                                   std::uint8_t cc,
                                                   AdaptationFieldControl af,
                                                   std::uint8_t af_len = 0,
                                                   std::uint8_t fill = 0xAA) {
    std::array<std::uint8_t, kTsPacketSize> p{};
    p[0] = kTsSyncByte;
    p[1] = static_cast<std::uint8_t>(((pusi ? 1 : 0) << 6) | ((pid >> 8) & 0x1F));
    p[2] = static_cast<std::uint8_t>(pid & 0xFF);
    p[3] = static_cast<std::uint8_t>((static_cast<std::uint8_t>(af) << 4) | (cc & 0x0F));
    std::size_t off = 4;
    if (af == AdaptationFieldControl::AfOnly || af == AdaptationFieldControl::AfThenPayload) {
        p[4] = af_len;
        off = 5 + af_len;
        // Zero AF body — caller can override fields after.
        if (af_len > 0) p[5] = 0;  // flags = 0
    }
    for (std::size_t i = off; i < kTsPacketSize; ++i) p[i] = fill;
    return p;
}

}  // namespace

TEST(TsPacketTest, SyncByteAndPid) {
    auto p = makePacket(0x1234, /*pusi=*/true, /*cc=*/5,
                        AdaptationFieldControl::PayloadOnly);
    TsPacketView v{p};
    EXPECT_TRUE(v.isValidSync());
    EXPECT_EQ(v.pid(), 0x1234);
    EXPECT_TRUE(v.pusi());
    EXPECT_FALSE(v.tei());
    EXPECT_EQ(v.continuityCounter(), 5);
    EXPECT_EQ(v.afControl(), AdaptationFieldControl::PayloadOnly);
    EXPECT_TRUE(v.hasPayload());
    EXPECT_FALSE(v.hasAdaptationField());
    EXPECT_EQ(v.payloadOffset(), 4u);
    EXPECT_EQ(v.payload().size(), kTsPacketSize - 4);
}

TEST(TsPacketTest, ReservedPidConstants) {
    EXPECT_EQ(kPidPat, 0x0000);
    EXPECT_EQ(kPidSdt, 0x0011);
    EXPECT_EQ(kPidEit, 0x0012);
    EXPECT_EQ(kPidNull, 0x1FFF);
}

TEST(TsPacketTest, AdaptationFieldOnlyHasNoPayload) {
    auto p = makePacket(0x100, /*pusi=*/false, /*cc=*/0,
                        AdaptationFieldControl::AfOnly, /*af_len=*/183);
    TsPacketView v{p};
    EXPECT_TRUE(v.hasAdaptationField());
    EXPECT_FALSE(v.hasPayload());
    EXPECT_EQ(v.payloadOffset(), kTsPacketSize);
    EXPECT_TRUE(v.payload().empty());
}

TEST(TsPacketTest, AfThenPayloadOffsetRespectsAfLength) {
    auto p = makePacket(0x100, /*pusi=*/true, /*cc=*/0,
                        AdaptationFieldControl::AfThenPayload, /*af_len=*/10);
    TsPacketView v{p};
    EXPECT_TRUE(v.hasAdaptationField());
    EXPECT_TRUE(v.hasPayload());
    EXPECT_EQ(v.payloadOffset(), 4u + 1u + 10u);
    EXPECT_EQ(v.payload().size(), kTsPacketSize - (4u + 1u + 10u));
}

TEST(TsPacketTest, MalformedAfLengthClampsToPacketEnd) {
    // AF length 200 — bigger than the packet itself. payloadOffset() must
    // clamp at kTsPacketSize so we never produce a span past the buffer.
    auto p = makePacket(0x100, /*pusi=*/false, /*cc=*/0,
                        AdaptationFieldControl::AfThenPayload, /*af_len=*/200);
    TsPacketView v{p};
    EXPECT_EQ(v.payloadOffset(), kTsPacketSize);
    EXPECT_TRUE(v.payload().empty());
}

TEST(TsPacketTest, PcrAbsentWhenNoAdaptationField) {
    auto p = makePacket(0x100, /*pusi=*/true, /*cc=*/0,
                        AdaptationFieldControl::PayloadOnly);
    TsPacketView v{p};
    EXPECT_EQ(v.pcr27Mhz(), TsPacketView::kPcrNone);
}

TEST(TsPacketTest, PcrAbsentWhenFlagOff) {
    auto p = makePacket(0x100, /*pusi=*/false, /*cc=*/0,
                        AdaptationFieldControl::AfOnly, /*af_len=*/7);
    p[5] = 0x00;  // PCR_flag bit 4 = 0
    TsPacketView v{p};
    EXPECT_EQ(v.pcr27Mhz(), TsPacketView::kPcrNone);
}

TEST(TsPacketTest, PcrExtractsBaseAndExtension) {
    // base = 0x1FFFFFFFF (33-bit max), ext = 0x12C (300 - 1)
    auto p = makePacket(0x100, /*pusi=*/false, /*cc=*/0,
                        AdaptationFieldControl::AfOnly, /*af_len=*/7);
    p[5] = 0x10;  // PCR_flag set
    // PCR base 33-bit = 0x1FFFFFFFF, layout:
    //   b6..b9 = base[32:1], b10 high bit = base[0]
    //   b10 low bit = ext[8], b11 = ext[7:0]
    // Encode base = 1, ext = 5 → packed: base bits at b6=0,b7=0,b8=0,b9=0,b10=0x80,b11=0x05
    p[6] = 0; p[7] = 0; p[8] = 0; p[9] = 0;
    p[10] = 0x80;  // base LSB=1, low bit (ext bit 8) = 0
    p[11] = 0x05;  // ext = 5
    TsPacketView v{p};
    const auto pcr = v.pcr27Mhz();
    ASSERT_NE(pcr, TsPacketView::kPcrNone);
    EXPECT_EQ(pcr, std::uint64_t{1} * 300 + 5);
}

TEST(TsPacketTest, DiscontinuityIndicatorReadsAfFlag) {
    auto p = makePacket(0x100, /*pusi=*/false, /*cc=*/0,
                        AdaptationFieldControl::AfOnly, /*af_len=*/3);
    p[5] = 0x80;  // discontinuity_indicator
    TsPacketView v{p};
    EXPECT_TRUE(v.discontinuityIndicator());
    p[5] = 0x00;
    TsPacketView v2{p};
    EXPECT_FALSE(v2.discontinuityIndicator());
}

TEST(TsPacketTest, MutSetPidPreservesOtherBits) {
    auto p = makePacket(0x0123, /*pusi=*/true, /*cc=*/9,
                        AdaptationFieldControl::PayloadOnly);
    TsPacketMut mut{std::span<std::uint8_t, kTsPacketSize>(p.data(), kTsPacketSize)};
    mut.setPid(0x1ABC);
    auto view = mut.view();
    EXPECT_EQ(view.pid(), 0x1ABC);
    EXPECT_TRUE(view.pusi());
    EXPECT_EQ(view.continuityCounter(), 9);
}

TEST(TsPacketTest, MutSetCcPreservesAfControl) {
    auto p = makePacket(0x100, /*pusi=*/false, /*cc=*/0,
                        AdaptationFieldControl::AfThenPayload, /*af_len=*/2);
    TsPacketMut mut{std::span<std::uint8_t, kTsPacketSize>(p.data(), kTsPacketSize)};
    mut.setContinuityCounter(0x0F);
    auto view = mut.view();
    EXPECT_EQ(view.continuityCounter(), 0x0F);
    EXPECT_EQ(view.afControl(), AdaptationFieldControl::AfThenPayload);
}

TEST(TsPacketTest, MutSetPcr27MhzRoundTrips) {
    auto p = makePacket(0x100, /*pusi=*/false, /*cc=*/0,
                        AdaptationFieldControl::AfOnly, /*af_len=*/7);
    p[5] = 0x10;  // PCR_flag
    TsPacketMut mut{std::span<std::uint8_t, kTsPacketSize>(p.data(), kTsPacketSize)};
    // base must fit in 33 bits (max 0x1'FFFF'FFFF = 8589934591); ext in 9 bits.
    constexpr std::uint64_t kPcr = std::uint64_t{12345678} * 300 + 199;
    EXPECT_TRUE(mut.setPcr27Mhz(kPcr));
    auto view = mut.view();
    EXPECT_EQ(view.pcr27Mhz(), kPcr);
}

TEST(TsPacketTest, MutSetPcrFailsWhenFlagOff) {
    auto p = makePacket(0x100, /*pusi=*/false, /*cc=*/0,
                        AdaptationFieldControl::AfOnly, /*af_len=*/7);
    p[5] = 0x00;  // PCR_flag clear
    TsPacketMut mut{std::span<std::uint8_t, kTsPacketSize>(p.data(), kTsPacketSize)};
    EXPECT_FALSE(mut.setPcr27Mhz(42));
}

TEST(TsPacketTest, MutSetPcrFailsWhenNoAdaptationField) {
    auto p = makePacket(0x100, /*pusi=*/false, /*cc=*/0,
                        AdaptationFieldControl::PayloadOnly);
    TsPacketMut mut{std::span<std::uint8_t, kTsPacketSize>(p.data(), kTsPacketSize)};
    EXPECT_FALSE(mut.setPcr27Mhz(42));
}

TEST(TsPacketTest, MutSetPcrPreservesDiscontinuityFlag) {
    auto p = makePacket(0x100, /*pusi=*/false, /*cc=*/0,
                        AdaptationFieldControl::AfOnly, /*af_len=*/7);
    p[5] = 0x90;  // discontinuity + PCR_flag
    TsPacketMut mut{std::span<std::uint8_t, kTsPacketSize>(p.data(), kTsPacketSize)};
    EXPECT_TRUE(mut.setPcr27Mhz(123456));
    auto view = mut.view();
    EXPECT_TRUE(view.discontinuityIndicator());
    EXPECT_EQ(view.pcr27Mhz(), 123456u);
}

TEST(TsPacketTest, MakeNullPacketIsWellFormed) {
    auto null = makeNullPacket();
    TsPacketView v{null};
    EXPECT_TRUE(v.isValidSync());
    EXPECT_EQ(v.pid(), kPidNull);
    EXPECT_FALSE(v.hasPayload());
    EXPECT_TRUE(v.hasAdaptationField());
    EXPECT_EQ(v.adaptationFieldLength(), 183);
    // All stuffing bytes must be 0xFF.
    for (std::size_t i = 6; i < kTsPacketSize; ++i)
        EXPECT_EQ(null[i], 0xFFu);
}

// ─── TsRingBuffer ───────────────────────────────────────────────────────────

TEST(TsRingBufferTest, PushPopSinglePacket) {
    TsRingBuffer<8> ring;
    auto null = makeNullPacket();
    EXPECT_TRUE(ring.empty());
    EXPECT_TRUE(ring.push(null));
    EXPECT_FALSE(ring.empty());
    EXPECT_EQ(ring.sizeApprox(), 1u);

    std::array<std::uint8_t, kTsPacketSize> out{};
    ASSERT_TRUE(ring.pop(out));
    EXPECT_EQ(std::memcmp(out.data(), null.data(), kTsPacketSize), 0);
    EXPECT_TRUE(ring.empty());
}

TEST(TsRingBufferTest, TryPushReturnsFalseWhenFull) {
    TsRingBuffer<4> ring;
    auto null = makeNullPacket();
    // Capacity 4 → 3 usable slots (one is reserved for empty/full distinction).
    EXPECT_TRUE(ring.tryPush(null));
    EXPECT_TRUE(ring.tryPush(null));
    EXPECT_TRUE(ring.tryPush(null));
    EXPECT_FALSE(ring.tryPush(null));
    EXPECT_EQ(ring.droppedOldest(), 0u);
}

TEST(TsRingBufferTest, PushDropsOldestWhenFull) {
    TsRingBuffer<4> ring;
    // Fill 3 packets each tagged with a unique PID via byte 1 low bits.
    auto mk = [](std::uint16_t pid) {
        auto p = makeNullPacket();
        p[1] = static_cast<std::uint8_t>(0x00 | ((pid >> 8) & 0x1F));
        p[2] = static_cast<std::uint8_t>(pid & 0xFF);
        return p;
    };
    EXPECT_TRUE(ring.push(mk(1)));
    EXPECT_TRUE(ring.push(mk(2)));
    EXPECT_TRUE(ring.push(mk(3)));
    // 4th push fills the last slot AND drops the oldest (slot 0).
    EXPECT_FALSE(ring.push(mk(4)));
    EXPECT_EQ(ring.droppedOldest(), 1u);

    // After the drop, popping should yield 2,3,4 (1 was overwritten).
    std::array<std::uint8_t, kTsPacketSize> out{};
    ASSERT_TRUE(ring.pop(out));
    TsPacketView v1{out};
    EXPECT_EQ(v1.pid(), 2u);
    ASSERT_TRUE(ring.pop(out));
    TsPacketView v2{out};
    EXPECT_EQ(v2.pid(), 3u);
    ASSERT_TRUE(ring.pop(out));
    TsPacketView v3{out};
    EXPECT_EQ(v3.pid(), 4u);
}

TEST(TsRingBufferTest, ResetDroppedCounter) {
    TsRingBuffer<4> ring;
    auto null = makeNullPacket();
    ring.push(null); ring.push(null); ring.push(null); ring.push(null);
    EXPECT_GT(ring.droppedOldest(), 0u);
    ring.resetDroppedCounter();
    EXPECT_EQ(ring.droppedOldest(), 0u);
}

TEST(TsRingBufferTest, PeekDoesNotAdvance) {
    TsRingBuffer<8> ring;
    auto null = makeNullPacket();
    ring.push(null);
    TsPacketView v{std::span<const std::uint8_t, kTsPacketSize>(null.data(), kTsPacketSize)};
    EXPECT_TRUE(ring.peek(v));
    EXPECT_EQ(v.pid(), kPidNull);
    EXPECT_FALSE(ring.empty());
    ring.peekAdvance();
    EXPECT_TRUE(ring.empty());
}

TEST(TsRingBufferTest, ProducerConsumerThreaded) {
    TsRingBuffer<1024> ring;
    constexpr std::size_t N = 10000;
    std::atomic<bool> stop{false};

    auto mk = [](std::uint32_t seq) {
        auto p = makeNullPacket();
        // Encode seq into bytes 6..9 (inside AF stuffing area we ignore).
        p[6] = static_cast<std::uint8_t>(seq >> 24);
        p[7] = static_cast<std::uint8_t>(seq >> 16);
        p[8] = static_cast<std::uint8_t>(seq >> 8);
        p[9] = static_cast<std::uint8_t>(seq);
        return p;
    };

    std::thread producer([&] {
        for (std::uint32_t i = 0; i < N; ++i) {
            while (!ring.tryPush(mk(i))) {
                std::this_thread::yield();
                if (stop.load()) return;
            }
        }
    });

    std::vector<std::uint32_t> received;
    received.reserve(N);
    std::array<std::uint8_t, kTsPacketSize> out{};
    while (received.size() < N) {
        if (!ring.pop(out)) {
            std::this_thread::yield();
            continue;
        }
        std::uint32_t seq = (std::uint32_t(out[6]) << 24) |
                            (std::uint32_t(out[7]) << 16) |
                            (std::uint32_t(out[8]) << 8)  |
                             std::uint32_t(out[9]);
        received.push_back(seq);
    }
    stop.store(true);
    producer.join();

    ASSERT_EQ(received.size(), N);
    for (std::size_t i = 0; i < N; ++i) EXPECT_EQ(received[i], i);
}
