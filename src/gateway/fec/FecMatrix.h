#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <vector>

#include "gateway/GatewayCfg.h"
#include "gateway/fec/FecHeader.h"
#include "gateway/fec/RtpPacket.h"

namespace liveqx::gateway::fec {

// Pro-MPEG COP3 r2 / SMPTE 2022-1 (1D column) and SMPTE 2022-2 (2D row+col)
// FEC matrix builder.
//
// The matrix collects L*D media RTP packets in row-major order. When a row
// fills (k*L .. k*L+L-1) it emits one row-FEC packet (only when mode==TwoD).
// When the full L*D matrix fills it emits L column-FEC packets. After the
// final emission the matrix wraps and resumes filling.
//
// Recovery group composition (sender side):
//   column k FEC ← XOR over packets at slots k, k+L, k+2L, …, k+(D-1)*L
//   row k    FEC ← XOR over packets at slots k*L, k*L+1, …, k*L+L-1
//
// For each FEC packet the sender XORs:
//   - per-slot RTP header fields (PT, timestamp, sequence-base, length)
//     into the FEC header recovery fields,
//   - per-slot RTP payload bytes into the FEC payload.
//
// FEC packet wire format = 12-byte RTP header + 16-byte FEC header + payload.
// FEC RTP carries its own (column / row) sequence-number space, payload type
// and SSRC (cfg.payload_type / cfg.ssrc + a process-derived offset).
class FecMatrix {
public:
    using PacketSink = std::function<void(std::span<const std::uint8_t>)>;

    FecMatrix(std::uint8_t  L,
              std::uint8_t  D,
              FecCfg::Mode  mode,
              std::uint8_t  fec_payload_type,
              std::uint32_t fec_column_ssrc,
              std::uint32_t fec_row_ssrc,
              PacketSink    column_sink,
              PacketSink    row_sink) noexcept;

    // Feed a fully-formed media RTP packet. The matrix copies what it needs
    // from the bytes; the caller may free / reuse the buffer afterward.
    // Packets shorter than `kRtpHeaderSize` are silently dropped.
    void feedRtp(std::span<const std::uint8_t> rtp_bytes);

    // Stats / introspection.
    std::uint64_t columnFecEmitted() const noexcept { return column_emitted_; }
    std::uint64_t rowFecEmitted()    const noexcept { return row_emitted_;    }
    std::size_t   matrixFill()       const noexcept { return fill_; }
    std::size_t   capacity()         const noexcept { return cap_; }

    // Test seam: start a new FEC group. Called automatically when the matrix
    // wraps; tests use it to flush partial state without feeding L*D packets.
    void resetMatrix() noexcept;

private:
    struct Slot {
        std::uint16_t                   seq         = 0;
        std::uint32_t                   ts          = 0;
        std::uint8_t                    pt          = 0;
        std::uint16_t                   payload_len = 0;
        std::vector<std::uint8_t>       payload;
        bool                            filled      = false;
    };

    void emitRow(std::uint8_t row_idx);
    void emitColumn(std::uint8_t col_idx);
    void emitFec(bool          is_row,
                 std::uint16_t snbase_low,
                 std::uint8_t  snbase_ext,
                 std::uint8_t  offset,
                 std::uint8_t  na,
                 std::span<const std::size_t> slot_indices,
                 std::uint16_t& seq_counter,
                 std::uint32_t  ssrc,
                 PacketSink&    sink);

    std::uint8_t  L_;
    std::uint8_t  D_;
    FecCfg::Mode  mode_;
    std::uint8_t  fec_pt_;
    std::uint32_t col_ssrc_;
    std::uint32_t row_ssrc_;
    PacketSink    col_sink_;
    PacketSink    row_sink_;

    std::size_t   cap_ = 0;
    std::size_t   fill_ = 0;
    std::vector<Slot> slots_;

    std::uint16_t col_seq_ = 0;
    std::uint16_t row_seq_ = 0;

    std::uint64_t column_emitted_ = 0;
    std::uint64_t row_emitted_    = 0;
};

}  // namespace liveqx::gateway::fec
