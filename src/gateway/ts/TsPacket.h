#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <span>

namespace liveqx::gateway::ts {

// MPEG-TS packet — fixed 188 bytes, ISO/IEC 13818-1 §2.4.3.2.
// Header layout (4 bytes):
//   byte 0:  sync_byte = 0x47
//   byte 1:  TEI(1) | PUSI(1) | priority(1) | PID_high(5)
//   byte 2:  PID_low(8)
//   byte 3:  scrambling(2) | adaptation_field_control(2) | continuity_counter(4)
// Bytes 4..187: payload, optionally prefixed by an adaptation field.
//
// We expose bit-level accessors as constexpr inline functions over a raw
// std::span<const std::uint8_t, 188> so the parser can be tested without
// owning the byte buffer (zero-copy over a TsRingBuffer slot).

inline constexpr std::size_t kTsPacketSize = 188;
inline constexpr std::uint8_t kTsSyncByte  = 0x47;

// Reserved PIDs from ISO/IEC 13818-1 §2.4.4 and ETSI EN 300 468.
inline constexpr std::uint16_t kPidPat  = 0x0000;  // Program Association Table
inline constexpr std::uint16_t kPidCat  = 0x0001;  // Conditional Access Table (out of scope; preserved as opaque)
inline constexpr std::uint16_t kPidTsdt = 0x0002;  // Transport Stream Description Table
inline constexpr std::uint16_t kPidNit  = 0x0010;  // DVB Network Information Table (default)
inline constexpr std::uint16_t kPidSdt  = 0x0011;  // DVB Service Description Table / BAT
inline constexpr std::uint16_t kPidEit  = 0x0012;  // DVB Event Information Table
inline constexpr std::uint16_t kPidNull = 0x1FFF;  // Null packet (stuffing)

// Adaptation-field control bits (header byte 3, mask 0x30).
enum class AdaptationFieldControl : std::uint8_t {
    Reserved        = 0b00,  // ISO reserves; treat as malformed
    PayloadOnly     = 0b01,
    AfOnly          = 0b10,
    AfThenPayload   = 0b11,
};

// Thin view over a 188-byte slot. Does not own memory. Construct from the
// IO-thread's recv buffer; copying the view is cheap.
class TsPacketView {
public:
    constexpr explicit TsPacketView(std::span<const std::uint8_t, kTsPacketSize> bytes) noexcept
        : bytes_(bytes) {}

    constexpr std::uint8_t syncByte() const noexcept { return bytes_[0]; }
    constexpr bool         isValidSync() const noexcept { return syncByte() == kTsSyncByte; }

    // Transport Error Indicator — set by demodulators on uncorrectable errors.
    // We forward TEI'd packets through passthrough but skip them in PSI parsing
    // (a corrupt PAT could mis-route).
    constexpr bool tei()  const noexcept { return (bytes_[1] & 0x80) != 0; }

    // Payload Unit Start Indicator — for PSI sections this means "first byte
    // of the payload is the pointer_field". For PES it means "the payload
    // starts at a PES header".
    constexpr bool pusi() const noexcept { return (bytes_[1] & 0x40) != 0; }

    constexpr bool priority() const noexcept { return (bytes_[1] & 0x20) != 0; }

    constexpr std::uint16_t pid() const noexcept {
        return static_cast<std::uint16_t>(((bytes_[1] & 0x1F) << 8) | bytes_[2]);
    }

    // Transport scrambling control — non-zero means the payload is encrypted
    // (DVB-CSA / AES). Out-of-scope for fix40 (CA dropped per scope).
    // Demux/remux pass scrambled payload through unchanged so that an
    // upstream CA descrambler can still operate; we just don't try to parse
    // its PSI semantics.
    constexpr std::uint8_t scrambling() const noexcept { return (bytes_[3] >> 6) & 0x03; }

    constexpr AdaptationFieldControl afControl() const noexcept {
        return static_cast<AdaptationFieldControl>((bytes_[3] >> 4) & 0x03);
    }
    constexpr bool hasAdaptationField() const noexcept {
        const auto a = afControl();
        return a == AdaptationFieldControl::AfOnly || a == AdaptationFieldControl::AfThenPayload;
    }
    constexpr bool hasPayload() const noexcept {
        const auto a = afControl();
        return a == AdaptationFieldControl::PayloadOnly || a == AdaptationFieldControl::AfThenPayload;
    }

    constexpr std::uint8_t continuityCounter() const noexcept { return bytes_[3] & 0x0F; }

    // Length of the adaptation field (excluding the length byte itself).
    // Returns 0 when no AF is present. The spec allows AF-length of 0 with
    // afControl == AfOnly (a single padding byte case).
    constexpr std::uint8_t adaptationFieldLength() const noexcept {
        return hasAdaptationField() ? bytes_[4] : 0;
    }

    // Offset where the payload begins (4 + AF length + AF length byte).
    // Returns kTsPacketSize when there is no payload (caller must check
    // hasPayload() first to avoid empty spans).
    constexpr std::size_t payloadOffset() const noexcept {
        if (!hasPayload()) return kTsPacketSize;
        if (!hasAdaptationField()) return 4;
        // +1 for the AF length byte itself.
        const std::size_t off = 5u + adaptationFieldLength();
        return off > kTsPacketSize ? kTsPacketSize : off;
    }

    // Payload as a span. Empty when no payload, or when AF length is malformed
    // (off >= kTsPacketSize). Caller does not need to bounds-check further.
    constexpr std::span<const std::uint8_t> payload() const noexcept {
        const auto off = payloadOffset();
        if (off >= kTsPacketSize) return {};
        return std::span<const std::uint8_t>(bytes_.data() + off, kTsPacketSize - off);
    }

    // PCR (Program Clock Reference) extraction. Returns std::nullopt when the
    // adaptation field doesn't carry PCR (most packets don't). The 33-bit base
    // is in 90 kHz units; the 9-bit extension is in 27 MHz. Combined into a
    // single 27-MHz uint64_t (base * 300 + ext) per §2.4.2.2.
    //
    // We use std::uint64_t rather than std::optional<std::uint64_t> + bool to
    // keep the hot path branchless when callers only need the timestamp; the
    // sentinel kPcrNone (UINT64_MAX) means "no PCR in this packet".
    static constexpr std::uint64_t kPcrNone = ~std::uint64_t{0};

    constexpr std::uint64_t pcr27Mhz() const noexcept {
        if (!hasAdaptationField()) return kPcrNone;
        const auto af_len = adaptationFieldLength();
        if (af_len < 7) return kPcrNone;                // need flags + 6 PCR bytes
        const std::uint8_t flags = bytes_[5];
        if ((flags & 0x10) == 0) return kPcrNone;       // PCR_flag bit 4
        // PCR occupies bytes 6..11: 33-bit base + 6 reserved bits + 9-bit ext.
        const auto& b = bytes_;
        const std::uint64_t base =
            (static_cast<std::uint64_t>(b[6])  << 25) |
            (static_cast<std::uint64_t>(b[7])  << 17) |
            (static_cast<std::uint64_t>(b[8])  <<  9) |
            (static_cast<std::uint64_t>(b[9])  <<  1) |
            (static_cast<std::uint64_t>(b[10]) >>  7);
        const std::uint16_t ext =
            static_cast<std::uint16_t>(((b[10] & 0x01) << 8) | b[11]);
        return base * 300u + ext;
    }

    // Discontinuity indicator (AF flag bit 7) — set by sources to signal
    // a clock or CC discontinuity. Important for restamping logic in fix-A5.
    constexpr bool discontinuityIndicator() const noexcept {
        if (!hasAdaptationField()) return false;
        if (adaptationFieldLength() < 1) return false;
        return (bytes_[5] & 0x80) != 0;
    }

    constexpr std::span<const std::uint8_t, kTsPacketSize> raw() const noexcept { return bytes_; }

private:
    std::span<const std::uint8_t, kTsPacketSize> bytes_;
};

// Mutable variant for packet rewriting in demux/remux (PID remap, CC rewrite,
// PCR restamp). Same layout, different access pattern.
class TsPacketMut {
public:
    constexpr explicit TsPacketMut(std::span<std::uint8_t, kTsPacketSize> bytes) noexcept
        : bytes_(bytes) {}

    constexpr TsPacketView view() const noexcept {
        return TsPacketView(std::span<const std::uint8_t, kTsPacketSize>(bytes_.data(), kTsPacketSize));
    }

    constexpr void setPid(std::uint16_t pid) noexcept {
        bytes_[1] = static_cast<std::uint8_t>((bytes_[1] & 0xE0) | ((pid >> 8) & 0x1F));
        bytes_[2] = static_cast<std::uint8_t>(pid & 0xFF);
    }

    constexpr void setContinuityCounter(std::uint8_t cc) noexcept {
        bytes_[3] = static_cast<std::uint8_t>((bytes_[3] & 0xF0) | (cc & 0x0F));
    }

    // Rewrite the PCR carried in this packet's adaptation field. Returns
    // false if the packet has no AF or the AF doesn't carry PCR (PCR_flag=0,
    // or AF too short) — caller must check before calling. The 27-MHz value
    // is split back into 33-bit base (90 kHz) + 9-bit extension (27 MHz).
    //
    // Used by PcrRestamper (fix-A5): replaces input-clock PCRs with
    // output-clock-domain PCRs derived from CLOCK_MONOTONIC + bytes-emitted
    // accounting so receivers see jitter-free pacing despite bursty input.
    constexpr bool setPcr27Mhz(std::uint64_t pcr_27mhz) noexcept {
        if (!hasAdaptationField()) return false;
        if (adaptationFieldLength() < 7) return false;
        if ((bytes_[5] & 0x10) == 0) return false;   // PCR_flag bit 4
        const std::uint64_t base = pcr_27mhz / 300u;
        const std::uint16_t ext  = static_cast<std::uint16_t>(pcr_27mhz % 300u);
        bytes_[6]  = static_cast<std::uint8_t>((base >> 25) & 0xFF);
        bytes_[7]  = static_cast<std::uint8_t>((base >> 17) & 0xFF);
        bytes_[8]  = static_cast<std::uint8_t>((base >>  9) & 0xFF);
        bytes_[9]  = static_cast<std::uint8_t>((base >>  1) & 0xFF);
        // byte 10: low bit of base, then 6 reserved 1-bits, then high bit of ext
        bytes_[10] = static_cast<std::uint8_t>(((base & 0x01) << 7) | 0x7E |
                                               ((ext >> 8) & 0x01));
        bytes_[11] = static_cast<std::uint8_t>(ext & 0xFF);
        return true;
    }

    constexpr std::uint8_t adaptationFieldLength() const noexcept {
        // Mirror of TsPacketView::adaptationFieldLength for use within this
        // mutable view (we don't carry a base class to share).
        const auto afc = static_cast<AdaptationFieldControl>((bytes_[3] >> 4) & 0x03);
        if (afc != AdaptationFieldControl::AfOnly &&
            afc != AdaptationFieldControl::AfThenPayload) return 0;
        return bytes_[4];
    }

    constexpr bool hasAdaptationField() const noexcept {
        const auto afc = static_cast<AdaptationFieldControl>((bytes_[3] >> 4) & 0x03);
        return afc == AdaptationFieldControl::AfOnly ||
               afc == AdaptationFieldControl::AfThenPayload;
    }

    constexpr std::span<std::uint8_t, kTsPacketSize> raw() noexcept { return bytes_; }

private:
    std::span<std::uint8_t, kTsPacketSize> bytes_;
};

// Build a null packet (PID 0x1FFF, no payload, AF stuffing). Used by the
// remux bitrate scheduler to fill underruns.
inline std::array<std::uint8_t, kTsPacketSize> makeNullPacket() noexcept {
    std::array<std::uint8_t, kTsPacketSize> p{};
    p[0] = kTsSyncByte;
    // PID 0x1FFF, PUSI=0, TEI=0
    p[1] = 0x1F;
    p[2] = 0xFF;
    // AF only, CC=0
    p[3] = 0x20;
    // AF length covers the rest of the packet (183 bytes), AF flags = 0
    p[4] = 183;
    p[5] = 0x00;
    std::memset(p.data() + 6, 0xFF, kTsPacketSize - 6);  // stuffing bytes
    return p;
}

}  // namespace liveqx::gateway::ts
