#pragma once

#include <cstdint>
#include <span>

namespace liveqx::gateway::fec {

// SMPTE 2022-1 / Pro-MPEG COP3 r2 FEC header — 16 bytes that follow the
// 12-byte RTP header in every FEC packet emitted on the column / row port.
//
// Layout (RFC 2733 §3.3 + Pro-MPEG COP3 r2 §8):
//
//  0                   1                   2                   3
//  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |       SNBase low bits         |       length recovery         |
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |E| PT recovery |                  mask                         |
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |                          TS recovery                          |
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |X|D|type |index|    offset     |      NA       | SNBase ext    |
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//
// Field semantics (sender side, the only side we currently implement):
//   SNBase    : lowest sequence number in the recovery group (low 16 + high 8)
//   length    : XOR of "length of media packet payload" for all packets in
//               the group (in network byte order); recovery side uses this to
//               find the original payload length.
//   E         : 1 — extension/header-of-FEC-header always present in COP3 r2
//   PT recovery: XOR of media packets' payload type (low 7 bits)
//   mask      : 24 bits, set to 0 in COP3 r2 (legacy RFC 2733 unused)
//   TS recovery: XOR of media RTP timestamps
//   X         : 0 (no extension beyond this 16-byte header)
//   D         : 0 = column FEC, 1 = row FEC
//   type      : 0 = XOR (only mode COP3 supports)
//   index     : 0 (XOR)
//   offset    : column FEC = L; row FEC = 1
//   NA        : column FEC = D; row FEC = L
//   SNBase ext: high 8 bits of SNBase

inline constexpr std::size_t kFecHeaderSize = 16;

struct FecHeaderFields {
    std::uint32_t snbase            = 0;   // 24-bit value (low 16 + ext 8)
    std::uint16_t length_recovery   = 0;
    bool          e_bit             = true;   // always set in COP3 r2
    std::uint8_t  pt_recovery       = 0;      // 7 bits
    std::uint32_t mask24            = 0;      // 24 bits, 0 in COP3 r2
    std::uint32_t ts_recovery       = 0;
    bool          x_bit             = false;  // 0 in COP3 r2
    bool          d_row             = false;  // 0 = column, 1 = row
    std::uint8_t  type              = 0;      // 3 bits, 0 = XOR
    std::uint8_t  index             = 0;      // 3 bits, 0
    std::uint8_t  offset            = 0;      // 8 bits
    std::uint8_t  na                = 0;      // 8 bits
};

inline void writeFecHeader(std::span<std::uint8_t, kFecHeaderSize> out,
                           const FecHeaderFields& f) noexcept {
    const std::uint16_t snbase_low = static_cast<std::uint16_t>(f.snbase & 0xFFFF);
    const std::uint8_t  snbase_ext = static_cast<std::uint8_t>((f.snbase >> 16) & 0xFF);

    out[0] = static_cast<std::uint8_t>(snbase_low >> 8);
    out[1] = static_cast<std::uint8_t>(snbase_low & 0xFF);
    out[2] = static_cast<std::uint8_t>(f.length_recovery >> 8);
    out[3] = static_cast<std::uint8_t>(f.length_recovery & 0xFF);
    out[4] = static_cast<std::uint8_t>(
        ((f.e_bit ? 1u : 0u) << 7) | (f.pt_recovery & 0x7F));
    out[5] = static_cast<std::uint8_t>((f.mask24 >> 16) & 0xFF);
    out[6] = static_cast<std::uint8_t>((f.mask24 >>  8) & 0xFF);
    out[7] = static_cast<std::uint8_t>( f.mask24        & 0xFF);
    out[8]  = static_cast<std::uint8_t>((f.ts_recovery >> 24) & 0xFF);
    out[9]  = static_cast<std::uint8_t>((f.ts_recovery >> 16) & 0xFF);
    out[10] = static_cast<std::uint8_t>((f.ts_recovery >>  8) & 0xFF);
    out[11] = static_cast<std::uint8_t>( f.ts_recovery        & 0xFF);
    out[12] = static_cast<std::uint8_t>(
        ((f.x_bit ? 1u : 0u) << 7)
      | ((f.d_row ? 1u : 0u) << 6)
      | ((f.type & 0x07) << 3)
      |  (f.index & 0x07));
    out[13] = f.offset;
    out[14] = f.na;
    out[15] = snbase_ext;
}

inline bool readFecHeader(std::span<const std::uint8_t> in,
                          FecHeaderFields& out) noexcept {
    if (in.size() < kFecHeaderSize) return false;
    const std::uint16_t snbase_low =
        static_cast<std::uint16_t>((in[0] << 8) | in[1]);
    const std::uint8_t  snbase_ext = in[15];
    out.snbase          = (static_cast<std::uint32_t>(snbase_ext) << 16) | snbase_low;
    out.length_recovery = static_cast<std::uint16_t>((in[2] << 8) | in[3]);
    out.e_bit           = (in[4] & 0x80) != 0;
    out.pt_recovery     = in[4] & 0x7F;
    out.mask24          = (static_cast<std::uint32_t>(in[5]) << 16)
                        | (static_cast<std::uint32_t>(in[6]) <<  8)
                        |  static_cast<std::uint32_t>(in[7]);
    out.ts_recovery     = (static_cast<std::uint32_t>(in[ 8]) << 24)
                        | (static_cast<std::uint32_t>(in[ 9]) << 16)
                        | (static_cast<std::uint32_t>(in[10]) <<  8)
                        |  static_cast<std::uint32_t>(in[11]);
    out.x_bit           = (in[12] & 0x80) != 0;
    out.d_row           = (in[12] & 0x40) != 0;
    out.type            = (in[12] >> 3) & 0x07;
    out.index           =  in[12]       & 0x07;
    out.offset          =  in[13];
    out.na              =  in[14];
    return true;
}

}  // namespace liveqx::gateway::fec
