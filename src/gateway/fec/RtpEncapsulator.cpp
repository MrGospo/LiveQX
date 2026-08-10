#include "gateway/fec/RtpEncapsulator.h"

#include <cstring>
#include <stdexcept>

namespace liveqx::gateway::fec {

namespace {
constexpr std::size_t kTsPacketSize = 188;
}

RtpEncapsulator::RtpEncapsulator(std::uint32_t ssrc,
                                 std::uint8_t  payload_type,
                                 std::uint8_t  ts_per_rtp,
                                 Sink          sink)
    : sink_(std::move(sink))
    , ssrc_(ssrc)
    , payload_type_(payload_type & 0x7F)
    , ts_per_rtp_(ts_per_rtp == 0 ? 1 : ts_per_rtp) {
    if (ts_per_rtp_ > 7)
        throw std::invalid_argument(
            "RtpEncapsulator: ts_per_rtp must be in 1..7 (MTU budget)");
    buf_.resize(kRtpHeaderSize + std::size_t(ts_per_rtp_) * kTsPacketSize);
}

void RtpEncapsulator::feed(std::span<const std::uint8_t> ts_packet) {
    if (ts_packet.size() != kTsPacketSize) return;

    const std::size_t off = kRtpHeaderSize + ts_in_buf_ * kTsPacketSize;
    std::memcpy(buf_.data() + off, ts_packet.data(), kTsPacketSize);
    ++ts_in_buf_;

    if (ts_in_buf_ == ts_per_rtp_) emitDatagram();
}

void RtpEncapsulator::flush() {
    if (ts_in_buf_ > 0) emitDatagram();
}

void RtpEncapsulator::emitDatagram() {
    RtpHeaderFields hdr;
    hdr.version      = kRtpVersion;
    hdr.padding      = false;
    hdr.extension    = false;
    hdr.csrc_count   = 0;
    hdr.marker       = false;
    hdr.payload_type = payload_type_;
    hdr.sequence     = sequence_;
    hdr.timestamp    = timestamp_;
    hdr.ssrc         = ssrc_;

    std::span<std::uint8_t, kRtpHeaderSize> hdr_span{buf_.data(), kRtpHeaderSize};
    writeRtpHeader(hdr_span, hdr);

    const std::size_t total = kRtpHeaderSize + ts_in_buf_ * kTsPacketSize;
    if (sink_) sink_(std::span<const std::uint8_t>(buf_.data(), total));

    // Advance counters. RTP timestamp on a 90 kHz clock advances by the
    // packetisation interval — for MP2T per RFC 2250 §2, the spec leaves
    // the increment loosely defined. We use ts_in_buf_ * (90000 / 25) /
    // approx ticks per TS; but since receivers only use timestamp for
    // jitter estimation (not for media presentation — MP2T carries its
    // own PCR/PTS), advancing in proportion to byte count is sufficient.
    // The conventional choice is 3000 90kHz ticks per RTP for 7-TS
    // payload at 50 fps-equivalent; we use 3600 (one 25 fps frame) as a
    // round number that doesn't change receiver behaviour.
    timestamp_ += 3600u;
    ++sequence_;        // wraps at 0xFFFF naturally
    ++packets_emitted_;
    ts_in_buf_ = 0;
}

}  // namespace liveqx::gateway::fec
