#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "gateway/ts/Descriptors.h"
#include "gateway/ts/PsiParser.h"
#include "gateway/ts/TsPacket.h"

namespace liveqx::gateway::ts {

// Inverse of PsiParser — emits valid PSI sections + TS packets for PAT/PMT/SDT.
//
// Used by DemuxGateway (fix-A2): each output SPTS gets a freshly-built PAT
// (single program), PMT with optionally-remapped PIDs, and SDT subset.
// fix-A4 will extend this with EIT regeneration.
//
// Section format (ISO 13818-1 §2.4.4):
//   pointer_field(1) + table_id(1) + section_syntax/length(2)
//     + table-specific header(N) + body + CRC32(4)
// The pointer_field is *not* part of the section — it lives in the TS packet
// payload and is handled by packetizeSection().
//
// All builders produce a single section ≤ 1021 bytes (PAT/PMT) or ≤ 1024
// bytes (SDT). For larger SDTs the caller must pre-split services into
// multiple SdtBuildInputs (rare in practice).

// ─── PAT ────────────────────────────────────────────────────────────────────

struct PatBuildInput {
    std::uint16_t transport_stream_id   = 0;
    std::uint8_t  version_number        = 0;       // 5-bit, wraps 0..31
    bool          current_next_indicator = true;
    std::uint16_t network_pid            = 0;       // 0 = no NIT entry
    std::vector<PatEntry> programs;                 // {program_number, pmt_pid}
};

std::vector<std::uint8_t> buildPatSection(const PatBuildInput& in);

// ─── PMT ────────────────────────────────────────────────────────────────────

struct PmtBuildInput {
    std::uint16_t program_number         = 0;
    std::uint8_t  version_number         = 0;
    bool          current_next_indicator = true;
    std::uint16_t pcr_pid                = 0x1FFF;
    std::vector<RawDescriptor> program_descriptors;
    std::vector<PmtStream>     streams;             // re-uses parser struct
};

std::vector<std::uint8_t> buildPmtSection(const PmtBuildInput& in);

// ─── SDT ────────────────────────────────────────────────────────────────────

struct SdtBuildInput {
    std::uint16_t transport_stream_id   = 0;
    std::uint16_t original_network_id   = 0;
    std::uint8_t  version_number        = 0;
    bool          current_next_indicator = true;
    bool          actual                 = true;    // 0x42 actual / 0x46 other
    std::vector<SdtService> services;               // raw descriptors only
};

std::vector<std::uint8_t> buildSdtSection(const SdtBuildInput& in);

// ─── EIT ────────────────────────────────────────────────────────────────────
//
// fix40 A4 — EIT regeneration. Inverse of parseEit. table_id selects:
//   * 0x4E — EIT actual present/following (this TS)
//   * 0x4F — EIT other present/following  (other TS)
//   * 0x50–0x5F — EIT actual schedule
//   * 0x60–0x6F — EIT other schedule
// segment_last_section_number / last_table_id default to single-section
// table_id_extension == service_id; section_length is patched after CRC.
//
// Section size limit is 4093 bytes per ISO 13818-1; large schedule tables
// must be pre-split by the caller (we never exceed PSI MTU in tests).

struct EitBuildInput {
    std::uint8_t  table_id              = 0x4E;
    std::uint16_t service_id            = 0;
    std::uint16_t transport_stream_id   = 0;
    std::uint16_t original_network_id   = 0;
    std::uint8_t  version_number        = 0;
    bool          current_next_indicator = true;
    std::vector<EitEvent> events;
};

std::vector<std::uint8_t> buildEitSection(const EitBuildInput& in);

// MJD+UTC encoder — inverse of decodeMjdUtc. unix_time must fit the MJD
// encodable range (1858-11-17 .. 2038-04-22 ≈ 65535 days). Returns 5 zeros
// for unix_time == 0 (start_time_undefined per spec).
void encodeMjdUtc(std::uint64_t unix_time, std::span<std::uint8_t, 5> out) noexcept;

// BCD HH:MM:SS encoder — inverse of decodeBcdDuration. duration_sec is
// clamped to 24*3600-1 (0..86399) so the BCD field never overflows.
void encodeBcdDuration(std::uint32_t duration_sec,
                       std::span<std::uint8_t, 3> out) noexcept;

// ─── Service descriptor (0x48) helper ──────────────────────────────────────
//
// SdtService.descriptors carries opaque RawDescriptors. When regenerating an
// SDT for an SPTS demux output we usually want to *rebuild* the 0x48 from
// service_name + provider_name (the strings the parser decoded into UTF-8) so
// the operator can override branding without touching raw byte buffers.
//
// service_type follows EN 300 468 §6.2.33: 0x01 = digital TV, 0x02 = digital
// radio, 0x16 = H.264 SD, 0x19 = H.264 HD, 0x1F = HEVC. Caller picks based on
// the program's video stream_type.
RawDescriptor makeServiceDescriptor(std::uint8_t service_type,
                                    std::string_view provider_name,
                                    std::string_view service_name);

// ─── Section packetization ─────────────────────────────────────────────────
//
// Wraps a finished section into one or more 188-byte TS packets:
//   * PUSI=1 on the first packet, pointer_field=0
//   * PID matches the table type (0x0000 PAT / pmt_pid PMT / 0x0011 SDT)
//   * adaptation_field_control = PayloadOnly; remaining bytes after the
//     section are 0xFF stuffing (required by spec §2.4.4)
//   * continuity_counter increments across packets; the caller passes a CC
//     reference so successive sections continue the count properly.
//
// For sections that fit into a single TS payload (≤183 bytes — 184 minus
// pointer_field) the result is one packet. Larger sections produce multiple
// packets, each carrying CC = (cc++)&0xF.
std::vector<std::array<std::uint8_t, kTsPacketSize>>
packetizeSection(std::uint16_t pid,
                 std::span<const std::uint8_t> section,
                 std::uint8_t& cc) noexcept;

}  // namespace liveqx::gateway::ts
