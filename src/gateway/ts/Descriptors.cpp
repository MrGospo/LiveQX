#include "gateway/ts/Descriptors.h"

#include <cctype>

namespace liveqx::gateway::ts {
namespace {

LanguageCode readLang(const std::uint8_t* p) noexcept {
    LanguageCode l{};
    for (int i = 0; i < 3; ++i) {
        // Lowercase normalisation. EN 300 468 says ISO 639-2 lowercase but
        // some sources emit uppercase — we make it deterministic so
        // downstream comparisons (e.g. "select language=rus") work.
        l.code[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(p[i])));
    }
    return l;
}

}  // namespace

std::vector<RawDescriptor> parseDescriptorLoop(std::span<const std::uint8_t> data) {
    std::vector<RawDescriptor> out;
    std::size_t i = 0;
    while (i + 2 <= data.size()) {
        const std::uint8_t tag = data[i];
        const std::uint8_t len = data[i + 1];
        if (i + 2 + len > data.size()) break;  // truncated — bail
        RawDescriptor d;
        d.tag = tag;
        d.body.assign(data.begin() + i + 2, data.begin() + i + 2 + len);
        out.push_back(std::move(d));
        i += 2u + len;
    }
    return out;
}

std::vector<SubtitlingEntry> parseSubtitlingDescriptor(std::span<const std::uint8_t> body) {
    std::vector<SubtitlingEntry> out;
    // Each entry is exactly 8 bytes: lang(3) + subtitling_type(1)
    //                              + composition_page_id(2) + ancillary_page_id(2).
    constexpr std::size_t kEntrySize = 8;
    for (std::size_t i = 0; i + kEntrySize <= body.size(); i += kEntrySize) {
        SubtitlingEntry e;
        e.language            = readLang(body.data() + i);
        e.subtitling_type     = body[i + 3];
        e.composition_page_id = static_cast<std::uint16_t>((body[i + 4] << 8) | body[i + 5]);
        e.ancillary_page_id   = static_cast<std::uint16_t>((body[i + 6] << 8) | body[i + 7]);
        out.push_back(e);
    }
    return out;
}

std::vector<TeletextEntry> parseTeletextDescriptor(std::span<const std::uint8_t> body) {
    std::vector<TeletextEntry> out;
    // Each entry is 5 bytes: lang(3) + (teletext_type 5b | magazine 3b)(1) + page(1).
    constexpr std::size_t kEntrySize = 5;
    for (std::size_t i = 0; i + kEntrySize <= body.size(); i += kEntrySize) {
        TeletextEntry e;
        e.language        = readLang(body.data() + i);
        e.teletext_type   = static_cast<std::uint8_t>((body[i + 3] >> 3) & 0x1F);
        e.magazine_number = static_cast<std::uint8_t>(body[i + 3] & 0x07);
        e.page_number     = body[i + 4];
        out.push_back(e);
    }
    return out;
}

std::optional<Ac3Descriptor> parseAc3Descriptor(std::span<const std::uint8_t> body) {
    if (body.empty()) return std::nullopt;
    Ac3Descriptor d;
    const std::uint8_t flags = body[0];
    d.component_type_flag = (flags & 0x80) ? 1 : 0;
    d.bsid_flag           = (flags & 0x40) ? 1 : 0;
    d.mainid_flag         = (flags & 0x20) ? 1 : 0;
    d.asvc_flag           = (flags & 0x10) ? 1 : 0;

    std::size_t i = 1;
    auto take = [&](std::optional<std::uint8_t>& dst, bool flag) -> bool {
        if (!flag) return true;
        if (i >= body.size()) return false;
        dst = body[i++];
        return true;
    };
    if (!take(d.component_type, d.component_type_flag)) return std::nullopt;
    if (!take(d.bsid,           d.bsid_flag))           return std::nullopt;
    if (!take(d.mainid,         d.mainid_flag))         return std::nullopt;
    if (!take(d.asvc,           d.asvc_flag))           return std::nullopt;

    if (i < body.size()) d.additional_info.assign(body.begin() + i, body.end());
    return d;
}

std::optional<LanguageCode> findIso639Language(std::span<const RawDescriptor> descs) {
    for (const auto& d : descs) {
        if (d.tag == kDescIso639Language && d.body.size() >= 3) {
            return readLang(d.body.data());
        }
    }
    return std::nullopt;
}

}  // namespace liveqx::gateway::ts
