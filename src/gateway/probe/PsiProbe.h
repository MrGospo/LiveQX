#pragma once
//
// One-shot MPEG-TS probe: open a UDP RX socket, read for a bounded interval,
// parse PAT / PMT / SDT out of the stream, and return a structured summary
// (transport-stream id, list of programs with service names and elementary
// streams, per-PID and total bitrate).
//
// Called by the /api/streams/probe REST handler when the operator wants to
// preview what programs a given multicast carries before wiring outputs.
// The socket is opened, drained, and closed — no gateway state is touched.

#include <cstdint>
#include <string>
#include <vector>

#include "gateway/net/UdpSocket.h"

namespace liveqx::gateway::probe {

struct ProbeStreamInfo {
    std::uint16_t pid            = 0;
    std::uint8_t  stream_type    = 0;   // ISO/IEC 13818-1 Table 2-34
    std::string   codec;                // human-readable, e.g. "H.264", "AAC"
    std::string   language;             // ISO 639-2 or "" if not advertised
    std::uint64_t bitrate_bps    = 0;
};

struct ProbeProgramInfo {
    std::uint16_t program_number = 0;
    std::uint16_t pmt_pid        = 0;
    std::uint16_t pcr_pid        = 0x1FFF;
    std::string   service_name;         // from SDT, "" if SDT never arrived
    std::string   provider_name;
    std::vector<ProbeStreamInfo> streams;
};

struct ProbeResult {
    bool          success               = false;
    std::string   error;                // populated iff !success
    std::uint16_t transport_stream_id   = 0;
    std::uint16_t original_network_id   = 0;
    std::uint64_t total_bitrate_bps     = 0;
    std::uint64_t bytes_received        = 0;
    std::uint64_t packets_received      = 0;   // TS packets, not UDP datagrams
    int           duration_ms           = 0;   // actual sampling window
    std::vector<ProbeProgramInfo> programs;
};

struct ProbeOptions {
    net::UdpRxOptions socket;
    // How long to sample before returning. Kept short by default so a
    // synchronous REST call doesn't tie up an httplib worker for too long.
    int duration_ms = 3000;
    // Cap the total number of TS packets we bother to inspect, so a
    // pathologically high-bitrate stream can't exhaust memory or CPU.
    // 0 = no cap (limited only by duration_ms + kernel RCV buffer).
    std::uint64_t max_packets = 200000;
};

// Blocking. Returns a ProbeResult; on socket-open failure success=false and
// error carries the reason. On successful open the result is always success=true,
// even when no packets arrived — the caller reads packets_received to tell
// "empty stream" from "healthy stream".
ProbeResult probe(const ProbeOptions& opts);

// Convert stream_type byte to a short human label (e.g. 0x1B → "H.264").
// Unknowns render as "type 0xNN" so the UI still shows something meaningful.
std::string streamTypeLabel(std::uint8_t stream_type);

}  // namespace liveqx::gateway::probe
