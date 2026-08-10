#include "gateway/fec/FecEncoder.h"

#include <utility>

#include "gateway/fec/FecMatrix.h"
#include "gateway/fec/RtpEncapsulator.h"

namespace liveqx::gateway::fec {

FecEncoder::FecEncoder(const FecCfg&  cfg,
                       std::uint32_t  media_ssrc,
                       std::uint32_t  column_ssrc,
                       std::uint32_t  row_ssrc,
                       PacketSink     media_sink,
                       PacketSink     column_sink,
                       PacketSink     row_sink)
    : cfg_(cfg)
    , media_sink_(std::move(media_sink))
{
    encap_ = std::make_unique<RtpEncapsulator>(
        media_ssrc,
        cfg.payload_type,
        cfg.ts_per_rtp,
        [this](std::span<const std::uint8_t> rtp) { onMediaRtp(rtp); });

    matrix_ = std::make_unique<FecMatrix>(
        cfg.L,
        cfg.D,
        cfg.mode,
        cfg.payload_type,
        column_ssrc,
        row_ssrc,
        std::move(column_sink),
        std::move(row_sink));
}

FecEncoder::~FecEncoder() = default;

void FecEncoder::feedTsPacket(std::span<const std::uint8_t> ts_packet) {
    encap_->feed(ts_packet);
}

void FecEncoder::flush() {
    encap_->flush();
}

void FecEncoder::onMediaRtp(std::span<const std::uint8_t> rtp) {
    // Forward the media RTP unchanged to the wire, then feed into the FEC
    // matrix so it contributes to the next column / row XOR group. Order
    // matters only insofar as the sink may not throw; our sinks are gateway
    // sendto callbacks that only log on error.
    if (media_sink_) media_sink_(rtp);
    ++media_emitted_;

    if (cfg_.enabled) {
        matrix_->feedRtp(rtp);
    }
}

std::uint64_t FecEncoder::columnFecEmitted() const noexcept {
    return matrix_->columnFecEmitted();
}

std::uint64_t FecEncoder::rowFecEmitted() const noexcept {
    return matrix_->rowFecEmitted();
}

void FecEncoder::resetMatrix() {
    matrix_->resetMatrix();
}

}  // namespace liveqx::gateway::fec
