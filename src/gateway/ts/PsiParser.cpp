#include "gateway/ts/PsiParser.h"

#include <algorithm>
#include <cstring>

#include "gateway/ts/Crc32.h"

namespace liveqx::gateway::ts {
namespace {

// PSI section header: 8 bytes for short tables (PAT/PMT) plus extension fields
// in long-format tables (SDT/EIT). Common prefix:
//   table_id(8) | section_syntax_indicator(1) | '0' | reserved(2)
//   | section_length(12) → so `payload_size_after_header = section_length`
constexpr std::size_t kSectionMinSize = 4 + 4;  // 4-byte common header + 4-byte CRC

bool sectionLooksValid(std::span<const std::uint8_t> sec) noexcept {
    if (sec.size() < kSectionMinSize) return false;
    const std::size_t section_length =
        static_cast<std::size_t>(((sec[1] & 0x0F) << 8) | sec[2]);
    if (section_length + 3 != sec.size()) return false;       // length mismatch
    if (crc32Mpeg2(sec) != 0) return false;                   // CRC over whole section
    return true;
}

// Returns std::nullopt if section_length looks malformed (would extend past
// the assembler's buffer). Used during reassembly so we abort early instead
// of silently waiting for ever more bytes.
std::optional<std::size_t> sectionLengthFromHeader(std::span<const std::uint8_t> hdr) noexcept {
    if (hdr.size() < 3) return std::nullopt;
    const std::size_t section_length =
        static_cast<std::size_t>(((hdr[1] & 0x0F) << 8) | hdr[2]);
    // Per ISO 13818-1 §2.4.4.10 section_length max = 1021 (PAT/PMT) or 4093 (private).
    // We cap at 4093 to be permissive; 4093 + 3 = 4096 fits in our buffer.
    if (section_length > 4093) return std::nullopt;
    return section_length + 3;
}

}  // namespace

// ─── Section assembler ──────────────────────────────────────────────────────

PsiSectionAssembler::PerPidState& PsiSectionAssembler::state(std::uint16_t pid) {
    return pids_[pid];
}

void PsiSectionAssembler::reset(std::uint16_t pid) {
    auto it = pids_.find(pid);
    if (it != pids_.end()) {
        it->second.buffer.clear();
        it->second.expected = 0;
        it->second.in_progress = false;
        it->second.last_cc = 0xFF;
    }
}

void PsiSectionAssembler::clearAll() { pids_.clear(); }

void PsiSectionAssembler::feed(TsPacketView pkt, const SectionCb& cb) {
    if (!pkt.isValidSync() || pkt.tei() || !pkt.hasPayload()) return;
    if (pkt.scrambling() != 0) return;  // PSI is never scrambled per spec

    const std::uint16_t pid = pkt.pid();
    auto& st = state(pid);

    // CC continuity check. Allow first packet (last_cc=0xFF) and exact
    // (last+1)&0xF. Anything else → drop in-progress section, restart on
    // next PUSI.
    if (st.in_progress) {
        const std::uint8_t expected_cc = (st.last_cc + 1u) & 0x0F;
        if (pkt.continuityCounter() != expected_cc) {
            st.buffer.clear();
            st.expected = 0;
            st.in_progress = false;
        }
    }
    st.last_cc = pkt.continuityCounter();

    auto payload = pkt.payload();
    if (payload.empty()) return;

    std::size_t cursor = 0;
    if (pkt.pusi()) {
        // First payload byte is pointer_field; bytes [0..ptr) belong to the
        // previous section (continuation), [ptr..end) starts a new section.
        const std::uint8_t pointer_field = payload[0];
        if (pointer_field >= payload.size()) {
            // Malformed pointer — drop and wait for next PUSI.
            st.buffer.clear();
            st.expected = 0;
            st.in_progress = false;
            return;
        }

        // Finish off any in-progress section with the bytes up to pointer.
        if (st.in_progress && pointer_field > 0) {
            st.buffer.insert(st.buffer.end(),
                             payload.begin() + 1,
                             payload.begin() + 1 + pointer_field);
            if (st.buffer.size() >= st.expected) {
                cb(pid, std::span<const std::uint8_t>(st.buffer.data(), st.expected));
            }
            // Whether complete or not, drop carry-over and start fresh below.
        }

        cursor = 1u + pointer_field;
        st.buffer.clear();
        st.expected = 0;
        st.in_progress = true;
    } else if (!st.in_progress) {
        // No PUSI and no in-progress section → ignore (can't sync mid-stream).
        return;
    }

    // Consume successive sections from the payload. A single PUSI packet may
    // start one section and inline-complete it before the slot ends; if there
    // are leftover bytes after the section, they may begin yet another
    // section (rare but legal).
    while (cursor < payload.size()) {
        if (st.expected == 0) {
            // Need at least 3 bytes to read section_length.
            const std::size_t avail = payload.size() - cursor;
            // Copy whatever we have so we can read length on the next packet
            // if it's split across the boundary.
            st.buffer.insert(st.buffer.end(),
                             payload.begin() + cursor,
                             payload.end());
            cursor = payload.size();
            if (st.buffer.size() < 3) return;                 // need more bytes
            auto len = sectionLengthFromHeader(
                std::span<const std::uint8_t>(st.buffer.data(), 3));
            if (!len) {
                st.buffer.clear();
                st.expected = 0;
                st.in_progress = false;
                return;
            }
            st.expected = *len;
            (void)avail;
        } else {
            const std::size_t need = st.expected - st.buffer.size();
            const std::size_t take = std::min(need, payload.size() - cursor);
            st.buffer.insert(st.buffer.end(),
                             payload.begin() + cursor,
                             payload.begin() + cursor + take);
            cursor += take;
        }

        if (st.buffer.size() >= st.expected) {
            cb(pid, std::span<const std::uint8_t>(st.buffer.data(), st.expected));
            // Stuffing bytes (0xFF) can follow a complete section in the same
            // payload. Skip them; if we hit a non-0xFF byte, that's a new
            // section and we restart the assembler for it.
            std::size_t tail = st.expected;
            (void)tail;
            st.buffer.clear();
            st.expected = 0;
            // Skip any 0xFF stuffing in the rest of the payload.
            while (cursor < payload.size() && payload[cursor] == 0xFF) ++cursor;
            // If there are non-stuffing bytes left, treat them as a new
            // section header (only valid if PUSI delivered them — but the
            // spec allows multiple sections per packet only when the second
            // is ≤ 1 byte short of the first; we accept it permissively).
        }
    }
}

// ─── PAT ────────────────────────────────────────────────────────────────────

std::optional<ParsedPat> parsePat(std::span<const std::uint8_t> section) {
    if (!sectionLooksValid(section)) return std::nullopt;
    if (section[0] != 0x00) return std::nullopt;          // table_id
    if ((section[1] & 0x80) == 0) return std::nullopt;    // section_syntax_indicator must be 1

    ParsedPat out;
    out.transport_stream_id   = static_cast<std::uint16_t>((section[3] << 8) | section[4]);
    out.version_number         = static_cast<std::uint8_t>((section[5] >> 1) & 0x1F);
    out.current_next_indicator = (section[5] & 0x01) != 0;

    // Body: 8-byte header (incl table_id), then N×4-byte program entries,
    // then 4-byte CRC.
    const std::size_t body_begin = 8;
    const std::size_t body_end   = section.size() - 4;  // exclude CRC
    for (std::size_t i = body_begin; i + 4 <= body_end; i += 4) {
        const std::uint16_t pn  = static_cast<std::uint16_t>((section[i] << 8) | section[i + 1]);
        const std::uint16_t pid = static_cast<std::uint16_t>(((section[i + 2] & 0x1F) << 8) | section[i + 3]);
        if (pn == 0) {
            out.network_pid = pid;
        } else {
            out.programs.push_back({pn, pid});
        }
    }
    return out;
}

// ─── PMT ────────────────────────────────────────────────────────────────────

std::optional<ParsedPmt> parsePmt(std::span<const std::uint8_t> section) {
    if (!sectionLooksValid(section)) return std::nullopt;
    if (section[0] != 0x02) return std::nullopt;
    if ((section[1] & 0x80) == 0) return std::nullopt;

    ParsedPmt out;
    out.program_number         = static_cast<std::uint16_t>((section[3] << 8) | section[4]);
    out.version_number         = static_cast<std::uint8_t>((section[5] >> 1) & 0x1F);
    out.current_next_indicator = (section[5] & 0x01) != 0;
    out.pcr_pid                = static_cast<std::uint16_t>(((section[8] & 0x1F) << 8) | section[9]);

    const std::size_t prog_info_length =
        static_cast<std::size_t>(((section[10] & 0x0F) << 8) | section[11]);
    const std::size_t pi_begin = 12;
    const std::size_t pi_end   = pi_begin + prog_info_length;
    if (pi_end > section.size() - 4) return std::nullopt;

    out.program_descriptors = parseDescriptorLoop(
        std::span<const std::uint8_t>(section.data() + pi_begin, prog_info_length));

    std::size_t i = pi_end;
    const std::size_t body_end = section.size() - 4;
    while (i + 5 <= body_end) {
        PmtStream s;
        s.stream_type    = section[i];
        s.elementary_pid = static_cast<std::uint16_t>(((section[i + 1] & 0x1F) << 8) | section[i + 2]);
        const std::size_t es_info_length =
            static_cast<std::size_t>(((section[i + 3] & 0x0F) << 8) | section[i + 4]);
        if (i + 5 + es_info_length > body_end) return std::nullopt;
        s.es_descriptors = parseDescriptorLoop(
            std::span<const std::uint8_t>(section.data() + i + 5, es_info_length));
        i += 5 + es_info_length;
        out.streams.push_back(std::move(s));
    }
    return out;
}

// ─── SDT ────────────────────────────────────────────────────────────────────

std::optional<ParsedSdt> parseSdt(std::span<const std::uint8_t> section) {
    if (!sectionLooksValid(section)) return std::nullopt;
    // table_id 0x42 (actual TS) or 0x46 (other TS). We accept both.
    if (section[0] != 0x42 && section[0] != 0x46) return std::nullopt;
    if ((section[1] & 0x80) == 0) return std::nullopt;

    ParsedSdt out;
    out.transport_stream_id    = static_cast<std::uint16_t>((section[3] << 8) | section[4]);
    out.version_number          = static_cast<std::uint8_t>((section[5] >> 1) & 0x1F);
    out.current_next_indicator  = (section[5] & 0x01) != 0;
    out.original_network_id     = static_cast<std::uint16_t>((section[8] << 8) | section[9]);
    // section[10] is reserved-future-use(8)

    std::size_t i = 11;
    const std::size_t body_end = section.size() - 4;
    while (i + 5 <= body_end) {
        SdtService svc;
        svc.service_id                 = static_cast<std::uint16_t>((section[i] << 8) | section[i + 1]);
        // section[i+2]: reserved(6) | EIT_schedule_flag(1) | EIT_present_following_flag(1)
        svc.eit_schedule_flag          = (section[i + 2] & 0x02) != 0;
        svc.eit_present_following_flag = (section[i + 2] & 0x01) != 0;
        // section[i+3..4]: running_status(3) | free_CA_mode(1) | descriptors_loop_length(12)
        svc.running_status   = static_cast<std::uint8_t>((section[i + 3] >> 5) & 0x07);
        svc.free_ca_mode     = (section[i + 3] & 0x10) != 0;
        const std::size_t descs_len =
            static_cast<std::size_t>(((section[i + 3] & 0x0F) << 8) | section[i + 4]);
        if (i + 5 + descs_len > body_end) return std::nullopt;
        svc.descriptors = parseDescriptorLoop(
            std::span<const std::uint8_t>(section.data() + i + 5, descs_len));

        // Walk descriptors looking for 0x48 service_descriptor (provider+name).
        for (const auto& d : svc.descriptors) {
            if (d.tag == 0x48 && d.body.size() >= 2) {
                const std::uint8_t prov_len = d.body[1];
                if (2u + prov_len + 1u > d.body.size()) continue;
                svc.provider_name = decodeDvbText(
                    std::span<const std::uint8_t>(d.body.data() + 2, prov_len));
                const std::uint8_t name_len = d.body[2 + prov_len];
                if (3u + prov_len + name_len > d.body.size()) continue;
                svc.service_name = decodeDvbText(
                    std::span<const std::uint8_t>(d.body.data() + 3 + prov_len, name_len));
                break;
            }
        }

        i += 5 + descs_len;
        out.services.push_back(std::move(svc));
    }
    return out;
}

// ─── EIT ────────────────────────────────────────────────────────────────────

std::optional<ParsedEit> parseEit(std::span<const std::uint8_t> section) {
    if (!sectionLooksValid(section)) return std::nullopt;
    const std::uint8_t tid = section[0];
    if (tid < 0x4E || tid > 0x6F) return std::nullopt;
    if ((section[1] & 0x80) == 0) return std::nullopt;

    ParsedEit out;
    out.table_id            = tid;
    out.actual              = (tid == 0x4E) || (tid >= 0x50 && tid <= 0x5F);
    out.present_following   = (tid == 0x4E) || (tid == 0x4F);
    out.service_id          = static_cast<std::uint16_t>((section[3] << 8) | section[4]);
    out.version_number      = static_cast<std::uint8_t>((section[5] >> 1) & 0x1F);
    out.transport_stream_id = static_cast<std::uint16_t>((section[8]  << 8) | section[9]);
    out.original_network_id = static_cast<std::uint16_t>((section[10] << 8) | section[11]);
    // section[12] = segment_last_section_number, section[13] = last_table_id

    std::size_t i = 14;
    const std::size_t body_end = section.size() - 4;
    while (i + 12 <= body_end) {
        EitEvent ev;
        ev.event_id       = static_cast<std::uint16_t>((section[i] << 8) | section[i + 1]);
        ev.start_time_utc = decodeMjdUtc(
            std::span<const std::uint8_t, 5>(section.data() + i + 2, 5));
        ev.duration_sec   = decodeBcdDuration(
            std::span<const std::uint8_t, 3>(section.data() + i + 7, 3));
        ev.running_status = static_cast<std::uint8_t>((section[i + 10] >> 5) & 0x07);
        ev.free_ca_mode   = (section[i + 10] & 0x10) != 0;
        const std::size_t descs_len =
            static_cast<std::size_t>(((section[i + 10] & 0x0F) << 8) | section[i + 11]);
        if (i + 12 + descs_len > body_end) return std::nullopt;
        ev.descriptors = parseDescriptorLoop(
            std::span<const std::uint8_t>(section.data() + i + 12, descs_len));
        i += 12 + descs_len;
        out.events.push_back(std::move(ev));
    }
    return out;
}

// ─── DVB text decoding (subset) ──────────────────────────────────────────────
//
// Full EN 300 468 Annex A handles ~15 character sets. We implement the common
// cases:
//   * No selector byte → ISO 6937 (mapped to ASCII for printable, others
//     escaped as '?')
//   * 0x01..0x05      → ISO/IEC 8859-1..-5
//   * 0x10 0x00 0xXX → ISO/IEC 8859-XX (where XX is selector)
//   * 0x11           → UTF-16BE (rare in DVB-T, common in IPTV)
//   * 0x15           → UTF-8 (already correct)
//
// Anything outside these is decoded as ISO 8859-1 (Latin-1) — safe default
// that survives round-trip in PSI regen.

namespace {

std::string utf8FromIso8859(std::span<const std::uint8_t> body, int part) {
    // Latin-1/-5/-15 etc. Only Latin-1 (part=1) and Latin/Cyrillic-5 (part=5)
    // cover what we see in real DVB; use Latin-1 by default with a Cyrillic
    // override at part=5.
    std::string out;
    out.reserve(body.size() * 2);
    for (auto b : body) {
        if (b == 0x86 || b == 0x87) continue;   // emphasis on/off control codes
        if (b < 0x80) {
            if (b >= 0x20 || b == 0x0A) out.push_back(static_cast<char>(b));
            continue;
        }
        std::uint32_t cp = b;
        if (part == 5) {
            // ISO 8859-5: 0xA0..0xFF maps to U+00A0, U+0401..U+04FF range.
            if (b >= 0xA1 && b <= 0xAC) cp = 0x0400u + (b - 0xA0);
            else if (b == 0xAD)         cp = 0x00ADu;  // soft hyphen
            else if (b >= 0xAE && b <= 0xEF) cp = 0x0400u + (b - 0xA0);
            else if (b >= 0xF0 && b <= 0xFC) cp = 0x0450u + (b - 0xF0);
        } else {
            // Latin-1 — direct mapping, U+0080..U+00FF.
        }
        // UTF-8 encode the codepoint.
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

}  // namespace

std::string decodeDvbText(std::span<const std::uint8_t> raw) {
    if (raw.empty()) return {};
    const std::uint8_t first = raw[0];

    // No selector — body is ISO 6937 / Latin-1 starting from byte 0.
    if (first >= 0x20 && first != 0x86 && first != 0x87) {
        return utf8FromIso8859(raw, 1);
    }

    // 0x01..0x0F → ISO 8859-(1+(first-1)). 0x05 → ISO 8859-5 (Cyrillic).
    if (first >= 0x01 && first <= 0x0F) {
        const int part = first;
        return utf8FromIso8859(raw.subspan(1), part);
    }

    // 0x10 0x00 0xXX → ISO 8859-XX.
    if (first == 0x10 && raw.size() >= 3 && raw[1] == 0x00) {
        const int part = raw[2];
        return utf8FromIso8859(raw.subspan(3), part);
    }

    // 0x11 → UTF-16BE.
    if (first == 0x11) {
        std::string out;
        out.reserve(raw.size());
        for (std::size_t i = 1; i + 2 <= raw.size(); i += 2) {
            std::uint32_t cp = (std::uint32_t(raw[i]) << 8) | raw[i + 1];
            // Emphasis controls in UTF-16 are also 0x0086/0x0087.
            if (cp == 0x0086 || cp == 0x0087) continue;
            if (cp < 0x80) {
                out.push_back(static_cast<char>(cp));
            } else if (cp < 0x800) {
                out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            } else {
                // BMP only; surrogate pairs not handled (not seen in practice in DVB).
                out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            }
        }
        return out;
    }

    // 0x15 → UTF-8 (pass-through after stripping selector).
    if (first == 0x15) {
        return std::string(reinterpret_cast<const char*>(raw.data() + 1), raw.size() - 1);
    }

    // Unknown selector — best-effort Latin-1 from byte 1.
    return utf8FromIso8859(raw.subspan(1), 1);
}

// ─── MJD/UTC + BCD time helpers ─────────────────────────────────────────────
//
// EIT start_time format: 16-bit MJD + 24-bit BCD UTC HHMMSS.
// MJD epoch: 1858-11-17. Conversion follows EN 300 468 Annex C.

std::uint64_t decodeMjdUtc(std::span<const std::uint8_t, 5> raw) {
    const std::uint16_t mjd = static_cast<std::uint16_t>((raw[0] << 8) | raw[1]);
    if (mjd == 0xFFFF) return 0;  // start_time_undefined

    // EN 300 468 Annex C inverse formula.
    int yp  = static_cast<int>((mjd - 15078.2) / 365.25);
    int mp  = static_cast<int>((mjd - 14956.1 - static_cast<int>(yp * 365.25)) / 30.6001);
    int day = mjd - 14956 - static_cast<int>(yp * 365.25) - static_cast<int>(mp * 30.6001);
    int k   = (mp == 14 || mp == 15) ? 1 : 0;
    int year  = 1900 + yp + k;
    int month = mp - 1 - k * 12;
    if (month < 1 || month > 12 || day < 1 || day > 31) return 0;

    auto bcd = [](std::uint8_t b) noexcept -> int {
        return ((b >> 4) * 10) + (b & 0x0F);
    };
    const int hour = bcd(raw[2]);
    const int min  = bcd(raw[3]);
    const int sec  = bcd(raw[4]);
    if (hour > 23 || min > 59 || sec > 60) return 0;

    // Days since unix epoch (1970-01-01) using Howard Hinnant's algorithm.
    int y = year - (month <= 2);
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const std::int64_t days = era * 146097LL + static_cast<std::int64_t>(doe) - 719468LL;

    return static_cast<std::uint64_t>(days * 86400 + hour * 3600 + min * 60 + sec);
}

std::uint32_t decodeBcdDuration(std::span<const std::uint8_t, 3> raw) {
    auto bcd = [](std::uint8_t b) noexcept -> int {
        return ((b >> 4) * 10) + (b & 0x0F);
    };
    const int h = bcd(raw[0]);
    const int m = bcd(raw[1]);
    const int s = bcd(raw[2]);
    if (h > 99 || m > 59 || s > 60) return 0;
    return static_cast<std::uint32_t>(h * 3600 + m * 60 + s);
}

}  // namespace liveqx::gateway::ts
