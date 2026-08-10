#include <gtest/gtest.h>

#include <array>
#include <vector>

#include "gateway/ts/PcrRestamper.h"
#include "gateway/ts/TsPacket.h"

using namespace liveqx::gateway::ts;

namespace {

// Build a packet with PCR_flag set and a given PCR value.
std::array<std::uint8_t, kTsPacketSize>
makePcrPacket(std::uint16_t pid, std::uint64_t pcr_27mhz, bool discontinuity = false) {
    std::array<std::uint8_t, kTsPacketSize> p{};
    p[0] = kTsSyncByte;
    p[1] = static_cast<std::uint8_t>((pid >> 8) & 0x1F);
    p[2] = static_cast<std::uint8_t>(pid & 0xFF);
    // adaptation_field_only (no payload) — keeps the math simple.
    p[3] = 0x20;            // afc=0b10
    p[4] = 183;             // af_length: rest of packet
    p[5] = static_cast<std::uint8_t>((discontinuity ? 0x80 : 0x00) | 0x10);  // PCR_flag
    // Stamp PCR via the mutator so we exercise it round-trip-capable.
    TsPacketMut mut{std::span<std::uint8_t, kTsPacketSize>(p.data(), kTsPacketSize)};
    mut.setPcr27Mhz(pcr_27mhz);
    // Stuffing 0xFF in the rest of the AF.
    for (std::size_t i = 12; i < kTsPacketSize; ++i) p[i] = 0xFF;
    return p;
}

std::array<std::uint8_t, kTsPacketSize> makeEsPacket(std::uint16_t pid) {
    std::array<std::uint8_t, kTsPacketSize> p{};
    p[0] = kTsSyncByte;
    p[1] = static_cast<std::uint8_t>((pid >> 8) & 0x1F);
    p[2] = static_cast<std::uint8_t>(pid & 0xFF);
    p[3] = 0x10;            // payload only
    return p;
}

}  // namespace

TEST(PcrRestamperTest, AdvancesByteCounterPerPacket) {
    PcrRestamper r(10'000'000);
    auto p = makeEsPacket(0x100);
    TsPacketMut mut{std::span<std::uint8_t, kTsPacketSize>(p.data(), kTsPacketSize)};
    r.onPacketOut(mut, 0x100);
    r.onPacketOut(mut, 0x100);
    EXPECT_EQ(r.bytesEmitted(), 2u * kTsPacketSize);
}

TEST(PcrRestamperTest, IgnoresPidsNotRegistered) {
    PcrRestamper r(10'000'000);
    auto p = makePcrPacket(0x200, 12345);
    TsPacketMut mut{std::span<std::uint8_t, kTsPacketSize>(p.data(), kTsPacketSize)};
    EXPECT_FALSE(r.onPacketOut(mut, 0x200));
    // Original PCR untouched.
    EXPECT_EQ(mut.view().pcr27Mhz(), 12345u);
}

TEST(PcrRestamperTest, FirstPcrBecomesAnchorAndIsNotRewritten) {
    PcrRestamper r(10'000'000);
    r.registerPid(0x100);
    auto p = makePcrPacket(0x100, 5'000'000);
    TsPacketMut mut{std::span<std::uint8_t, kTsPacketSize>(p.data(), kTsPacketSize)};
    EXPECT_TRUE(r.onPacketOut(mut, 0x100));
    EXPECT_EQ(mut.view().pcr27Mhz(), 5'000'000u);                  // anchor unchanged
}

TEST(PcrRestamperTest, SubsequentPcrIsRewrittenForOutputBitrate) {
    constexpr std::uint32_t kBitrate = 10'000'000;                   // 10 Mbps
    PcrRestamper r(kBitrate);
    r.registerPid(0x100);

    // Anchor packet — input PCR=0, becomes output anchor.
    auto a = makePcrPacket(0x100, 0);
    TsPacketMut amut{std::span<std::uint8_t, kTsPacketSize>(a.data(), kTsPacketSize)};
    r.onPacketOut(amut, 0x100);

    // Send 99 ES packets without PCR — pure byte accounting.
    auto es = makeEsPacket(0x100);
    TsPacketMut emut{std::span<std::uint8_t, kTsPacketSize>(es.data(), kTsPacketSize)};
    for (int i = 0; i < 99; ++i) r.onPacketOut(emut, 0x100);

    // Now another PCR packet on this PID — input PCR is bogus (e.g. 999999999),
    // restamper must rewrite it based on bytes_emitted * 8 * 27e6 / bitrate.
    auto b = makePcrPacket(0x100, 999'999'999);
    TsPacketMut bmut{std::span<std::uint8_t, kTsPacketSize>(b.data(), kTsPacketSize)};
    r.onPacketOut(bmut, 0x100);

    // 100 packets of 188 bytes between anchor and this packet.
    constexpr std::uint64_t bytes = 100ull * kTsPacketSize;
    constexpr std::uint64_t expected = (bytes * 8ull * 27'000'000ull) / kBitrate;
    EXPECT_EQ(bmut.view().pcr27Mhz(), expected);
}

TEST(PcrRestamperTest, BitrateZeroPassesThroughInputPcr) {
    PcrRestamper r(0);
    r.registerPid(0x100);
    auto a = makePcrPacket(0x100, 1'000);
    TsPacketMut amut{std::span<std::uint8_t, kTsPacketSize>(a.data(), kTsPacketSize)};
    r.onPacketOut(amut, 0x100);

    auto b = makePcrPacket(0x100, 5'000);
    TsPacketMut bmut{std::span<std::uint8_t, kTsPacketSize>(b.data(), kTsPacketSize)};
    r.onPacketOut(bmut, 0x100);
    EXPECT_EQ(bmut.view().pcr27Mhz(), 5'000u);                       // unchanged
}

TEST(PcrRestamperTest, DiscontinuityReanchorsClock) {
    constexpr std::uint32_t kBitrate = 10'000'000;
    PcrRestamper r(kBitrate);
    r.registerPid(0x100);

    auto a = makePcrPacket(0x100, 1'000'000);
    TsPacketMut amut{std::span<std::uint8_t, kTsPacketSize>(a.data(), kTsPacketSize)};
    r.onPacketOut(amut, 0x100);

    // Burn 50 packets between the anchor and a discontinuity.
    auto es = makeEsPacket(0x100);
    TsPacketMut emut{std::span<std::uint8_t, kTsPacketSize>(es.data(), kTsPacketSize)};
    for (int i = 0; i < 50; ++i) r.onPacketOut(emut, 0x100);

    // Discontinuity-flagged PCR — re-anchor at value 9'000'000.
    auto disc = makePcrPacket(0x100, 9'000'000, /*discontinuity=*/true);
    TsPacketMut dmut{std::span<std::uint8_t, kTsPacketSize>(disc.data(), kTsPacketSize)};
    r.onPacketOut(dmut, 0x100);
    EXPECT_EQ(dmut.view().pcr27Mhz(), 9'000'000u);                  // anchor preserved

    // Subsequent PCR ticks from the new anchor.
    for (int i = 0; i < 99; ++i) r.onPacketOut(emut, 0x100);
    auto next = makePcrPacket(0x100, 99'999'999);
    TsPacketMut nmut{std::span<std::uint8_t, kTsPacketSize>(next.data(), kTsPacketSize)};
    r.onPacketOut(nmut, 0x100);
    constexpr std::uint64_t bytes_since_disc = 100ull * kTsPacketSize;
    constexpr std::uint64_t expected =
        9'000'000ull + (bytes_since_disc * 8ull * 27'000'000ull) / kBitrate;
    EXPECT_EQ(nmut.view().pcr27Mhz(), expected);
}

TEST(PcrRestamperTest, MultiplePidsTracksIndependently) {
    PcrRestamper r(10'000'000);
    r.registerPid(0x100);
    r.registerPid(0x200);

    auto a = makePcrPacket(0x100, 1'000);
    auto b = makePcrPacket(0x200, 99'999'999);                       // unrelated clock
    TsPacketMut amut{std::span<std::uint8_t, kTsPacketSize>(a.data(), kTsPacketSize)};
    TsPacketMut bmut{std::span<std::uint8_t, kTsPacketSize>(b.data(), kTsPacketSize)};
    r.onPacketOut(amut, 0x100);
    r.onPacketOut(bmut, 0x200);
    EXPECT_EQ(amut.view().pcr27Mhz(), 1'000u);                       // own anchor
    EXPECT_EQ(bmut.view().pcr27Mhz(), 99'999'999u);                  // own anchor
}

TEST(PcrRestamperTest, RegisterIsIdempotent) {
    PcrRestamper r(10'000'000);
    r.registerPid(0x100);
    r.registerPid(0x100);
    auto a = makePcrPacket(0x100, 555);
    TsPacketMut amut{std::span<std::uint8_t, kTsPacketSize>(a.data(), kTsPacketSize)};
    EXPECT_TRUE(r.onPacketOut(amut, 0x100));
    EXPECT_EQ(amut.view().pcr27Mhz(), 555u);
}

TEST(PcrRestamperTest, UnregisterStopsRestamping) {
    PcrRestamper r(10'000'000);
    r.registerPid(0x100);
    r.unregisterPid(0x100);
    auto p = makePcrPacket(0x100, 12345);
    TsPacketMut mut{std::span<std::uint8_t, kTsPacketSize>(p.data(), kTsPacketSize)};
    EXPECT_FALSE(r.onPacketOut(mut, 0x100));
    EXPECT_EQ(mut.view().pcr27Mhz(), 12345u);                        // untouched
}

TEST(PcrRestamperTest, ResetClearsState) {
    PcrRestamper r(10'000'000);
    r.registerPid(0x100);
    auto a = makePcrPacket(0x100, 1'000);
    TsPacketMut amut{std::span<std::uint8_t, kTsPacketSize>(a.data(), kTsPacketSize)};
    r.onPacketOut(amut, 0x100);
    r.reset();
    EXPECT_EQ(r.bytesEmitted(), 0u);
    EXPECT_FALSE(r.isTracked(0x100));                                // tracking cleared
}

TEST(PcrRestamperTest, NoPcrPacketDoesNotChangeAnchor) {
    PcrRestamper r(10'000'000);
    r.registerPid(0x100);
    auto a = makePcrPacket(0x100, 1'000);
    TsPacketMut amut{std::span<std::uint8_t, kTsPacketSize>(a.data(), kTsPacketSize)};
    r.onPacketOut(amut, 0x100);

    auto e = makeEsPacket(0x100);                                     // no PCR
    TsPacketMut emut{std::span<std::uint8_t, kTsPacketSize>(e.data(), kTsPacketSize)};
    EXPECT_FALSE(r.onPacketOut(emut, 0x100));                        // no PCR to rewrite
    // bytes accounted for, anchor unchanged.
    EXPECT_EQ(r.bytesEmitted(), 2u * kTsPacketSize);
}

TEST(PcrRestamperTest, CurrentPcrComputesLiveValue) {
    constexpr std::uint32_t kBitrate = 10'000'000;
    PcrRestamper r(kBitrate);
    r.registerPid(0x100);
    auto a = makePcrPacket(0x100, 0);
    TsPacketMut amut{std::span<std::uint8_t, kTsPacketSize>(a.data(), kTsPacketSize)};
    r.onPacketOut(amut, 0x100);
    auto e = makeEsPacket(0x100);
    TsPacketMut emut{std::span<std::uint8_t, kTsPacketSize>(e.data(), kTsPacketSize)};
    for (int i = 0; i < 50; ++i) r.onPacketOut(emut, 0x100);
    constexpr std::uint64_t bytes = 51ull * kTsPacketSize;
    constexpr std::uint64_t expected = (bytes * 8ull * 27'000'000ull) / kBitrate;
    EXPECT_EQ(r.currentPcr(0x100), expected);
}
