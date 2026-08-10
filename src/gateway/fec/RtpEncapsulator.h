#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <vector>

#include "gateway/fec/RtpPacket.h"

namespace liveqx::gateway::fec {

// Encapsulate one or more 188-byte MPEG-TS packets into a single RTP/UDP
// payload per RFC 2250 §2 ("Encapsulation of MPEG TS in RTP").
//
// Per RFC 2250 §2:
//   - PT must be MP2T (33) when using the well-known mapping; we let the
//     operator override (FecCfg.payload_type) to interoperate with hardware
//     that pins a different PT.
//   - Marker bit MUST be 0 for continuous TS (RFC 2250 §3 reserves it as a
//     hint for the start of a media stream — we don't generate it).
//   - Timestamp is a 90 kHz clock; for sender flexibility RFC 2250 allows
//     either system clock or PCR-derived. We use a monotonic 90 kHz wall
//     clock anchored at start() — receivers care about delta, not absolute.
//   - 1..7 TS packets per RTP datagram; 7 is the canonical choice (1316-byte
//     payload + 12-byte RTP + 8-byte UDP + 20-byte IP = 1356-byte UDP
//     datagram, well under MTU 1500).
//
// The encapsulator is single-threaded — owned by the gateway IO thread.
class RtpEncapsulator {
public:
    // Sink receives a complete RTP packet as a contiguous byte sequence
    // (header + payload). The sink retains no reference past the call.
    using Sink = std::function<void(std::span<const std::uint8_t>)>;

    explicit RtpEncapsulator(std::uint32_t ssrc,
                             std::uint8_t  payload_type,
                             std::uint8_t  ts_per_rtp,
                             Sink          sink);

    // Push one 188-byte TS packet. The encapsulator buffers up to ts_per_rtp
    // and flushes a complete RTP packet to the sink. Partial buffers are
    // flushed by flush() (called at gateway stop, never mid-stream).
    void feed(std::span<const std::uint8_t> ts_packet);

    // Drain whatever is pending (1..ts_per_rtp-1 TS) into a final RTP packet.
    void flush();

    // Override the next emitted RTP timestamp. Used at start() to seed the
    // 90 kHz clock; otherwise advanced internally by ts_per_rtp count.
    void seedTimestamp(std::uint32_t ts) noexcept { timestamp_ = ts; }

    // Override the SSRC after construction (used by the gateway to randomise
    // SSRC at start when cfg.ssrc == 0).
    void setSsrc(std::uint32_t ssrc) noexcept { ssrc_ = ssrc; }

    std::uint16_t nextSequence() const noexcept { return sequence_; }
    std::uint32_t nextTimestamp() const noexcept { return timestamp_; }
    std::uint64_t packetsEmitted() const noexcept { return packets_emitted_; }

private:
    void emitDatagram();

    Sink          sink_;
    std::uint32_t ssrc_;
    std::uint8_t  payload_type_;
    std::uint8_t  ts_per_rtp_;

    std::uint16_t sequence_  = 0;
    std::uint32_t timestamp_ = 0;
    std::uint64_t packets_emitted_ = 0;

    // Pre-sized buffer: 12 byte RTP header + ts_per_rtp * 188.
    std::vector<std::uint8_t> buf_;
    std::size_t               ts_in_buf_ = 0;
};

}  // namespace liveqx::gateway::fec
