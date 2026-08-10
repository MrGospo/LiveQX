#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <span>

#include "gateway/GatewayCfg.h"

namespace liveqx::gateway::fec {

class RtpEncapsulator;
class FecMatrix;

// FecEncoder ties together RtpEncapsulator (1..7 TS → RTP) and FecMatrix
// (L*D media RTP → column / row XOR-FEC packets) into one entrypoint that
// the gateway feeds raw 188-byte TS packets into.
//
// Three sinks consume the wire bytes:
//
//   media_sink   ← media RTP datagrams           (port + 0)
//   column_sink  ← column FEC RTP datagrams      (port + 2)
//   row_sink     ← row FEC RTP datagrams         (port + 4, 2D only)
//
// The encoder owns no sockets; the caller wires sinks to whatever transport
// it likes (sendto on a UDP fd, an in-memory test sink, an OutputManager
// pump, …). FEC is enabled iff cfg.enabled — when disabled the encoder is
// pass-through-only on the media path (still RTP-wraps but never emits FEC),
// but typically the caller skips constructing it altogether in that case.
//
// The media RTP carries its own SSRC (typically the input flow's SSRC); the
// column / row FEC streams carry independent SSRCs and sequence spaces per
// SMPTE 2022-1.
class FecEncoder {
public:
    using PacketSink = std::function<void(std::span<const std::uint8_t>)>;

    FecEncoder(const FecCfg&  cfg,
               std::uint32_t  media_ssrc,
               std::uint32_t  column_ssrc,
               std::uint32_t  row_ssrc,
               PacketSink     media_sink,
               PacketSink     column_sink,
               PacketSink     row_sink);
    ~FecEncoder();

    FecEncoder(const FecEncoder&)            = delete;
    FecEncoder& operator=(const FecEncoder&) = delete;

    // Push one 188-byte TS packet. Internally batched to ts_per_rtp;
    // emits 0, 1, or many sink calls as appropriate.
    void feedTsPacket(std::span<const std::uint8_t> ts_packet);

    // Flush any partial RTP batch (end-of-stream, gateway shutdown). Does
    // not flush partial FEC matrix state — the receiver doesn't expect
    // partial recovery groups.
    void flush();

    // Per-stream stats.
    std::uint64_t mediaRtpEmitted()  const noexcept { return media_emitted_; }
    std::uint64_t columnFecEmitted() const noexcept;
    std::uint64_t rowFecEmitted()    const noexcept;

    // Test seam.
    void resetMatrix();

private:
    void onMediaRtp(std::span<const std::uint8_t> rtp);

    FecCfg                          cfg_;
    PacketSink                      media_sink_;
    std::unique_ptr<RtpEncapsulator> encap_;
    std::unique_ptr<FecMatrix>       matrix_;
    std::uint64_t                   media_emitted_ = 0;
};

}  // namespace liveqx::gateway::fec
