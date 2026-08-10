#include "gateway/ts/PsiBuilder.h"

#include <algorithm>
#include <cstring>

#include "gateway/ts/Crc32.h"

namespace liveqx::gateway::ts {
namespace {

// Encode the common 5-byte tail of every long-form PSI section header:
//   reserved(2) | section_length(12)
//   table_id_extension(16)
//   reserved(2) | version(5) | current_next(1)
//   section_number(8)        — always 0 for our single-section tables
//   last_section_number(8)   — also 0
//
// The CRC32 over (table_id..end-of-body) is appended afterwards; section_length
// is patched in at the very end once body size is known.
void writeShortSyntaxHeader(std::vector<std::uint8_t>& out,
                            std::uint8_t table_id,
                            std::uint16_t table_id_extension,
                            std::uint8_t version_number,
                            bool current_next_indicator) {
    out.push_back(table_id);
    // section_syntax_indicator(1)=1 + '0' bit + reserved(2)=0b11 + length(12)
    // Length is patched later — write 0 placeholder for now.
    out.push_back(0xB0);                               // 1011 0000 — top 4 bits + length high nibble=0
    out.push_back(0x00);                               // length low byte
    out.push_back(static_cast<std::uint8_t>(table_id_extension >> 8));
    out.push_back(static_cast<std::uint8_t>(table_id_extension & 0xFF));
    out.push_back(static_cast<std::uint8_t>(0xC0 | ((version_number & 0x1F) << 1) |
                                            (current_next_indicator ? 1 : 0)));
    out.push_back(0x00);                               // section_number
    out.push_back(0x00);                               // last_section_number
}

// Patch section_length (12 bits) at offset 1..2 of `buf`. Must be called
// BEFORE the 4-byte CRC is appended; the +4 accounts for those CRC bytes
// since section_length covers everything after the length field including
// the CRC, per ISO 13818-1 §2.4.4.
void patchSectionLength(std::vector<std::uint8_t>& buf) {
    const std::size_t length = buf.size() - 3 + 4;     // body after length + CRC
    buf[1] = static_cast<std::uint8_t>((buf[1] & 0xF0) | ((length >> 8) & 0x0F));
    buf[2] = static_cast<std::uint8_t>(length & 0xFF);
}

// Append big-endian CRC32 over the entire buffer so far.
void appendCrc32(std::vector<std::uint8_t>& buf) {
    const std::uint32_t c = crc32Mpeg2(std::span<const std::uint8_t>(buf.data(), buf.size()));
    buf.push_back(static_cast<std::uint8_t>((c >> 24) & 0xFF));
    buf.push_back(static_cast<std::uint8_t>((c >> 16) & 0xFF));
    buf.push_back(static_cast<std::uint8_t>((c >>  8) & 0xFF));
    buf.push_back(static_cast<std::uint8_t>( c        & 0xFF));
}

// Emit a descriptor list verbatim (each entry: tag + length + body).
// Returns total bytes written.
std::size_t writeDescriptorLoop(std::vector<std::uint8_t>& out,
                                std::span<const RawDescriptor> descs) {
    const std::size_t before = out.size();
    for (const auto& d : descs) {
        out.push_back(d.tag);
        // Body length is stored in body.size() per RawDescriptor convention.
        // Spec caps single-descriptor length at 255.
        const std::size_t blen = std::min<std::size_t>(d.body.size(), 255);
        out.push_back(static_cast<std::uint8_t>(blen));
        out.insert(out.end(), d.body.begin(), d.body.begin() + blen);
    }
    return out.size() - before;
}

}  // namespace

// ─── PAT ────────────────────────────────────────────────────────────────────

std::vector<std::uint8_t> buildPatSection(const PatBuildInput& in) {
    std::vector<std::uint8_t> sec;
    sec.reserve(8 + 4 * (in.programs.size() + (in.network_pid != 0 ? 1u : 0u)) + 4);

    writeShortSyntaxHeader(sec, /*table_id=*/0x00,
                           in.transport_stream_id,
                           in.version_number,
                           in.current_next_indicator);

    // Emit network entry first if present (program_number=0).
    if (in.network_pid != 0) {
        sec.push_back(0x00);                                        // pn high
        sec.push_back(0x00);                                        // pn low
        sec.push_back(static_cast<std::uint8_t>(0xE0 | ((in.network_pid >> 8) & 0x1F)));
        sec.push_back(static_cast<std::uint8_t>(in.network_pid & 0xFF));
    }
    for (const auto& p : in.programs) {
        sec.push_back(static_cast<std::uint8_t>((p.program_number >> 8) & 0xFF));
        sec.push_back(static_cast<std::uint8_t>(p.program_number & 0xFF));
        sec.push_back(static_cast<std::uint8_t>(0xE0 | ((p.pmt_pid >> 8) & 0x1F)));
        sec.push_back(static_cast<std::uint8_t>(p.pmt_pid & 0xFF));
    }

    patchSectionLength(sec);
    appendCrc32(sec);
    return sec;
}

// ─── PMT ────────────────────────────────────────────────────────────────────

std::vector<std::uint8_t> buildPmtSection(const PmtBuildInput& in) {
    std::vector<std::uint8_t> sec;
    sec.reserve(64 + in.streams.size() * 8);

    writeShortSyntaxHeader(sec, /*table_id=*/0x02,
                           in.program_number,
                           in.version_number,
                           in.current_next_indicator);

    // PCR_PID (13 bits) with reserved=0b111.
    sec.push_back(static_cast<std::uint8_t>(0xE0 | ((in.pcr_pid >> 8) & 0x1F)));
    sec.push_back(static_cast<std::uint8_t>(in.pcr_pid & 0xFF));

    // program_info_length placeholder; fill in after writing descriptors.
    const std::size_t pi_len_off = sec.size();
    sec.push_back(0xF0);                                            // reserved=0b1111 + len high
    sec.push_back(0x00);
    const std::size_t pi_written = writeDescriptorLoop(sec, in.program_descriptors);
    sec[pi_len_off]     = static_cast<std::uint8_t>(0xF0 | ((pi_written >> 8) & 0x0F));
    sec[pi_len_off + 1] = static_cast<std::uint8_t>(pi_written & 0xFF);

    for (const auto& s : in.streams) {
        sec.push_back(s.stream_type);
        sec.push_back(static_cast<std::uint8_t>(0xE0 | ((s.elementary_pid >> 8) & 0x1F)));
        sec.push_back(static_cast<std::uint8_t>(s.elementary_pid & 0xFF));
        const std::size_t es_len_off = sec.size();
        sec.push_back(0xF0);
        sec.push_back(0x00);
        const std::size_t es_written = writeDescriptorLoop(sec, s.es_descriptors);
        sec[es_len_off]     = static_cast<std::uint8_t>(0xF0 | ((es_written >> 8) & 0x0F));
        sec[es_len_off + 1] = static_cast<std::uint8_t>(es_written & 0xFF);
    }

    patchSectionLength(sec);
    appendCrc32(sec);
    return sec;
}

// ─── SDT ────────────────────────────────────────────────────────────────────

std::vector<std::uint8_t> buildSdtSection(const SdtBuildInput& in) {
    std::vector<std::uint8_t> sec;
    sec.reserve(16 + in.services.size() * 32);

    writeShortSyntaxHeader(sec, /*table_id=*/in.actual ? 0x42 : 0x46,
                           in.transport_stream_id,
                           in.version_number,
                           in.current_next_indicator);
    // SDT-specific extra header bytes (after the common 8): original_network_id
    // (16) + reserved_future_use (8).
    sec.push_back(static_cast<std::uint8_t>((in.original_network_id >> 8) & 0xFF));
    sec.push_back(static_cast<std::uint8_t>(in.original_network_id & 0xFF));
    sec.push_back(0xFF);                                            // reserved_future_use

    for (const auto& svc : in.services) {
        sec.push_back(static_cast<std::uint8_t>((svc.service_id >> 8) & 0xFF));
        sec.push_back(static_cast<std::uint8_t>(svc.service_id & 0xFF));
        // reserved(6)=0b111111 + EIT_schedule(1) + EIT_present_following(1)
        std::uint8_t flags = 0xFC;
        if (svc.eit_schedule_flag)          flags |= 0x02;
        if (svc.eit_present_following_flag) flags |= 0x01;
        sec.push_back(flags);
        // running_status(3) + free_CA_mode(1) + descriptors_loop_length(12)
        const std::size_t dlen_off = sec.size();
        sec.push_back(static_cast<std::uint8_t>(((svc.running_status & 0x07) << 5) |
                                                (svc.free_ca_mode ? 0x10 : 0x00)));
        sec.push_back(0x00);
        const std::size_t dwritten = writeDescriptorLoop(sec, svc.descriptors);
        sec[dlen_off]     = static_cast<std::uint8_t>((sec[dlen_off] & 0xF0) | ((dwritten >> 8) & 0x0F));
        sec[dlen_off + 1] = static_cast<std::uint8_t>(dwritten & 0xFF);
    }

    patchSectionLength(sec);
    appendCrc32(sec);
    return sec;
}

// ─── EIT ────────────────────────────────────────────────────────────────────

namespace {

// Forward MJD from civil date — inverse of the Annex C decoder. Howard
// Hinnant's days-from-civil with EN 300 468 §C.2 mjd offset of 40587.
std::int32_t civilToMjd(int year, int month, int day) noexcept {
    const int y   = year - (month <= 2);
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const std::int64_t days_unix = era * 146097LL + static_cast<std::int64_t>(doe) - 719468LL;
    return static_cast<std::int32_t>(days_unix + 40587);
}

// Inverse Howard Hinnant: unix days → civil (year, month, day).
void daysToCivil(std::int64_t days, int& year, int& month, int& day) noexcept {
    const std::int64_t z   = days + 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe     = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe     = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int y            = static_cast<int>(yoe) + static_cast<int>(era) * 400;
    const unsigned doy     = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp      = (5 * doy + 2) / 153;
    day                    = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    month                  = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    year                   = y + (month <= 2);
}

inline std::uint8_t toBcd(int v) noexcept {
    return static_cast<std::uint8_t>(((v / 10) << 4) | (v % 10));
}

}  // namespace

void encodeMjdUtc(std::uint64_t unix_time, std::span<std::uint8_t, 5> out) noexcept {
    // start_time_undefined per spec is all-ones; we use unix_time==0 sentinel
    // (the decoder also returns 0 for that case → clean round-trip).
    if (unix_time == 0) {
        out[0] = out[1] = out[2] = out[3] = out[4] = 0;
        return;
    }
    const std::int64_t days = static_cast<std::int64_t>(unix_time / 86400);
    const std::uint32_t tod = static_cast<std::uint32_t>(unix_time % 86400);

    int year = 0, month = 0, day = 0;
    daysToCivil(days, year, month, day);
    const std::int32_t mjd = civilToMjd(year, month, day);

    out[0] = static_cast<std::uint8_t>((mjd >> 8) & 0xFF);
    out[1] = static_cast<std::uint8_t>(mjd & 0xFF);
    out[2] = toBcd(static_cast<int>(tod / 3600));
    out[3] = toBcd(static_cast<int>((tod / 60) % 60));
    out[4] = toBcd(static_cast<int>(tod % 60));
}

void encodeBcdDuration(std::uint32_t duration_sec, std::span<std::uint8_t, 3> out) noexcept {
    if (duration_sec > 86399) duration_sec = 86399;       // clamp to <24h
    out[0] = toBcd(static_cast<int>(duration_sec / 3600));
    out[1] = toBcd(static_cast<int>((duration_sec / 60) % 60));
    out[2] = toBcd(static_cast<int>(duration_sec % 60));
}

std::vector<std::uint8_t> buildEitSection(const EitBuildInput& in) {
    std::vector<std::uint8_t> sec;
    sec.reserve(20 + in.events.size() * 16);

    writeShortSyntaxHeader(sec, in.table_id,
                           in.service_id,
                           in.version_number,
                           in.current_next_indicator);
    // EIT-specific extras after the 8-byte common header:
    //   transport_stream_id(16) + original_network_id(16)
    //   + segment_last_section_number(8) + last_table_id(8)
    sec.push_back(static_cast<std::uint8_t>((in.transport_stream_id >> 8) & 0xFF));
    sec.push_back(static_cast<std::uint8_t>(in.transport_stream_id & 0xFF));
    sec.push_back(static_cast<std::uint8_t>((in.original_network_id >> 8) & 0xFF));
    sec.push_back(static_cast<std::uint8_t>(in.original_network_id & 0xFF));
    sec.push_back(0x00);                                            // segment_last_section_number
    sec.push_back(in.table_id);                                     // last_table_id

    for (const auto& ev : in.events) {
        sec.push_back(static_cast<std::uint8_t>((ev.event_id >> 8) & 0xFF));
        sec.push_back(static_cast<std::uint8_t>(ev.event_id & 0xFF));
        // 5-byte start_time (MJD + BCD HHMMSS)
        const std::size_t st_off = sec.size();
        sec.resize(st_off + 5);
        encodeMjdUtc(ev.start_time_utc,
                     std::span<std::uint8_t, 5>(sec.data() + st_off, 5));
        // 3-byte BCD duration
        const std::size_t du_off = sec.size();
        sec.resize(du_off + 3);
        encodeBcdDuration(ev.duration_sec,
                          std::span<std::uint8_t, 3>(sec.data() + du_off, 3));
        // running_status(3) + free_CA_mode(1) + descs_loop_length(12) placeholder
        const std::size_t dlen_off = sec.size();
        sec.push_back(static_cast<std::uint8_t>(((ev.running_status & 0x07) << 5) |
                                                (ev.free_ca_mode ? 0x10 : 0x00)));
        sec.push_back(0x00);
        const std::size_t dwritten = writeDescriptorLoop(sec, ev.descriptors);
        sec[dlen_off]     = static_cast<std::uint8_t>((sec[dlen_off] & 0xF0) | ((dwritten >> 8) & 0x0F));
        sec[dlen_off + 1] = static_cast<std::uint8_t>(dwritten & 0xFF);
    }

    patchSectionLength(sec);
    appendCrc32(sec);
    return sec;
}

// ─── service_descriptor (0x48) builder ─────────────────────────────────────

RawDescriptor makeServiceDescriptor(std::uint8_t service_type,
                                    std::string_view provider_name,
                                    std::string_view service_name) {
    RawDescriptor d;
    d.tag = 0x48;
    // EN 300 468 §6.2.33: service_type(8) | provider_name_length(8) |
    //                     provider_chars[N] | service_name_length(8) |
    //                     service_chars[M]
    // We emit names as UTF-8 with selector 0x15 prefix when non-ASCII;
    // otherwise leave selector-less (ISO 6937 default).
    auto needsSelector = [](std::string_view s) {
        for (char c : s) if (static_cast<std::uint8_t>(c) >= 0x80) return true;
        return false;
    };

    auto pushDvbString = [&](std::string_view s) {
        const bool sel = needsSelector(s);
        const std::size_t len = std::min<std::size_t>(s.size() + (sel ? 1 : 0), 255);
        d.body.push_back(static_cast<std::uint8_t>(len));
        if (sel) d.body.push_back(0x15);                            // UTF-8 selector
        std::size_t avail = len - (sel ? 1 : 0);
        avail = std::min(avail, s.size());
        d.body.insert(d.body.end(), s.begin(), s.begin() + avail);
    };

    d.body.push_back(service_type);
    pushDvbString(provider_name);
    pushDvbString(service_name);
    return d;
}

// ─── Packetization ──────────────────────────────────────────────────────────

std::vector<std::array<std::uint8_t, kTsPacketSize>>
packetizeSection(std::uint16_t pid,
                 std::span<const std::uint8_t> section,
                 std::uint8_t& cc) noexcept {
    std::vector<std::array<std::uint8_t, kTsPacketSize>> packets;
    if (section.empty()) return packets;

    std::size_t pos = 0;
    bool first = true;
    while (pos < section.size()) {
        std::array<std::uint8_t, kTsPacketSize> p{};
        p[0] = kTsSyncByte;
        // PUSI on first packet only.
        p[1] = static_cast<std::uint8_t>((first ? 0x40 : 0x00) | ((pid >> 8) & 0x1F));
        p[2] = static_cast<std::uint8_t>(pid & 0xFF);
        // PayloadOnly + CC.
        p[3] = static_cast<std::uint8_t>(0x10 | (cc & 0x0F));
        cc = static_cast<std::uint8_t>((cc + 1) & 0x0F);

        std::size_t off = 4;
        if (first) {
            p[off++] = 0x00;                                        // pointer_field
            first = false;
        }
        const std::size_t take = std::min<std::size_t>(kTsPacketSize - off, section.size() - pos);
        std::memcpy(p.data() + off, section.data() + pos, take);
        pos += take;
        off += take;
        // Stuffing 0xFF on the final packet's tail per §2.4.4.
        if (off < kTsPacketSize) std::memset(p.data() + off, 0xFF, kTsPacketSize - off);
        packets.push_back(p);
    }
    return packets;
}

}  // namespace liveqx::gateway::ts
