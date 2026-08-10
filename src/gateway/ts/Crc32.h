#pragma once

#include <cstdint>
#include <span>

namespace liveqx::gateway::ts {

// MPEG-2 Systems CRC32 (ISO/IEC 13818-1 Annex A).
//
// Polynomial: 0x04C11DB7, MSB-first, initial value 0xFFFFFFFF, no final XOR,
// no reflection. This is NOT the same as Ethernet/zlib CRC32 — those use
// reflected input/output and final XOR.
//
// Used to validate every PSI section (PAT/PMT/SDT/EIT). A mismatched CRC
// rejects the section silently — the next valid copy will arrive within
// one PMT period (≤100 ms in DVB).
//
// Table-driven byte-at-a-time impl, ~1 GiB/s on a single core. Hot path is
// in PsiParser::feed — avoid touching this in tight loops, parse once per
// section reassembly.

std::uint32_t crc32Mpeg2(std::span<const std::uint8_t> data, std::uint32_t init = 0xFFFFFFFFu) noexcept;

}  // namespace liveqx::gateway::ts
