#pragma once

#include <cstdint>
#include <cstring>
#include <span>

namespace liveqx::gateway::fec {

// RFC 3550 §5.1 RTP fixed header — 12 bytes for the version-2, no-CSRC,
// no-extension case (the only variant we emit on TS-over-RTP per RFC 2250).
//
//  0                   1                   2                   3
//  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |V=2|P|X|  CC   |M|     PT      |       sequence number         |
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |                           timestamp                           |
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |           synchronization source (SSRC) identifier            |
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+

inline constexpr std::size_t kRtpHeaderSize = 12;
inline constexpr std::uint8_t kRtpVersion   = 2;

// RFC 3551 well-known PT for MP2T (MPEG-2 transport stream over RTP).
inline constexpr std::uint8_t kRtpPayloadTypeMp2t = 33;

struct RtpHeaderFields {
    std::uint8_t  version       = kRtpVersion;   // 2 bits
    bool          padding       = false;          // 1 bit
    bool          extension     = false;          // 1 bit
    std::uint8_t  csrc_count    = 0;              // 4 bits
    bool          marker        = false;          // 1 bit
    std::uint8_t  payload_type  = kRtpPayloadTypeMp2t;  // 7 bits
    std::uint16_t sequence      = 0;              // 16 bits
    std::uint32_t timestamp     = 0;              // 32 bits
    std::uint32_t ssrc          = 0;              // 32 bits
};

// Write the 12-byte fixed header into out[0..11]. out must have at least
// kRtpHeaderSize writable bytes.
inline void writeRtpHeader(std::span<std::uint8_t, kRtpHeaderSize> out,
                           const RtpHeaderFields& f) noexcept {
    out[0] = static_cast<std::uint8_t>(
        ((f.version & 0x03) << 6)
      | ((f.padding ? 1u : 0u) << 5)
      | ((f.extension ? 1u : 0u) << 4)
      |  (f.csrc_count & 0x0F));
    out[1] = static_cast<std::uint8_t>(
        ((f.marker ? 1u : 0u) << 7)
      |  (f.payload_type & 0x7F));
    out[2] = static_cast<std::uint8_t>(f.sequence >> 8);
    out[3] = static_cast<std::uint8_t>(f.sequence & 0xFF);
    out[4] = static_cast<std::uint8_t>((f.timestamp >> 24) & 0xFF);
    out[5] = static_cast<std::uint8_t>((f.timestamp >> 16) & 0xFF);
    out[6] = static_cast<std::uint8_t>((f.timestamp >>  8) & 0xFF);
    out[7] = static_cast<std::uint8_t>((f.timestamp >>  0) & 0xFF);
    out[8] = static_cast<std::uint8_t>((f.ssrc      >> 24) & 0xFF);
    out[9] = static_cast<std::uint8_t>((f.ssrc      >> 16) & 0xFF);
    out[10] = static_cast<std::uint8_t>((f.ssrc     >>  8) & 0xFF);
    out[11] = static_cast<std::uint8_t>((f.ssrc     >>  0) & 0xFF);
}

// Parse the 12-byte fixed header. Returns false if version != 2 or CSRC
// count != 0 (which would extend the header beyond 12 bytes — out of scope
// for our emitter). For receivers we still want strict parsing, hence the
// check.
inline bool readRtpHeader(std::span<const std::uint8_t> in,
                          RtpHeaderFields& out) noexcept {
    if (in.size() < kRtpHeaderSize) return false;
    out.version      = (in[0] >> 6) & 0x03;
    out.padding      = (in[0] & 0x20) != 0;
    out.extension    = (in[0] & 0x10) != 0;
    out.csrc_count   =  in[0] & 0x0F;
    out.marker       = (in[1] & 0x80) != 0;
    out.payload_type =  in[1] & 0x7F;
    out.sequence     = static_cast<std::uint16_t>((in[2] << 8) | in[3]);
    out.timestamp    = (static_cast<std::uint32_t>(in[4]) << 24)
                     | (static_cast<std::uint32_t>(in[5]) << 16)
                     | (static_cast<std::uint32_t>(in[6]) <<  8)
                     | (static_cast<std::uint32_t>(in[7])      );
    out.ssrc         = (static_cast<std::uint32_t>(in[ 8]) << 24)
                     | (static_cast<std::uint32_t>(in[ 9]) << 16)
                     | (static_cast<std::uint32_t>(in[10]) <<  8)
                     | (static_cast<std::uint32_t>(in[11])      );
    return out.version == kRtpVersion && out.csrc_count == 0;
}

}  // namespace liveqx::gateway::fec
