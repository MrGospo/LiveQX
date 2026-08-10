#include "gateway/ts/Crc32.h"

#include <array>

namespace liveqx::gateway::ts {
namespace {

// Generated at compile time so we don't pay an init cost or risk a static-init
// race when the parser is exercised before main() (unit-test fixtures).
constexpr std::array<std::uint32_t, 256> buildTable() noexcept {
    std::array<std::uint32_t, 256> t{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t c = i << 24;
        for (int k = 0; k < 8; ++k) {
            c = (c & 0x80000000u) ? (c << 1) ^ 0x04C11DB7u : (c << 1);
        }
        t[i] = c;
    }
    return t;
}

constexpr auto kTable = buildTable();

}  // namespace

std::uint32_t crc32Mpeg2(std::span<const std::uint8_t> data, std::uint32_t init) noexcept {
    std::uint32_t crc = init;
    for (auto b : data) {
        crc = (crc << 8) ^ kTable[((crc >> 24) ^ b) & 0xFFu];
    }
    return crc;
}

}  // namespace liveqx::gateway::ts
