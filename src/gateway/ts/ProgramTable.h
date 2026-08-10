#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "gateway/ts/PsiParser.h"

namespace liveqx::gateway::ts {

// Distilled view of one program — the subset of PMT info that demux/remux/
// transcode actually need at the routing layer. Built from a ParsedPmt by
// ProgramTable::ingestPmt().
struct ProgramInfo {
    std::uint16_t service_id     = 0;        // == PAT program_number
    std::uint16_t pmt_pid        = 0;
    std::uint16_t pcr_pid        = 0x1FFF;
    std::uint8_t  pmt_version    = 0;

    // Optional service metadata cross-referenced from the SDT once it arrives.
    std::string   service_name;
    std::string   provider_name;
    std::uint8_t  running_status = 0;  // EN 300 468 §6.2.39
    bool          eit_present    = false;

    struct Stream {
        std::uint8_t  stream_type    = 0;
        std::uint16_t elementary_pid = 0;
        // Decoded language (descriptor 0x0A) — empty if absent.
        std::string   language;

        // Subtitle / teletext detection — set if descriptor 0x59 / 0x56 is
        // present. UI uses these to badge programs in the Programs tab.
        bool has_subtitling = false;
        bool has_teletext   = false;

        // Codec hint derived from stream_type + descriptors. Used purely for
        // display; transcode uses the libav stream_id directly.
        std::string codec_name;
    };
    std::vector<Stream> streams;

    // Discovery completeness. discovered=false means we know the program
    // exists (from PAT) but have not yet seen its PMT — demux can route the
    // PAT itself but cannot remap PIDs yet.
    bool discovered = false;
};

// Atomic snapshot of the current MPTS layout. Readers (router thread, SSE
// snapshot endpoint) call current() — returns a shared_ptr to an immutable
// snapshot, so iteration is safe without locks. Writers (parser callback)
// call ingestPat / ingestPmt / ingestSdt and a new snapshot is published
// atomically.
//
// Snapshot identity is preserved across PMT updates that don't actually
// change anything: equal() comparison short-circuits the swap, so the
// router's `current_table_ != prev_table_` check stays cheap.

struct ProgramTableSnapshot {
    std::uint16_t transport_stream_id   = 0;
    std::uint16_t original_network_id   = 0;
    std::uint8_t  pat_version           = 0;
    std::vector<ProgramInfo> programs;

    // Lookup by service_id. Returns nullptr if not found.
    const ProgramInfo* find(std::uint16_t service_id) const noexcept;
};

class ProgramTable {
public:
    ProgramTable();

    // Feed a parsed table. Each call may republish the snapshot if state
    // changed; equal-payload calls are no-ops.
    void ingestPat(const ParsedPat& pat);
    void ingestPmt(const ParsedPmt& pmt);
    void ingestSdt(const ParsedSdt& sdt);

    // Hot path — readers grab the current snapshot and iterate without locks.
    std::shared_ptr<const ProgramTableSnapshot> current() const noexcept {
        return std::atomic_load_explicit(&snapshot_, std::memory_order_acquire);
    }

    // For warm-hint persistence (programs.json on disk). Sanity-check
    // signature: a stable hash over (transport_stream_id, sorted service_ids
    // and their PMT PIDs). The router validates the persisted hint against
    // the first observed PAT before trusting it for routing.
    std::uint64_t signature() const noexcept;

    // Drop everything — used on CC discontinuity recovery or explicit
    // operator-triggered rediscovery (POST /programs/refresh).
    void clear();

private:
    void republish(std::shared_ptr<ProgramTableSnapshot> next) noexcept;

    std::shared_ptr<ProgramTableSnapshot> snapshot_;
};

}  // namespace liveqx::gateway::ts
