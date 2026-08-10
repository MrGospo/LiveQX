#include "gateway/probe/PsiProbe.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

#include <sys/socket.h>
#include <unistd.h>

#include "gateway/ts/PsiParser.h"
#include "gateway/ts/TsPacket.h"

namespace liveqx::gateway::probe {

namespace {

using ts::kTsPacketSize;
using ts::kTsSyncByte;
using ts::kPidPat;
using ts::kPidSdt;
using ts::TsPacketView;

// Detect where MPEG-TS packets begin inside a UDP datagram. Broadcasters send
// either raw TS (7×188 = 1316 bytes per datagram) or RTP-wrapped (12-byte RTP
// header + N×188). We test position 0 and, failing that, position 12 — matches
// what tcpdump / VLC / ffprobe do in practice.
std::size_t tsPayloadOffset(const std::uint8_t* buf, std::size_t len) noexcept {
    if (len >= kTsPacketSize && buf[0] == kTsSyncByte) return 0;
    if (len >= 12 + kTsPacketSize && buf[12] == kTsSyncByte) return 12;
    return len;  // no recognisable TS
}

const char* codecFromStreamType(std::uint8_t st) noexcept {
    switch (st) {
        case 0x01: return "MPEG-1 Video";
        case 0x02: return "MPEG-2 Video";
        case 0x03: return "MPEG-1 Audio";
        case 0x04: return "MPEG-2 Audio";
        case 0x0F: return "AAC";
        case 0x10: return "MPEG-4 Video";
        case 0x11: return "AAC LATM";
        case 0x15: return "Metadata";
        case 0x1B: return "H.264";
        case 0x24: return "H.265";
        case 0x81: return "AC-3";
        case 0x87: return "E-AC-3";
        case 0x06: return "Private/PES";  // often DVB subtitles, teletext, AC-3
        case 0x05: return "Private sections";
        default:   return nullptr;
    }
}

}  // namespace

std::string streamTypeLabel(std::uint8_t st) {
    if (const char* s = codecFromStreamType(st)) return s;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "type 0x%02X", st);
    return buf;
}

ProbeResult probe(const ProbeOptions& opts) {
    ProbeResult res;

    // Force a short recv timeout so the loop can honour duration_ms even when
    // there's zero traffic. Otherwise recv() would block forever on an empty
    // multicast group.
    net::UdpRxOptions sock_opts = opts.socket;
    if (sock_opts.rcv_timeout_ms <= 0 || sock_opts.rcv_timeout_ms > 200) {
        sock_opts.rcv_timeout_ms = 200;
    }

    std::string err;
    int fd = net::openRxSocket(sock_opts, &err);
    if (fd < 0) {
        res.success = false;
        res.error   = err;
        return res;
    }
    res.success = true;

    ts::PsiSectionAssembler assembler;

    // Per-PID byte counter (full TS packet size, matches how ffprobe reports
    // per-PID bitrate — packet header + payload, not just PES payload).
    std::unordered_map<std::uint16_t, std::uint64_t> bytes_per_pid;

    // Parsed table state — accumulated across the probe window. PAT+SDT are
    // small and repeat every ~100 ms in DVB; we keep the latest snapshot.
    std::optional<ts::ParsedPat>                     pat_latest;
    std::optional<ts::ParsedSdt>                     sdt_latest;
    std::unordered_map<std::uint16_t, ts::ParsedPmt> pmt_by_program;
    // PIDs we know are PMT PIDs (learned from PAT). Grows as PATs arrive.
    std::unordered_set<std::uint16_t> pmt_pids;

    std::array<std::uint8_t, 65536> buf{};
    const auto t0       = std::chrono::steady_clock::now();
    const auto deadline = t0 + std::chrono::milliseconds(opts.duration_ms);

    while (std::chrono::steady_clock::now() < deadline) {
        ssize_t n = ::recv(fd, buf.data(), buf.size(), 0);
        if (n <= 0) continue;  // EAGAIN via SO_RCVTIMEO — try again until deadline

        const auto off = tsPayloadOffset(buf.data(), static_cast<std::size_t>(n));
        for (std::size_t i = off;
             i + kTsPacketSize <= static_cast<std::size_t>(n);
             i += kTsPacketSize) {
            if (buf[i] != kTsSyncByte) break;  // desync — bail on this datagram
            std::span<const std::uint8_t, kTsPacketSize> pkt_bytes(
                buf.data() + i, kTsPacketSize);
            TsPacketView pkt(pkt_bytes);
            if (!pkt.isValidSync() || pkt.tei()) continue;

            const auto pid = pkt.pid();
            bytes_per_pid[pid] += kTsPacketSize;
            res.packets_received++;
            res.bytes_received += kTsPacketSize;

            const bool is_psi = (pid == kPidPat) || (pid == kPidSdt) ||
                                pmt_pids.count(pid) > 0;
            if (is_psi) {
                assembler.feed(pkt, [&](std::uint16_t sec_pid,
                                        std::span<const std::uint8_t> section) {
                    if (sec_pid == kPidPat) {
                        if (auto p = ts::parsePat(section)) {
                            pat_latest = *p;
                            for (const auto& e : p->programs) {
                                if (e.program_number != 0) pmt_pids.insert(e.pmt_pid);
                            }
                        }
                    } else if (sec_pid == kPidSdt) {
                        if (auto s = ts::parseSdt(section)) sdt_latest = *s;
                    } else {
                        if (auto m = ts::parsePmt(section)) {
                            pmt_by_program[m->program_number] = *m;
                        }
                    }
                });
            }

            if (opts.max_packets > 0 &&
                res.packets_received >= opts.max_packets) {
                goto done;
            }
        }
    }
done:
    ::close(fd);

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0).count();
    res.duration_ms = static_cast<int>(elapsed);

    // ─── Assemble ProbeResult from accumulated state ─────────────────────
    if (pat_latest) {
        res.transport_stream_id = pat_latest->transport_stream_id;
        for (const auto& e : pat_latest->programs) {
            if (e.program_number == 0) continue;  // NIT pointer, not a program
            ProbeProgramInfo prog;
            prog.program_number = e.program_number;
            prog.pmt_pid        = e.pmt_pid;
            auto it = pmt_by_program.find(e.program_number);
            if (it != pmt_by_program.end()) {
                const auto& pmt = it->second;
                prog.pcr_pid = pmt.pcr_pid;
                for (const auto& s : pmt.streams) {
                    ProbeStreamInfo si;
                    si.pid         = s.elementary_pid;
                    si.stream_type = s.stream_type;
                    si.codec       = streamTypeLabel(s.stream_type);
                    if (auto lang = ts::findIso639Language(s.es_descriptors)) {
                        si.language = lang->toString();
                    }
                    prog.streams.push_back(std::move(si));
                }
            }
            res.programs.push_back(std::move(prog));
        }
    }

    // Merge SDT service names into programs by matching service_id ==
    // program_number (standard DVB convention — Table 2-34 note).
    if (sdt_latest) {
        res.original_network_id = sdt_latest->original_network_id;
        for (auto& prog : res.programs) {
            for (const auto& svc : sdt_latest->services) {
                if (svc.service_id != prog.program_number) continue;
                prog.service_name  = svc.service_name;
                prog.provider_name = svc.provider_name;
                break;
            }
        }
    }

    // Bitrate: bytes*8 / seconds. Guard against duration==0 (e.g. max_packets
    // cap fired instantly on a tiny synthetic test) by treating <10 ms as 10 ms.
    const std::uint64_t denom_ms =
        std::max<std::int64_t>(elapsed, 10);
    auto bps = [denom_ms](std::uint64_t bytes) -> std::uint64_t {
        return (bytes * 8ull * 1000ull) / denom_ms;
    };
    res.total_bitrate_bps = bps(res.bytes_received);
    for (auto& prog : res.programs) {
        for (auto& s : prog.streams) {
            auto it = bytes_per_pid.find(s.pid);
            if (it != bytes_per_pid.end()) s.bitrate_bps = bps(it->second);
        }
    }

    return res;
}

}  // namespace liveqx::gateway::probe
