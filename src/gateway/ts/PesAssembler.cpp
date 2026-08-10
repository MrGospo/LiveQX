#include "gateway/ts/PesAssembler.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace liveqx::gateway::ts {

namespace {

// Stream IDs for which PES carries the optional header (flags1/flags2/PES
// header data). Padding (0xBE) and private_stream_2 (0xBF) skip it. Everything
// in the audio (0xC0..0xDF) and video (0xE0..0xEF) range carries it, plus a
// few system streams. Per ISO/IEC 13818-1 §2.4.3.7.
constexpr bool streamIdHasOptionalHeader(std::uint8_t sid) noexcept {
    if (sid == 0xBC) return false;  // program_stream_map
    if (sid == 0xBE) return false;  // padding_stream
    if (sid == 0xBF) return false;  // private_stream_2
    if (sid == 0xF0) return false;  // ECM
    if (sid == 0xF1) return false;  // EMM
    if (sid == 0xFF) return false;  // program_stream_directory
    if (sid == 0xF2) return false;  // DSM-CC
    if (sid == 0xF8) return false;  // ITU-T Rec. H.222.1 type E
    return true;
}

}  // namespace

// ─── PTS/DTS codec ───────────────────────────────────────────────────────────

std::int64_t decodePts33(const std::uint8_t* p) noexcept {
    // Layout (big-endian, with three '1' marker bits interleaved):
    //   byte0: 4-bit prefix | PTS[32..30] | marker(1)
    //   byte1: PTS[29..22]
    //   byte2: PTS[21..15] | marker(1)
    //   byte3: PTS[14..7]
    //   byte4: PTS[6..0]   | marker(1)
    const std::uint64_t b0 = p[0], b1 = p[1], b2 = p[2], b3 = p[3], b4 = p[4];
    const std::uint64_t pts =
        (((b0 >> 1) & 0x07ULL) << 30) |
        ( b1                   << 22) |
        (((b2 >> 1) & 0x7FULL) << 15) |
        ( b3                   <<  7) |
        ( (b4 >> 1) & 0x7FULL);
    return static_cast<std::int64_t>(pts);
}

void encodePts33(std::uint8_t* out, std::int64_t pts_90khz, std::uint8_t prefix) noexcept {
    const std::uint64_t v = static_cast<std::uint64_t>(pts_90khz) & 0x1FFFFFFFFULL;  // 33 bits
    out[0] = static_cast<std::uint8_t>(((prefix & 0x0F) << 4) | (((v >> 30) & 0x07) << 1) | 0x01);
    out[1] = static_cast<std::uint8_t>((v >> 22) & 0xFF);
    out[2] = static_cast<std::uint8_t>((((v >> 15) & 0x7F) << 1) | 0x01);
    out[3] = static_cast<std::uint8_t>((v >> 7) & 0xFF);
    out[4] = static_cast<std::uint8_t>(((v & 0x7F) << 1) | 0x01);
}

// ─── PesAssembler ────────────────────────────────────────────────────────────

PesAssembler::PesAssembler(Sink sink) noexcept
    : sink_(std::move(sink)) {}

void PesAssembler::reset() noexcept {
    buffer_.clear();
    collecting_ = false;
    last_cc_.reset();
}

void PesAssembler::feed(TsPacketView pkt) {
    if (!pkt.isValidSync() || pkt.tei()) {
        // TEI-flagged packets are unreliable; drop the in-progress PES.
        if (collecting_) ++malformed_pes_;
        reset();
        return;
    }
    if (pkt.scrambling() != 0) {
        // Encrypted payload — out of scope. Drop quietly.
        reset();
        return;
    }

    // CC validation (only when packet has payload).
    if (pkt.hasPayload()) {
        const auto cc = pkt.continuityCounter();
        if (last_cc_.has_value()) {
            const auto expected = static_cast<std::uint8_t>((*last_cc_ + 1) & 0x0F);
            if (cc != expected) {
                ++cc_discontinuities_;
                reset();
                last_cc_ = cc;  // resync after drop
                // Fall through: PUSI=1 may still start a fresh PES below.
                if (!pkt.pusi()) return;
            } else {
                last_cc_ = cc;
            }
        } else {
            last_cc_ = cc;
        }
    }

    if (!pkt.hasPayload()) return;
    const auto payload = pkt.payload();
    if (payload.empty()) return;

    if (pkt.pusi()) {
        // New PES boundary — finalise any in-progress collection first.
        finalize();
        buffer_.assign(payload.begin(), payload.end());
        collecting_ = true;
    } else if (collecting_) {
        buffer_.insert(buffer_.end(), payload.begin(), payload.end());
    }
    // PUSI=0 with no in-progress collection means we joined mid-PES; ignore.
}

void PesAssembler::flush() {
    finalize();
    last_cc_.reset();
}

void PesAssembler::finalize() {
    if (!collecting_ || buffer_.size() < 6) {
        if (collecting_) ++malformed_pes_;
        buffer_.clear();
        collecting_ = false;
        return;
    }

    // Verify start code 0x000001
    if (buffer_[0] != 0x00 || buffer_[1] != 0x00 || buffer_[2] != 0x01) {
        ++malformed_pes_;
        buffer_.clear();
        collecting_ = false;
        return;
    }

    const std::uint8_t  stream_id  = buffer_[3];
    const std::uint16_t pkt_length = static_cast<std::uint16_t>((buffer_[4] << 8) | buffer_[5]);

    // Where does ES begin? For streams without optional header, ES starts at
    // offset 6. For streams with one, parse PTS/DTS and skip the header data.
    std::size_t es_offset = 6;
    std::optional<std::int64_t> pts;
    std::optional<std::int64_t> dts;

    if (streamIdHasOptionalHeader(stream_id)) {
        if (buffer_.size() < 9) {
            ++malformed_pes_;
            buffer_.clear();
            collecting_ = false;
            return;
        }
        // buffer_[6] = flags1 ('10' + ...)
        // buffer_[7] = flags2 (PTS_DTS_flags + ...)
        // buffer_[8] = PES_header_data_length
        const std::uint8_t flags2  = buffer_[7];
        const std::uint8_t hdr_len = buffer_[8];
        const std::size_t  opt_off = 9;
        const std::size_t  opt_end = opt_off + hdr_len;
        if (opt_end > buffer_.size()) {
            ++malformed_pes_;
            buffer_.clear();
            collecting_ = false;
            return;
        }
        const std::uint8_t pts_dts = (flags2 >> 6) & 0x03;
        if (pts_dts == 0b10) {  // PTS only
            if (hdr_len >= 5) {
                pts = decodePts33(&buffer_[opt_off]);
            }
        } else if (pts_dts == 0b11) {  // PTS + DTS
            if (hdr_len >= 10) {
                pts = decodePts33(&buffer_[opt_off]);
                dts = decodePts33(&buffer_[opt_off + 5]);
            }
        } else if (pts_dts == 0b01) {
            // Forbidden — but tolerate by treating as no timestamp.
        }
        es_offset = opt_end;
    }

    // Truncate to declared length when nonzero. Length is the number of bytes
    // following the 6-byte fixed header. Video commonly uses 0 (unbounded).
    std::size_t es_end = buffer_.size();
    if (pkt_length > 0) {
        const std::size_t declared_end = 6u + pkt_length;
        if (declared_end <= buffer_.size()) es_end = declared_end;
    }

    if (es_offset > es_end) {
        ++malformed_pes_;
        buffer_.clear();
        collecting_ = false;
        return;
    }

    PesPacket out;
    out.stream_id = stream_id;
    out.pts_90khz = pts;
    out.dts_90khz = dts;
    out.es.assign(buffer_.begin() + static_cast<std::ptrdiff_t>(es_offset),
                  buffer_.begin() + static_cast<std::ptrdiff_t>(es_end));

    ++pes_assembled_;
    buffer_.clear();
    collecting_ = false;
    if (sink_) sink_(std::move(out));
}

// ─── PesPacketizer ───────────────────────────────────────────────────────────

PesPacketizer::PesPacketizer(std::uint16_t pid, std::uint8_t& cc_state, PacketSink sink) noexcept
    : pid_(pid), cc_(cc_state), sink_(std::move(sink)) {}

void PesPacketizer::emit(const PesPacket& pes) {
    if (!sink_) return;

    // ── Build the PES header in a small scratch buffer ──────────────────────
    std::array<std::uint8_t, 19> hdr{};  // 6 fixed + 3 flags + up to 10 for PTS+DTS
    std::size_t hdr_len = 0;

    hdr[0] = 0x00;
    hdr[1] = 0x00;
    hdr[2] = 0x01;
    hdr[3] = pes.stream_id;
    // [4..5]: PES_packet_length, filled below

    std::uint8_t pts_dts_flags = 0;
    if (pes.pts_90khz.has_value() && pes.dts_90khz.has_value()) pts_dts_flags = 0b11;
    else if (pes.pts_90khz.has_value())                          pts_dts_flags = 0b10;

    const std::size_t pts_dts_bytes =
        (pts_dts_flags == 0b11) ? 10u :
        (pts_dts_flags == 0b10) ?  5u : 0u;

    hdr[6] = 0x80;                         // marker bits + no scrambling/priority/...
    hdr[7] = static_cast<std::uint8_t>(pts_dts_flags << 6);
    hdr[8] = static_cast<std::uint8_t>(pts_dts_bytes);

    if (pts_dts_flags == 0b10) {
        encodePts33(&hdr[9], *pes.pts_90khz, 0x2);
    } else if (pts_dts_flags == 0b11) {
        encodePts33(&hdr[9],  *pes.pts_90khz, 0x3);
        encodePts33(&hdr[14], *pes.dts_90khz, 0x1);
    }
    hdr_len = 9u + pts_dts_bytes;

    // PES_packet_length covers everything after the 6-byte fixed header. For
    // video where the total can exceed 65535 bytes, we write 0 ("unbounded").
    const std::size_t total_after_fixed = (hdr_len - 6u) + pes.es.size();
    std::uint16_t length_field = 0;
    if (total_after_fixed <= 0xFFFFu &&
        (pes.stream_id < 0xE0 || pes.stream_id > 0xEF)) {
        length_field = static_cast<std::uint16_t>(total_after_fixed);
    }
    hdr[4] = static_cast<std::uint8_t>((length_field >> 8) & 0xFF);
    hdr[5] = static_cast<std::uint8_t>(length_field & 0xFF);

    // ── Stream the (header + ES) bytes into 188-byte TS packets ─────────────
    // First packet has PUSI=1; remaining packets have PUSI=0. The last packet
    // gets an adaptation field with stuffing if the payload ends short.
    auto emit_packet = [&](const std::uint8_t* payload, std::size_t payload_len, bool first) {
        std::array<std::uint8_t, kTsPacketSize> pkt{};
        pkt[0] = kTsSyncByte;
        // PID + PUSI
        pkt[1] = static_cast<std::uint8_t>(((first ? 0x40 : 0x00)) | ((pid_ >> 8) & 0x1F));
        pkt[2] = static_cast<std::uint8_t>(pid_ & 0xFF);

        std::size_t payload_off = 4;
        if (payload_len < 184) {
            // Need adaptation field stuffing. After 4-byte header we have
            // 184 bytes; AF length byte + AF body must fill (184 - payload_len).
            const std::size_t af_total = 184u - payload_len;     // includes length byte
            const std::uint8_t af_len  = static_cast<std::uint8_t>(af_total - 1u);
            // adaptation_field_control = 0b11 (AF then payload), CC filled below.
            pkt[3] = 0x30;
            pkt[4] = af_len;
            if (af_len >= 1) {
                pkt[5] = 0x00;  // AF flags: nothing set
                std::memset(&pkt[6], 0xFF, af_len - 1u);  // stuffing
            }
            payload_off = 5u + af_len;
        } else {
            // No AF, payload-only.
            pkt[3] = 0x10;
        }
        // Set CC (low 4 bits of byte 3).
        pkt[3] = static_cast<std::uint8_t>((pkt[3] & 0xF0) | (cc_ & 0x0F));
        cc_ = static_cast<std::uint8_t>((cc_ + 1) & 0x0F);

        std::memcpy(&pkt[payload_off], payload, payload_len);
        sink_(pkt.data(), pkt.size());
    };

    // Concatenate hdr + es into a virtual stream and slice into 184-byte chunks.
    std::vector<std::uint8_t> linear;
    linear.reserve(hdr_len + pes.es.size());
    linear.insert(linear.end(), hdr.begin(), hdr.begin() + static_cast<std::ptrdiff_t>(hdr_len));
    linear.insert(linear.end(), pes.es.begin(), pes.es.end());

    const std::size_t total = linear.size();
    if (total == 0) return;

    std::size_t pos = 0;
    bool first = true;
    while (pos < total) {
        const std::size_t remaining = total - pos;
        const std::size_t chunk     = std::min<std::size_t>(remaining, 184u);
        emit_packet(linear.data() + pos, chunk, first);
        pos += chunk;
        first = false;
    }
}

}  // namespace liveqx::gateway::ts
