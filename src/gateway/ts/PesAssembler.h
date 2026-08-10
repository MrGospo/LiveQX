#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include "gateway/ts/TsPacket.h"

namespace liveqx::gateway::ts {

// ─── PES assembly ────────────────────────────────────────────────────────────
//
// MPEG-TS carries Packetized Elementary Stream (PES) data on payload PIDs
// (anything that's not PAT/PMT/SDT/EIT/etc.). A PES packet can span many TS
// packets and is delimited by Payload Unit Start Indicator (PUSI=1) on the
// first TS packet of the new PES.
//
// Layout (ISO/IEC 13818-1 §2.4.3.6):
//   3 bytes:  packet_start_code_prefix = 0x000001
//   1 byte:   stream_id (0xC0..0xDF audio, 0xE0..0xEF video, plus padding/etc.)
//   2 bytes:  PES_packet_length (0 means "until next PUSI" for video)
//   1 byte:   flags1 ('10' + scrambling + priority + alignment + copyright + original)
//   1 byte:   flags2 (PTS_DTS_flags + ESCR + ES_rate + DSM_trick + copy + CRC + ext)
//   1 byte:   PES_header_data_length
//   N bytes:  optional PES header (PTS, DTS, ESCR, ...) — N = PES_header_data_length
//   ...:      ES payload
//
// Use case in fix40 A6: TranscodeGateway feeds every TS packet on the video
// or audio PID into one PesAssembler instance per stream. The assembler
// reassembles full PES packets, extracts PTS/DTS, and hands the ES bytes off
// to the decoder. The inverse direction (re-PES, re-TS-packetise) is handled
// by PesPacketizer (declared below).

struct PesPacket {
    std::uint8_t              stream_id = 0;
    std::optional<std::int64_t> pts_90khz;  // nullopt when PTS_DTS_flags == '00'
    std::optional<std::int64_t> dts_90khz;  // nullopt when only PTS is signalled
    std::vector<std::uint8_t>   es;         // elementary-stream bytes (no PES header)
};

class PesAssembler {
public:
    using Sink = std::function<void(PesPacket&&)>;

    // The sink is invoked exactly once per fully-assembled PES packet.
    // Construction without a sink is valid for tests that drain via flush().
    explicit PesAssembler(Sink sink) noexcept;
    PesAssembler() noexcept = default;

    void setSink(Sink sink) noexcept { sink_ = std::move(sink); }

    // Feed one TS packet that has already been classified to belong to this
    // assembler's PID. The caller is responsible for PID demultiplexing.
    //
    // PUSI=1 finalises any in-progress PES (emits via sink) and starts a new
    // one. PUSI=0 appends payload to the current buffer. CC discontinuities
    // drop the in-progress PES (it would be corrupt).
    void feed(TsPacketView pkt);

    // Force finalisation of any buffered PES. For unbounded video (PES length
    // 0 in the header) this is the only way to deliver the last packet on
    // shutdown / EOF.
    void flush();

    // Forget any in-progress state. Called on PID change or external reset.
    void reset() noexcept;

    // ── stats ──────────────────────────────────────────────────────────────
    std::uint64_t pesAssembled()       const noexcept { return pes_assembled_; }
    std::uint64_t ccDiscontinuities()  const noexcept { return cc_discontinuities_; }
    std::uint64_t malformedPes()       const noexcept { return malformed_pes_; }

private:
    Sink                       sink_;
    std::vector<std::uint8_t>  buffer_;
    bool                       collecting_ = false;
    std::optional<std::uint8_t> last_cc_;

    std::uint64_t pes_assembled_      = 0;
    std::uint64_t cc_discontinuities_ = 0;
    std::uint64_t malformed_pes_      = 0;

    void finalize();
};

// ─── PTS/DTS codec (5-byte big-endian with marker bits) ──────────────────────
//
// The PES timestamp encoding interleaves three marker '1' bits between the
// 33-bit timestamp's three fields. Centralised here so both the assembler and
// the inverse builder use the same primitive.

// Decode 33-bit PTS/DTS from 5 bytes. The leading 4-bit nibble (0010 for PTS,
// 0011 for PTS+DTS, 0001 for DTS) is NOT verified here — caller checks
// PTS_DTS_flags first.
std::int64_t decodePts33(const std::uint8_t* p) noexcept;

// Encode a 33-bit timestamp into the 5-byte form. The leading nibble is taken
// from `prefix` (typical values: 0x2 for PTS-only, 0x3 for PTS, 0x1 for DTS).
void encodePts33(std::uint8_t* out, std::int64_t pts_90khz, std::uint8_t prefix) noexcept;

// ─── PES → TS packetiser ─────────────────────────────────────────────────────
//
// Used by the encode path to wrap an output ES packet (with its PTS/DTS) into
// one or more TS packets on a target PID. The first TS packet has PUSI=1, and
// CC values come from the caller's per-PID counter. Adaptation field is added
// only when needed for PCR (caller passes a flag) or for end-of-PES padding.

class PesPacketizer {
public:
    using PacketSink = std::function<void(const std::uint8_t* pkt, std::size_t len)>;

    // pid: target output PID for these TS packets.
    // cc_state: 4-bit continuity counter (caller-owned, advanced per packet).
    PesPacketizer(std::uint16_t pid, std::uint8_t& cc_state, PacketSink sink) noexcept;

    // Wrap one PES packet (es bytes + optional PTS/DTS) into TS packets and
    // hand each 188-byte packet to the sink in order. Adaptation field is
    // added on the first packet when stuffing is required to align ES end
    // with packet boundary, and on the last packet to absorb any tail.
    void emit(const PesPacket& pes);

private:
    std::uint16_t  pid_;
    std::uint8_t&  cc_;
    PacketSink     sink_;
};

}  // namespace liveqx::gateway::ts
