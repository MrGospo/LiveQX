#include "gateway/ts/ProgramTable.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace liveqx::gateway::ts {
namespace {

// Decode stream_type into a short codec name for display. Comprehensive enough
// to cover modern broadcast (H.264, HEVC, AAC, AC-3, E-AC-3, MP2A) without
// bringing in libavformat just for the lookup.
std::string codecNameFor(std::uint8_t stream_type) noexcept {
    switch (stream_type) {
        case 0x01: case 0x02: return "MPEG-2 Video";
        case 0x03: case 0x04: return "MPEG-1/2 Audio";
        case 0x0F:            return "AAC";
        case 0x11:            return "AAC LATM";
        case 0x1B:            return "H.264";
        case 0x24:            return "HEVC";
        case 0x81:            return "AC-3";
        case 0x87:            return "E-AC-3";
        case 0x06:            return "PES private";   // commonly DVB subtitle/teletext
        default:              return "stream_type 0x" + std::to_string(stream_type);
    }
}

bool snapshotEqual(const ProgramTableSnapshot& a, const ProgramTableSnapshot& b) noexcept {
    if (a.transport_stream_id != b.transport_stream_id) return false;
    if (a.original_network_id != b.original_network_id) return false;
    if (a.pat_version != b.pat_version) return false;
    if (a.programs.size() != b.programs.size()) return false;
    for (std::size_t i = 0; i < a.programs.size(); ++i) {
        const auto& x = a.programs[i];
        const auto& y = b.programs[i];
        if (x.service_id != y.service_id) return false;
        if (x.pmt_pid    != y.pmt_pid)    return false;
        if (x.pcr_pid    != y.pcr_pid)    return false;
        if (x.pmt_version != y.pmt_version) return false;
        if (x.service_name != y.service_name) return false;
        if (x.provider_name != y.provider_name) return false;
        if (x.running_status != y.running_status) return false;
        if (x.eit_present != y.eit_present) return false;
        if (x.discovered != y.discovered) return false;
        if (x.streams.size() != y.streams.size()) return false;
        for (std::size_t j = 0; j < x.streams.size(); ++j) {
            const auto& xs = x.streams[j];
            const auto& ys = y.streams[j];
            if (xs.stream_type != ys.stream_type) return false;
            if (xs.elementary_pid != ys.elementary_pid) return false;
            if (xs.language != ys.language) return false;
            if (xs.has_subtitling != ys.has_subtitling) return false;
            if (xs.has_teletext != ys.has_teletext) return false;
        }
    }
    return true;
}

// FNV-1a 64-bit. Cheap and sufficient for warm-hint sanity checking.
std::uint64_t fnvMix(std::uint64_t h, std::uint64_t v) noexcept {
    h ^= v;
    h *= 1099511628211ULL;
    return h;
}

}  // namespace

const ProgramInfo* ProgramTableSnapshot::find(std::uint16_t service_id) const noexcept {
    for (const auto& p : programs) {
        if (p.service_id == service_id) return &p;
    }
    return nullptr;
}

ProgramTable::ProgramTable()
    : snapshot_(std::make_shared<ProgramTableSnapshot>()) {}

void ProgramTable::clear() {
    republish(std::make_shared<ProgramTableSnapshot>());
}

void ProgramTable::republish(std::shared_ptr<ProgramTableSnapshot> next) noexcept {
    auto prev = std::atomic_load_explicit(&snapshot_, std::memory_order_acquire);
    if (prev && next && snapshotEqual(*prev, *next)) return;
    std::atomic_store_explicit(&snapshot_, std::move(next), std::memory_order_release);
}

void ProgramTable::ingestPat(const ParsedPat& pat) {
    auto curr = current();
    auto next = std::make_shared<ProgramTableSnapshot>(*curr);

    next->transport_stream_id = pat.transport_stream_id;
    next->pat_version         = pat.version_number;

    // Reconcile programs:
    //   * For each (program_number, pmt_pid) in PAT, ensure entry exists.
    //   * Drop programs that disappeared from PAT — their stale PMT data
    //     is forgotten so the router stops trying to route them.
    std::vector<ProgramInfo> reconciled;
    reconciled.reserve(pat.programs.size());
    for (const auto& e : pat.programs) {
        // Carry forward PMT/SDT details if we already had this service.
        ProgramInfo info;
        info.service_id = e.program_number;
        info.pmt_pid    = e.pmt_pid;
        if (auto* existing = curr->find(e.program_number); existing && existing->pmt_pid == e.pmt_pid) {
            info = *existing;  // keep streams, names, etc.
            info.pmt_pid = e.pmt_pid;
        }
        reconciled.push_back(std::move(info));
    }
    next->programs = std::move(reconciled);
    republish(std::move(next));
}

void ProgramTable::ingestPmt(const ParsedPmt& pmt) {
    auto curr = current();
    auto next = std::make_shared<ProgramTableSnapshot>(*curr);

    auto it = std::find_if(next->programs.begin(), next->programs.end(),
                           [&](const ProgramInfo& p) {
                               return p.service_id == pmt.program_number;
                           });
    // PMT for an unknown program — could happen during a PAT/PMT cross-over
    // when PMT version_number bumps before PAT does. Add the entry anyway;
    // PAT will reconcile shortly.
    if (it == next->programs.end()) {
        next->programs.push_back(ProgramInfo{});
        it = std::prev(next->programs.end());
        it->service_id = pmt.program_number;
    }

    it->pmt_version = pmt.version_number;
    it->pcr_pid     = pmt.pcr_pid;
    it->discovered  = true;

    it->streams.clear();
    it->streams.reserve(pmt.streams.size());
    for (const auto& s : pmt.streams) {
        ProgramInfo::Stream out;
        out.stream_type    = s.stream_type;
        out.elementary_pid = s.elementary_pid;
        out.codec_name     = codecNameFor(s.stream_type);

        if (auto lang = findIso639Language(s.es_descriptors); lang) {
            out.language = lang->toString();
        }
        for (const auto& d : s.es_descriptors) {
            if (d.tag == kDescSubtitling) out.has_subtitling = true;
            if (d.tag == kDescTeletext)   out.has_teletext   = true;
        }
        it->streams.push_back(std::move(out));
    }
    republish(std::move(next));
}

void ProgramTable::ingestSdt(const ParsedSdt& sdt) {
    auto curr = current();
    auto next = std::make_shared<ProgramTableSnapshot>(*curr);

    if (next->transport_stream_id == 0)
        next->transport_stream_id = sdt.transport_stream_id;
    next->original_network_id = sdt.original_network_id;

    for (const auto& svc : sdt.services) {
        auto it = std::find_if(next->programs.begin(), next->programs.end(),
                               [&](const ProgramInfo& p) {
                                   return p.service_id == svc.service_id;
                               });
        if (it == next->programs.end()) continue;  // SDT for a service not in PAT — ignore
        it->service_name   = svc.service_name;
        it->provider_name  = svc.provider_name;
        it->running_status = svc.running_status;
        it->eit_present    = svc.eit_present_following_flag;
    }
    republish(std::move(next));
}

std::uint64_t ProgramTable::signature() const noexcept {
    auto snap = current();
    std::uint64_t h = 1469598103934665603ULL;  // FNV offset basis
    h = fnvMix(h, snap->transport_stream_id);
    h = fnvMix(h, snap->programs.size());
    // Sort service_ids for deterministic hash regardless of PAT ordering.
    std::vector<std::pair<std::uint16_t, std::uint16_t>> ids;
    ids.reserve(snap->programs.size());
    for (const auto& p : snap->programs) ids.emplace_back(p.service_id, p.pmt_pid);
    std::sort(ids.begin(), ids.end());
    for (auto [sid, pid] : ids) {
        h = fnvMix(h, sid);
        h = fnvMix(h, pid);
    }
    return h;
}

}  // namespace liveqx::gateway::ts
