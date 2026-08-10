#include "gateway/fec/FecMatrix.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include "gateway/fec/FecHeader.h"
#include "gateway/fec/RtpPacket.h"

namespace liveqx::gateway::fec {

FecMatrix::FecMatrix(std::uint8_t  L,
                     std::uint8_t  D,
                     FecCfg::Mode  mode,
                     std::uint8_t  fec_payload_type,
                     std::uint32_t fec_column_ssrc,
                     std::uint32_t fec_row_ssrc,
                     PacketSink    column_sink,
                     PacketSink    row_sink) noexcept
    : L_(L)
    , D_(D)
    , mode_(mode)
    , fec_pt_(fec_payload_type)
    , col_ssrc_(fec_column_ssrc)
    , row_ssrc_(fec_row_ssrc)
    , col_sink_(std::move(column_sink))
    , row_sink_(std::move(row_sink))
    , cap_(static_cast<std::size_t>(L) * static_cast<std::size_t>(D))
    , slots_(cap_)
{}

void FecMatrix::resetMatrix() noexcept {
    fill_ = 0;
    for (auto& s : slots_) {
        s.filled      = false;
        s.seq         = 0;
        s.ts          = 0;
        s.pt          = 0;
        s.payload_len = 0;
        s.payload.clear();
    }
}

void FecMatrix::feedRtp(std::span<const std::uint8_t> rtp_bytes) {
    if (rtp_bytes.size() < kRtpHeaderSize) return;

    RtpHeaderFields h{};
    if (!readRtpHeader(rtp_bytes, h)) return;

    // Ignore RTP extension / padding handling — Pro-MPEG COP3 r2 senders emit
    // plain RFC 2250 MP2T-over-RTP, so we only see the 12-byte fixed header.
    const std::size_t payload_offset = kRtpHeaderSize;
    const std::size_t payload_len    = rtp_bytes.size() - payload_offset;

    Slot& slot = slots_[fill_];
    slot.seq         = h.sequence;
    slot.ts          = h.timestamp;
    slot.pt          = h.payload_type;
    slot.payload_len = static_cast<std::uint16_t>(payload_len);
    slot.payload.assign(rtp_bytes.begin() + payload_offset, rtp_bytes.end());
    slot.filled      = true;

    const std::size_t old_fill = fill_;
    ++fill_;

    // 2D row emission: when we just placed the last column of a row.
    if (mode_ == FecCfg::Mode::TwoD) {
        const std::uint8_t col_in_row =
            static_cast<std::uint8_t>(old_fill % L_);
        if (col_in_row == static_cast<std::uint8_t>(L_ - 1)) {
            const std::uint8_t row_idx =
                static_cast<std::uint8_t>(old_fill / L_);
            emitRow(row_idx);
        }
    }

    // Column emission: matrix full → flush L column-FEC packets, then wrap.
    if (fill_ == cap_) {
        for (std::uint8_t c = 0; c < L_; ++c) {
            emitColumn(c);
        }
        resetMatrix();
    }
}

void FecMatrix::emitRow(std::uint8_t row_idx) {
    std::vector<std::size_t> idxs(L_);
    for (std::uint8_t c = 0; c < L_; ++c) {
        idxs[c] = static_cast<std::size_t>(row_idx) * L_ + c;
    }
    const std::uint16_t snbase_low = slots_[idxs.front()].seq;
    const std::uint8_t  snbase_ext = 0;
    emitFec(/*is_row=*/true,
            snbase_low,
            snbase_ext,
            /*offset=*/1,
            /*na=*/L_,
            std::span<const std::size_t>(idxs.data(), idxs.size()),
            row_seq_,
            row_ssrc_,
            row_sink_);
    ++row_emitted_;
}

void FecMatrix::emitColumn(std::uint8_t col_idx) {
    std::vector<std::size_t> idxs(D_);
    for (std::uint8_t r = 0; r < D_; ++r) {
        idxs[r] = static_cast<std::size_t>(r) * L_ + col_idx;
    }
    const std::uint16_t snbase_low = slots_[idxs.front()].seq;
    const std::uint8_t  snbase_ext = 0;
    emitFec(/*is_row=*/false,
            snbase_low,
            snbase_ext,
            /*offset=*/L_,
            /*na=*/D_,
            std::span<const std::size_t>(idxs.data(), idxs.size()),
            col_seq_,
            col_ssrc_,
            col_sink_);
    ++column_emitted_;
}

void FecMatrix::emitFec(bool          is_row,
                        std::uint16_t snbase_low,
                        std::uint8_t  snbase_ext,
                        std::uint8_t  offset,
                        std::uint8_t  na,
                        std::span<const std::size_t> slot_indices,
                        std::uint16_t& seq_counter,
                        std::uint32_t  ssrc,
                        PacketSink&    sink) {
    // The protected payloads can differ in length; the receiver uses
    // length_recovery (XOR of media payload lengths) to reconstruct the
    // missing length. We zero-pad each shorter payload up to the maximum
    // before XOR'ing, so the FEC payload size = max(payload_len) across the
    // protected slots.
    std::size_t max_payload = 0;
    for (auto i : slot_indices) {
        max_payload = std::max(max_payload, slots_[i].payload.size());
    }

    std::uint16_t length_recovery = 0;
    std::uint8_t  pt_recovery     = 0;
    std::uint32_t ts_recovery     = 0;
    std::vector<std::uint8_t> xor_payload(max_payload, 0);

    for (auto i : slot_indices) {
        const Slot& s = slots_[i];
        length_recovery = static_cast<std::uint16_t>(length_recovery ^ s.payload_len);
        pt_recovery     = static_cast<std::uint8_t>(pt_recovery ^ (s.pt & 0x7F));
        ts_recovery    ^= s.ts;
        for (std::size_t b = 0; b < s.payload.size(); ++b) {
            xor_payload[b] = static_cast<std::uint8_t>(
                xor_payload[b] ^ s.payload[b]);
        }
    }

    // RTP header for the FEC packet itself. SMPTE 2022-1 §5: FEC packet
    // timestamp = timestamp of the first protected media packet.
    RtpHeaderFields rtp{};
    rtp.payload_type = fec_pt_;
    rtp.sequence     = seq_counter++;
    rtp.timestamp    = slots_[slot_indices.front()].ts;
    rtp.ssrc         = ssrc;

    FecHeaderFields fec{};
    fec.snbase          =
        (static_cast<std::uint32_t>(snbase_ext) << 16) | snbase_low;
    fec.length_recovery = length_recovery;
    fec.e_bit           = true;
    fec.pt_recovery     = pt_recovery;
    fec.mask24          = 0;
    fec.ts_recovery     = ts_recovery;
    fec.x_bit           = false;
    fec.d_row           = is_row;
    fec.type            = 0;   // XOR
    fec.index           = 0;
    fec.offset          = offset;
    fec.na              = na;

    std::vector<std::uint8_t> pkt(
        kRtpHeaderSize + kFecHeaderSize + max_payload);
    writeRtpHeader(
        std::span<std::uint8_t, kRtpHeaderSize>(pkt.data(), kRtpHeaderSize),
        rtp);
    writeFecHeader(
        std::span<std::uint8_t, kFecHeaderSize>(pkt.data() + kRtpHeaderSize,
                                                kFecHeaderSize),
        fec);
    if (max_payload != 0) {
        std::memcpy(pkt.data() + kRtpHeaderSize + kFecHeaderSize,
                    xor_payload.data(), max_payload);
    }
    sink(std::span<const std::uint8_t>(pkt.data(), pkt.size()));
}

}  // namespace liveqx::gateway::fec
