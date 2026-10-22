#pragma once
//
// Case-insensitive search over UTF-8 text without a locale or ICU.
//
// foldForSearch() lower-cases Latin (A-Z) and Cyrillic (А-Я, Ё and the
// Ukrainian/Belarusian/Serbian capitals U+0400..U+042F) and leaves every other
// character as it is, so "касса" finds "Касса" and "TILL" finds "till". Text in
// other alphabets is compared exactly: say so where the search is offered.
// Malformed UTF-8 passes through byte by byte (nothing is dropped or guessed).

#include <cstdint>
#include <string>

namespace liveqx::text {

inline std::string foldForSearch(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size();) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        if (c < 0x80) {                                           // ASCII
            out.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : static_cast<char>(c));
            ++i;
            continue;
        }
        // Two-byte sequences carry all of Cyrillic (U+0400..U+04FF).
        if ((c & 0xE0) == 0xC0 && i + 1 < in.size() && (static_cast<unsigned char>(in[i + 1]) & 0xC0) == 0x80) {
            std::uint32_t cp = ((c & 0x1Fu) << 6) | (static_cast<unsigned char>(in[i + 1]) & 0x3Fu);
            if (cp >= 0x0410 && cp <= 0x042F) cp += 0x20;          // А-Я -> а-я
            else if (cp >= 0x0400 && cp <= 0x040F) cp += 0x50;     // Ѐ-Џ (Ё, Є, І, Ї, Ј...) -> ѐ-џ
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            i += 2;
            continue;
        }
        out.push_back(static_cast<char>(c));                      // anything else: as it is
        ++i;
    }
    return out;
}

}  // namespace liveqx::text
