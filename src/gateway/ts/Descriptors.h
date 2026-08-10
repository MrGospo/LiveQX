#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace liveqx::gateway::ts {

// MPEG-TS / DVB descriptor parsing — only the descriptors we need for
// scope: subtitle/teletext/AC-3 preservation and stream identification.
// Anything else stays as opaque bytes; PSI regeneration copies it through
// without interpretation so we don't drop unknown CA/copy-protection
// metadata that downstream gear may need.
//
// Tag values per ETSI EN 300 468 §6.1 and ISO/IEC 13818-1 Table 2-39.

enum DescriptorTag : std::uint8_t {
    kDescVideoStream      = 0x02,
    kDescAudioStream      = 0x03,
    kDescRegistration     = 0x05,
    kDescIso639Language   = 0x0A,
    kDescStreamIdentifier = 0x52,
    kDescTeletext         = 0x56,
    kDescSubtitling       = 0x59,
    kDescAc3              = 0x6A,  // DVB AC-3
    kDescEnhancedAc3      = 0x7A,  // DVB E-AC-3 / DD+
};

struct LanguageCode {
    std::array<char, 3> code{};  // ISO 639-2 lowercase, e.g. "rus","eng","fra"
    std::string toString() const { return std::string(code.data(), 3); }
};

// Descriptor 0x59 — DVB subtitling. May contain multiple language entries
// in one descriptor (each 8 bytes after the tag/length pair).
struct SubtitlingEntry {
    LanguageCode  language;
    std::uint8_t  subtitling_type     = 0;  // EN 300 468 Table 26
    std::uint16_t composition_page_id = 0;
    std::uint16_t ancillary_page_id   = 0;
};

// Descriptor 0x56 — teletext. Multiple language entries (5 bytes each).
struct TeletextEntry {
    LanguageCode  language;
    std::uint8_t  teletext_type   = 0;  // EN 300 468 §6.2.43, e.g. 0x01 initial
    std::uint8_t  magazine_number = 0;  // 0..7
    std::uint8_t  page_number     = 0;  // BCD
};

// Descriptor 0x6A — DVB AC-3. Flags-driven; we only carry the raw bytes
// through PSI regen rather than re-encoding the flags byte exactly, since
// receivers parse it themselves.
struct Ac3Descriptor {
    std::uint8_t              component_type_flag : 1 = 0;
    std::uint8_t              bsid_flag           : 1 = 0;
    std::uint8_t              mainid_flag         : 1 = 0;
    std::uint8_t              asvc_flag           : 1 = 0;
    std::optional<std::uint8_t> component_type;
    std::optional<std::uint8_t> bsid;
    std::optional<std::uint8_t> mainid;
    std::optional<std::uint8_t> asvc;
    std::vector<std::uint8_t> additional_info;  // remaining body, opaque
};

// Generic descriptor — keeps the raw bytes verbatim. We always store this
// alongside any parsed view so PSI regen can pass unknown descriptors
// through byte-for-byte.
struct RawDescriptor {
    std::uint8_t              tag = 0;
    std::vector<std::uint8_t> body;  // length-prefix is implicit in body.size()
};

// Parse a descriptor loop. Walks `tag, length, body[length]` triples until
// the span is exhausted. Returns the parsed list; on truncation (length
// byte points past end) the parse stops and returns what it has so far —
// the caller decides whether to reject the table (CRC will usually catch
// it first anyway).
std::vector<RawDescriptor> parseDescriptorLoop(std::span<const std::uint8_t> data);

// Specialised parsers — each returns std::nullopt if the descriptor body is
// malformed (insufficient bytes, etc).
std::vector<SubtitlingEntry> parseSubtitlingDescriptor(std::span<const std::uint8_t> body);
std::vector<TeletextEntry>   parseTeletextDescriptor  (std::span<const std::uint8_t> body);
std::optional<Ac3Descriptor> parseAc3Descriptor       (std::span<const std::uint8_t> body);

// Helper: extract the first ISO 639 language code (descriptor 0x0A) from a
// descriptor loop. Many audio/subtitle PIDs carry this for receivers that
// don't parse 0x59/0x56. Returns lowercase 3-letter code or empty on miss.
std::optional<LanguageCode> findIso639Language(std::span<const RawDescriptor> descs);

}  // namespace liveqx::gateway::ts
