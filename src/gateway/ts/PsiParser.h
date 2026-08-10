#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "gateway/ts/Descriptors.h"
#include "gateway/ts/TsPacket.h"

namespace liveqx::gateway::ts {

// ─── Section reassembly ──────────────────────────────────────────────────────
//
// PSI sections can span multiple TS packets. The protocol uses a `pointer_field`
// in the first byte of every PUSI'd payload to mark where a new section begins
// (anything before the pointer is the tail of the previous section).
//
// One assembler instance per gateway tracks state across all PIDs of interest.
// We keep a per-PID buffer because PAT/PMT/SDT/EIT can interleave and each may
// span multiple packets.
//
// CRC validation is the final gate — anything that fails CRC is dropped
// silently; the next valid copy arrives within one table period (≤100 ms in
// DVB).

class PsiSectionAssembler {
public:
    using SectionCb = std::function<void(std::uint16_t pid, std::span<const std::uint8_t> section)>;

    // Feed a single TS packet. The callback fires zero or more times — sections
    // smaller than one packet's payload deliver immediately, larger sections
    // accumulate until complete.
    void feed(TsPacketView pkt, const SectionCb& cb);

    // Forget any partially-assembled section for the given PID. Called by
    // PsiParser::clear() and on CC discontinuity (we can't trust the buffered
    // bytes if we missed packets in the middle).
    void reset(std::uint16_t pid);
    void clearAll();

private:
    struct PerPidState {
        std::vector<std::uint8_t> buffer;       // accumulated section bytes
        std::size_t               expected = 0; // total section size (header + body + CRC)
        std::uint8_t              last_cc = 0xFF; // 0xFF = no packet yet; else 4-bit CC
        bool                      in_progress = false;
    };

    PerPidState& state(std::uint16_t pid);
    std::unordered_map<std::uint16_t, PerPidState> pids_;
};

// ─── Parsed-table structs ────────────────────────────────────────────────────
//
// All structs are POD-ish (std::vector members for variable sections). They
// own their data so callers can store snapshots without lifetime concerns.

struct PatEntry {
    std::uint16_t program_number = 0;
    std::uint16_t pmt_pid        = 0;  // when program_number == 0 this is network_pid
};

struct ParsedPat {
    std::uint16_t transport_stream_id   = 0;
    std::uint8_t  version_number         = 0;
    bool          current_next_indicator = false;
    std::uint16_t network_pid            = 0;  // 0 = absent
    std::vector<PatEntry> programs;
};

struct PmtStream {
    std::uint8_t  stream_type    = 0;  // ISO/IEC 13818-1 Table 2-34
    std::uint16_t elementary_pid = 0;
    std::vector<RawDescriptor> es_descriptors;
};

struct ParsedPmt {
    std::uint16_t program_number         = 0;
    std::uint8_t  version_number         = 0;
    bool          current_next_indicator = false;
    std::uint16_t pcr_pid                = 0x1FFF;  // 0x1FFF = no PCR for this program
    std::vector<RawDescriptor> program_descriptors;
    std::vector<PmtStream>     streams;
};

struct SdtService {
    std::uint16_t service_id                  = 0;
    bool          eit_schedule_flag           = false;
    bool          eit_present_following_flag  = false;
    std::uint8_t  running_status              = 0;
    bool          free_ca_mode                = false;
    std::string   service_name;   // UTF-8
    std::string   provider_name;  // UTF-8
    std::vector<RawDescriptor> descriptors;
};

struct ParsedSdt {
    std::uint16_t transport_stream_id   = 0;
    std::uint16_t original_network_id   = 0;
    std::uint8_t  version_number         = 0;
    bool          current_next_indicator = false;
    std::vector<SdtService> services;
};

struct EitEvent {
    std::uint16_t event_id        = 0;
    std::uint64_t start_time_utc  = 0;  // unix sec; 0 = "undefined"
    std::uint32_t duration_sec    = 0;
    std::uint8_t  running_status  = 0;
    bool          free_ca_mode    = false;
    std::vector<RawDescriptor> descriptors;
};

struct ParsedEit {
    std::uint16_t service_id          = 0;
    std::uint8_t  table_id            = 0;
    std::uint16_t transport_stream_id = 0;
    std::uint16_t original_network_id = 0;
    std::uint8_t  version_number      = 0;
    bool          actual              = false;  // table_id 0x4E/0x50-0x5F vs 0x4F/0x60-0x6F
    bool          present_following   = false;  // 0x4E/0x4F (vs schedule)
    std::vector<EitEvent> events;
};

// ─── Section parsers ─────────────────────────────────────────────────────────
//
// Each parser validates CRC32 and structural invariants. Returns std::nullopt
// for any anomaly; the assembler will pick up the next valid copy of the
// table on the next period.

std::optional<ParsedPat> parsePat(std::span<const std::uint8_t> section);
std::optional<ParsedPmt> parsePmt(std::span<const std::uint8_t> section);
std::optional<ParsedSdt> parseSdt(std::span<const std::uint8_t> section);
std::optional<ParsedEit> parseEit(std::span<const std::uint8_t> section);

// Decode a DVB text string (ETSI EN 300 468 Annex A) into UTF-8. Handles the
// optional first-byte character-set selector (0x01..0x1F) and falls back to
// ISO 6937 / Latin-1 when no selector is present. Control codes (0x86 / 0x87
// emphasis on/off) are stripped.
std::string decodeDvbText(std::span<const std::uint8_t> raw);

// Decode an MJD+UTC start time (5 bytes) into a unix timestamp. Used by EIT.
// Returns 0 on malformed inputs (start_time_undefined per spec).
std::uint64_t decodeMjdUtc(std::span<const std::uint8_t, 5> raw);

// Decode a BCD duration (3 bytes HH:MM:SS BCD) into seconds.
std::uint32_t decodeBcdDuration(std::span<const std::uint8_t, 3> raw);

}  // namespace liveqx::gateway::ts
