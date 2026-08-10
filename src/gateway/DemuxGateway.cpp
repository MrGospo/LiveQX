#include "gateway/DemuxGateway.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <utility>

#include <spdlog/spdlog.h>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "gateway/fec/FecEncoder.h"
#include "gateway/net/UdpSocket.h"
#include "gateway/ts/PsiBuilder.h"
#include "utils/CpuAffinity.h"

namespace liveqx::gateway {

namespace {

using ts::kTsPacketSize;
using ts::kPidPat;
using ts::kPidSdt;
using ts::kPidEit;
using ts::kPidNull;
using ts::TsPacketView;

// Up to 7 TS packets per UDP datagram (1316 bytes — fits in standard MTU
// without IP fragmentation). Receivers expect this on broadcast networks.
constexpr std::size_t kPacketsPerDatagram = 7;
constexpr std::size_t kPidSpace           = 8192;       // 2^13
constexpr std::size_t kRecvBufBytes       = 8192;       // ≥ kPacketsPerDatagram*188

// ─── RAII fd ─────────────────────────────────────────────────────────────────

class FdGuard {
public:
    FdGuard() = default;
    explicit FdGuard(int fd) noexcept : fd_(fd) {}
    FdGuard(const FdGuard&) = delete;
    FdGuard& operator=(const FdGuard&) = delete;
    FdGuard(FdGuard&& o) noexcept : fd_(std::exchange(o.fd_, -1)) {}
    FdGuard& operator=(FdGuard&& o) noexcept {
        reset(); fd_ = std::exchange(o.fd_, -1); return *this;
    }
    ~FdGuard() { reset(); }

    int  get()    const noexcept { return fd_; }
    bool valid()  const noexcept { return fd_ >= 0; }
    int  release() noexcept { return std::exchange(fd_, -1); }
    void reset(int fd = -1) noexcept {
        if (fd_ >= 0) ::close(fd_);
        fd_ = fd;
    }
private:
    int fd_ = -1;
};

bool isMulticast(const in_addr& a) noexcept {
    return (ntohl(a.s_addr) & 0xF0000000u) == 0xE0000000u;
}

int openInputSocket(const InputCfg& cfg, spdlog::logger& lg) {
    net::UdpRxOptions opts{
        .address        = cfg.address,
        .port           = cfg.port,
        .interface_name = cfg.interface_name,
        .interface_addr = cfg.interface_addr,
        .recv_buffer_kb = cfg.recv_buffer_kb,
        .rcv_timeout_ms = 200,
    };
    std::string err;
    int fd = net::openRxSocket(opts, &err);
    if (fd < 0) lg.error("DemuxGateway: input {}", err);
    return fd;
}

int openOutputSocket(const OutputCfg& cfg, sockaddr_in& dst_out,
                     spdlog::logger& lg) {
    in_addr dst{};
    if (::inet_pton(AF_INET, cfg.address.c_str(), &dst) != 1) {
        lg.error("DemuxGateway[{}]: invalid output address '{}'",
                 cfg.id, cfg.address);
        return -1;
    }
    FdGuard fd(::socket(AF_INET, SOCK_DGRAM, 0));
    if (!fd.valid()) {
        lg.error("DemuxGateway[{}]: output socket(): {}",
                 cfg.id, std::strerror(errno));
        return -1;
    }
    const int sndbuf = cfg.send_buffer_kb * 1024;
    ::setsockopt(fd.get(), SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    if (isMulticast(dst)) {
        const int ttl = cfg.ttl;
        ::setsockopt(fd.get(), IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
        in_addr ifaddr{}; bool have_if = false;
        if (!cfg.interface_addr.empty()) {
            ::inet_pton(AF_INET, cfg.interface_addr.c_str(), &ifaddr);
            have_if = true;
        } else if (!cfg.interface_name.empty()) {
            have_if = net::nicNameToIPv4(cfg.interface_name, ifaddr);
        }
        if (have_if) ::setsockopt(fd.get(), IPPROTO_IP, IP_MULTICAST_IF, &ifaddr, sizeof(ifaddr));
    } else if (!cfg.interface_name.empty()) {
        ::setsockopt(fd.get(), SOL_SOCKET, SO_BINDTODEVICE,
                     cfg.interface_name.c_str(),
                     static_cast<socklen_t>(cfg.interface_name.size()));
    }
    dst_out = sockaddr_in{};
    dst_out.sin_family = AF_INET;
    dst_out.sin_port   = htons(static_cast<std::uint16_t>(cfg.port));
    dst_out.sin_addr   = dst;
    return fd.release();
}

// service_type lookup from PMT stream_type. Used for the SDT regen 0x48
// service_descriptor we emit per service. EN 300 468 Table 87.
std::uint8_t serviceTypeFromStreams(const ts::ProgramInfo& p) noexcept {
    bool has_video = false, has_audio = false, hd = false, hevc = false;
    for (const auto& s : p.streams) {
        switch (s.stream_type) {
            case 0x01: case 0x02: case 0x10: has_video = true; break;
            case 0x1B: has_video = true; break;            // H.264
            case 0x24: has_video = true; hevc = true; break; // HEVC
            case 0x03: case 0x04: case 0x0F: case 0x11:
            case 0x81: has_audio = true; break;
            default: break;
        }
        // Resolution ≥ 720 lines is typically signalled by H.264/HEVC; we
        // can't easily tell SD vs HD without the actual stream header. Pick
        // HD for H.264/HEVC by convention.
        if (s.stream_type == 0x1B || s.stream_type == 0x24) hd = true;
    }
    if (hevc)            return 0x1F;          // HEVC digital TV
    if (hd && has_video) return 0x19;          // H.264 HD
    if (has_video)       return 0x01;          // SD digital TV
    if (has_audio)       return 0x02;          // digital radio
    return 0x01;
}

} // namespace

// ─── Output buffer ───────────────────────────────────────────────────────────

struct DemuxGateway::OutputBuffer {
    std::array<std::uint8_t, kPacketsPerDatagram * kTsPacketSize> bytes{};
    std::size_t fill = 0;

    bool full() const noexcept { return fill == bytes.size(); }
    void append(std::span<const std::uint8_t, kTsPacketSize> pkt) noexcept {
        std::memcpy(bytes.data() + fill, pkt.data(), kTsPacketSize);
        fill += kTsPacketSize;
    }
    void reset() noexcept { fill = 0; }
};

// ─── Sockets ─────────────────────────────────────────────────────────────────

struct DemuxGateway::Sockets {
    FdGuard                  in_fd;
    std::vector<FdGuard>     out_fds;
    std::vector<sockaddr_in> out_addrs;
    // Parallel sockaddrs for column / row FEC packets. Same fd as out_fds[i],
    // but a different destination port (cfg.fec.column_port_offset /
    // row_port_offset added to the media port). Empty when FEC is disabled.
    std::vector<sockaddr_in> out_addrs_col_fec;
    std::vector<sockaddr_in> out_addrs_row_fec;
};

// ─── Lifecycle ───────────────────────────────────────────────────────────────

DemuxGateway::DemuxGateway(int id, std::string name, GatewayCfg cfg)
    : id_(id), name_(std::move(name)), cfg_(std::move(cfg)),
      pid_table_(kPidSpace) {
    resolveOutputs();
}

DemuxGateway::~DemuxGateway() { DemuxGateway::stop(); }

void DemuxGateway::resolveOutputs() {
    outputs_.clear();
    outputs_.reserve(cfg_.outputs.size());
    for (std::size_t i = 0; i < cfg_.outputs.size(); ++i) {
        Output o;
        o.id = cfg_.outputs[i].id;
        o.buffer = std::make_unique<OutputBuffer>();
        // Default output PMT PID 0x100, 0x101, 0x102, … per output index.
        // pid_remap on the route can override.
        o.output_pmt_pid = static_cast<std::uint16_t>(0x0100 + i);
        outputs_.push_back(std::move(o));
    }
    // Bind each route to its output by id.
    std::unordered_map<std::string, std::size_t> id_to_idx;
    for (std::size_t i = 0; i < cfg_.outputs.size(); ++i)
        id_to_idx[cfg_.outputs[i].id] = i;
    for (const auto& r : cfg_.demux.routes) {
        auto it = id_to_idx.find(r.output_id);
        if (it == id_to_idx.end())
            throw std::invalid_argument(
                "DemuxGateway: route output_id '" + r.output_id + "' not found in outputs");
        outputs_[it->second].rule        = r;
        outputs_[it->second].service_id  = r.service_id;
        // Apply pmt_pid remap if present (from=0 means "PMT PID itself")
        for (auto [from, to] : r.pid_remap) {
            if (from == 0) {
                outputs_[it->second].output_pmt_pid = to;
            }
        }
    }
}

bool DemuxGateway::start() {
    if (running_.load(std::memory_order_acquire)) return false;
    if (cfg_.outputs.empty()) {
        if (logger_) logger_->error("DemuxGateway[{}]: no outputs", id_);
        return false;
    }

    auto& lg = logger_ ? *logger_ : *spdlog::default_logger();

    auto set = std::make_unique<Sockets>();
    const int in_fd = openInputSocket(cfg_.input, lg);
    if (in_fd < 0) return false;
    set->in_fd.reset(in_fd);

    set->out_fds.reserve(cfg_.outputs.size());
    set->out_addrs.reserve(cfg_.outputs.size());
    for (const auto& ocfg : cfg_.outputs) {
        sockaddr_in dst{};
        const int ofd = openOutputSocket(ocfg, dst, lg);
        if (ofd < 0) return false;
        set->out_fds.emplace_back(ofd);
        set->out_addrs.push_back(dst);
    }

    // Pre-build column / row FEC sockaddrs (same dst IP, port + offset). The
    // cfg parser already validated port + max_offset ≤ 65535 when FEC is on.
    if (cfg_.fec.enabled) {
        set->out_addrs_col_fec.reserve(cfg_.outputs.size());
        set->out_addrs_row_fec.reserve(cfg_.outputs.size());
        for (std::size_t i = 0; i < cfg_.outputs.size(); ++i) {
            sockaddr_in col = set->out_addrs[i];
            col.sin_port = htons(static_cast<std::uint16_t>(
                cfg_.outputs[i].port + cfg_.fec.column_port_offset));
            sockaddr_in row = set->out_addrs[i];
            row.sin_port = htons(static_cast<std::uint16_t>(
                cfg_.outputs[i].port + cfg_.fec.row_port_offset));
            set->out_addrs_col_fec.push_back(col);
            set->out_addrs_row_fec.push_back(row);
        }
    }

    sockets_ = std::move(set);

    // Construct per-output FEC encoders. Each encoder's media / column / row
    // sinks are bound to sendto on the same fd with the precomputed sockaddrs.
    // The encoder snapshots fd and sockaddr by value, so as long as we tear
    // it down before sockets_ in stop(), no dangling state.
    if (cfg_.fec.enabled) {
        const std::uint32_t base_ssrc =
            cfg_.fec.ssrc != 0
              ? cfg_.fec.ssrc
              : static_cast<std::uint32_t>(
                    std::chrono::steady_clock::now().time_since_epoch().count());
        for (std::size_t i = 0; i < outputs_.size(); ++i) {
            const int        fd      = sockets_->out_fds[i].get();
            const sockaddr_in media  = sockets_->out_addrs[i];
            const sockaddr_in col    = sockets_->out_addrs_col_fec[i];
            const sockaddr_in row    = cfg_.fec.mode == FecCfg::Mode::TwoD
                                        ? sockets_->out_addrs_row_fec[i]
                                        : sockaddr_in{};
            auto sendOn = [fd, this](const sockaddr_in& addr,
                                     std::span<const std::uint8_t> bytes) {
                const ssize_t s = ::sendto(
                    fd, bytes.data(), bytes.size(), MSG_DONTWAIT,
                    reinterpret_cast<const sockaddr*>(&addr),
                    sizeof(sockaddr_in));
                std::lock_guard<std::mutex> lk(stats_mu_);
                if (s == static_cast<ssize_t>(bytes.size())) {
                    stats_.bytes_out += bytes.size();
                } else {
                    stats_.drops += 1;
                }
            };
            const std::uint32_t out_seed =
                base_ssrc + static_cast<std::uint32_t>(i * 0x100u);
            outputs_[i].fec_encoder = std::make_unique<fec::FecEncoder>(
                cfg_.fec,
                /*media_ssrc=*/  out_seed,
                /*column_ssrc=*/ out_seed + 1u,
                /*row_ssrc=*/    out_seed + 2u,
                [media, sendOn](std::span<const std::uint8_t> p) { sendOn(media, p); },
                [col,   sendOn](std::span<const std::uint8_t> p) { sendOn(col,   p); },
                cfg_.fec.mode == FecCfg::Mode::TwoD
                    ? fec::FecEncoder::PacketSink(
                          [row, sendOn](std::span<const std::uint8_t> p) {
                              sendOn(row, p);
                          })
                    : fec::FecEncoder::PacketSink{});
        }
    }

    // Reset PSI state for clean rediscovery.
    psi_assembler_.clearAll();
    program_table_.clear();
    last_routed_snapshot_.reset();
    pid_table_.assign(kPidSpace, PidEntry{});

    stop_flag_.store(false, std::memory_order_release);
    running_.store(true,  std::memory_order_release);
    io_thread_ = std::thread([this] { runIoThread(); });

    if (logger_) {
        logger_->info("DemuxGateway[{}/{}]: started, in={}:{} → {} routes",
                      id_, name_, cfg_.input.address, cfg_.input.port,
                      cfg_.demux.routes.size());
    }
    return true;
}

void DemuxGateway::stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) return;
    stop_flag_.store(true, std::memory_order_release);
    if (io_thread_.joinable()) io_thread_.join();
    // Tear down FEC encoders before sockets_ — their sinks captured the fd /
    // sockaddrs by value but the underlying socket descriptors live in
    // sockets_->out_fds.
    for (auto& o : outputs_) {
        if (o.fec_encoder) {
            o.fec_encoder->flush();
            o.fec_encoder.reset();
        }
    }
    sockets_.reset();
    if (logger_) logger_->info("DemuxGateway[{}/{}]: stopped", id_, name_);
}

GatewayStats DemuxGateway::getStats() const {
    std::lock_guard<std::mutex> lk(stats_mu_);
    return stats_;
}

nlohmann::json DemuxGateway::statusJson() const {
    const auto s = getStats();
    nlohmann::json programs = nlohmann::json::array();
    auto snap = program_table_.current();
    if (snap) {
        for (const auto& p : snap->programs) {
            nlohmann::json streams = nlohmann::json::array();
            for (const auto& es : p.streams) {
                streams.push_back({
                    {"stream_type",   es.stream_type},
                    {"elementary_pid", es.elementary_pid},
                    {"language",     es.language},
                    {"codec_name",   es.codec_name},
                    {"has_subtitling", es.has_subtitling},
                    {"has_teletext",   es.has_teletext},
                });
            }
            programs.push_back({
                {"service_id",  p.service_id},
                {"pmt_pid",     p.pmt_pid},
                {"pcr_pid",     p.pcr_pid},
                {"discovered",  p.discovered},
                {"service_name", p.service_name},
                {"provider_name", p.provider_name},
                {"streams",     streams},
            });
        }
    }
    nlohmann::json fec_json = toJson(cfg_.fec);
    if (cfg_.fec.enabled) {
        std::uint64_t media = 0, col = 0, row = 0;
        for (const auto& o : outputs_) {
            if (!o.fec_encoder) continue;
            media += o.fec_encoder->mediaRtpEmitted();
            col   += o.fec_encoder->columnFecEmitted();
            row   += o.fec_encoder->rowFecEmitted();
        }
        fec_json["media_rtp_emitted"]  = media;
        fec_json["column_fec_emitted"] = col;
        fec_json["row_fec_emitted"]    = row;
    }
    return nlohmann::json{
        {"id",       id_},
        {"name",     name_},
        {"mode",     "demux"},
        {"running",  running_.load(std::memory_order_acquire)},
        {"input",    toJson(cfg_.input)},
        {"outputs",  toJson(cfg_).at("outputs")},
        {"demux",    toJson(cfg_.demux)},
        {"fec",      fec_json},
        {"programs", programs},
        {"pkt_in",   s.pkt_in},
        {"bytes_in", s.bytes_in},
        {"pkt_out",  s.pkt_out},
        {"bytes_out", s.bytes_out},
        {"drops",    s.drops},
    };
}

void DemuxGateway::setCfg(GatewayCfg cfg) {
    cfg_ = std::move(cfg);
    resolveOutputs();
}

// ─── Test seam ───────────────────────────────────────────────────────────────

void DemuxGateway::testFeedDatagram(std::span<const std::uint8_t> bytes) {
    processUdpDatagram(bytes);
}

void DemuxGateway::testEmitPsi() { emitPsi(); }

std::vector<std::vector<std::uint8_t>>
DemuxGateway::testDrainOutput(std::size_t output_idx) {
    if (output_idx >= outputs_.size()) return {};
    auto& o = outputs_[output_idx];
    // Flush whatever's in the buffer before draining.
    if (o.buffer && o.buffer->fill > 0) {
        std::vector<std::uint8_t> v(o.buffer->bytes.data(),
                                    o.buffer->bytes.data() + o.buffer->fill);
        o.testQueue.push_back(std::move(v));
        o.buffer->reset();
    }
    auto out = std::move(o.testQueue);
    o.testQueue.clear();
    return out;
}

// ─── Hot path ────────────────────────────────────────────────────────────────

void DemuxGateway::processUdpDatagram(std::span<const std::uint8_t> bytes) {
    {
        std::lock_guard<std::mutex> lk(stats_mu_);
        stats_.pkt_in   += 1;
        stats_.bytes_in += bytes.size();
    }
    // A datagram should contain an integer number of TS packets. If not,
    // we trim to the nearest 188-byte multiple — anything else is corrupt
    // input and the receiver will detect it via missing CC.
    const std::size_t whole = (bytes.size() / kTsPacketSize) * kTsPacketSize;
    for (std::size_t off = 0; off + kTsPacketSize <= whole; off += kTsPacketSize) {
        if (bytes[off] != ts::kTsSyncByte) continue;
        std::span<const std::uint8_t, kTsPacketSize> pkt(bytes.data() + off, kTsPacketSize);
        processTsPacket(pkt);
    }
    // After a datagram boundary, flush any half-filled output buffers so we
    // keep low latency at low rates.
    for (std::size_t i = 0; i < outputs_.size(); ++i) flushOutput(i);
}

void DemuxGateway::processTsPacket(std::span<const std::uint8_t, kTsPacketSize> bytes) {
    TsPacketView v(bytes);
    if (!v.isValidSync()) return;
    const std::uint16_t pid = v.pid();
    if (pid == kPidNull) return;

    // PSI tap: PAT/SDT/EIT and any known PMT PID feed the assembler.
    auto snap_for_pmt = program_table_.current();
    bool is_pmt_pid = false;
    if (snap_for_pmt) {
        for (const auto& p : snap_for_pmt->programs) {
            if (p.pmt_pid == pid) { is_pmt_pid = true; break; }
        }
    }
    if (pid == kPidPat || pid == kPidSdt || pid == kPidEit || is_pmt_pid) {
        psi_assembler_.feed(v, [this](std::uint16_t spid, std::span<const std::uint8_t> sec) {
            onPsiSection(spid, sec);
        });
        // PSI tables are regenerated per output, so we never forward the
        // input PSI packets directly. A receiver consuming our SPTS sees
        // only our re-emitted PAT/PMT/SDT.
        return;
    }

    // ES forwarding via pid_table_.
    const auto& e = pid_table_[pid];
    if (e.out_idx == 0xFF) return;          // unrouted PID — drop

    // CC rewrite: copy the packet into a mutable scratch buffer with new
    // CC + new PID.
    std::array<std::uint8_t, kTsPacketSize> rewritten;
    std::memcpy(rewritten.data(), bytes.data(), kTsPacketSize);
    ts::TsPacketMut m(std::span<std::uint8_t, kTsPacketSize>(rewritten.data(), kTsPacketSize));
    if (e.out_pid != pid) m.setPid(e.out_pid);
    m.setContinuityCounter(e.out_cc);

    // CC advances per output PID — bump the entry. Note that two ES PIDs
    // might collide on the same output_pid only in pathological remap; we
    // don't try to detect it here, the PsiBuilder's PMT will reflect what
    // the table says.
    pid_table_[pid].out_cc = static_cast<std::uint8_t>((e.out_cc + 1) & 0x0F);

    sendOrBuffer(e.out_idx,
                 std::span<const std::uint8_t, kTsPacketSize>(rewritten.data(), kTsPacketSize));
}

void DemuxGateway::onPsiSection(std::uint16_t pid, std::span<const std::uint8_t> sec) {
    if (pid == kPidPat) {
        if (auto pat = ts::parsePat(sec)) {
            program_table_.ingestPat(*pat);
            rebuildPidTable();
        }
        return;
    }
    if (pid == kPidSdt) {
        if (auto sdt = ts::parseSdt(sec)) program_table_.ingestSdt(*sdt);
        return;
    }
    if (pid == kPidEit) {
        // EIT pass-through (fix-A4). Parse, then for each output bound to
        // the section's service_id with preserve_eit=true, rebuild via
        // PsiBuilder (re-stamping TSID/ONID to our snapshot) and forward at
        // PID 0x12 with the per-output CC. Cadence is whatever the input
        // sends — typically 2 s for present/following, slower for schedule.
        auto parsed = ts::parseEit(sec);
        if (!parsed) return;
        auto snap = program_table_.current();
        const std::uint16_t out_tsid =
            (snap && snap->transport_stream_id != 0) ? snap->transport_stream_id : 1;
        const std::uint16_t out_onid =
            (snap && snap->original_network_id != 0) ? snap->original_network_id
                                                    : parsed->original_network_id;

        ts::EitBuildInput bi;
        bi.table_id              = parsed->table_id;
        bi.service_id            = parsed->service_id;
        bi.transport_stream_id   = out_tsid;
        bi.original_network_id   = out_onid;
        bi.version_number        = parsed->version_number;
        bi.current_next_indicator = true;
        bi.events                = std::move(parsed->events);

        for (std::size_t i = 0; i < outputs_.size(); ++i) {
            auto& o = outputs_[i];
            if (!o.rule.preserve_eit) continue;
            if (o.service_id != bi.service_id) continue;
            auto eit_sec = ts::buildEitSection(bi);
            auto eit_pkts = ts::packetizeSection(kPidEit, eit_sec, o.eit_cc);
            for (const auto& pk : eit_pkts) {
                sendOrBuffer(i, std::span<const std::uint8_t, kTsPacketSize>(
                                  pk.data(), kTsPacketSize));
            }
        }
        return;
    }
    // Otherwise this must be one of the known PMT PIDs.
    if (auto pmt = ts::parsePmt(sec)) {
        program_table_.ingestPmt(*pmt);
        rebuildPidTable();
    }
}

void DemuxGateway::rebuildPidTable() {
    auto snap = program_table_.current();
    if (!snap) return;
    if (snap->programs.empty()) return;

    // Snapshot identity is preserved across no-op republishes (e.g. PAT
    // rebroadcast with same payload). On any real change ProgramTable
    // publishes a fresh shared_ptr — compare pointers, not contents.
    if (last_routed_snapshot_ == snap) return;
    last_routed_snapshot_ = snap;

    // Reset table.
    for (auto& e : pid_table_) e = PidEntry{};

    // Walk routes, look up program in snapshot, install entries.
    for (std::size_t out_idx = 0; out_idx < outputs_.size(); ++out_idx) {
        auto& o = outputs_[out_idx];
        if (o.service_id == 0) continue;            // output not bound to a route
        const auto* p = snap->find(o.service_id);
        if (!p || !p->discovered) continue;

        // Build a remap helper — input PID → output PID, defaulting to identity.
        auto remapped = [&](std::uint16_t pid) -> std::uint16_t {
            for (auto [from, to] : o.rule.pid_remap)
                if (from == pid) return to;
            return pid;
        };

        for (const auto& es : p->streams) {
            // Subtitle/teletext PID gating: skip if operator disabled them.
            if (es.has_subtitling && !o.rule.preserve_subtitles) continue;
            if (es.has_teletext   && !o.rule.preserve_teletext)  continue;
            const std::uint16_t opid = remapped(es.elementary_pid);
            auto& e = pid_table_[es.elementary_pid];
            e.out_idx = static_cast<std::uint8_t>(out_idx);
            e.out_pid = opid;
            e.out_cc  = 0;
        }
        // PCR PID — forward separately if it's not also one of the ES PIDs.
        if (p->pcr_pid != 0x1FFF) {
            bool already = false;
            for (const auto& es : p->streams)
                if (es.elementary_pid == p->pcr_pid) { already = true; break; }
            if (!already) {
                const std::uint16_t opid = remapped(p->pcr_pid);
                auto& e = pid_table_[p->pcr_pid];
                e.out_idx = static_cast<std::uint8_t>(out_idx);
                e.out_pid = opid;
                e.out_cc  = 0;
            }
        }
    }
}

void DemuxGateway::sendOrBuffer(std::size_t out_idx,
                                std::span<const std::uint8_t, kTsPacketSize> pkt) {
    auto& o = outputs_[out_idx];
    // Live + FEC: every TS packet passes through the encoder, which batches
    // into media RTP and emits column / row FEC. The legacy OutputBuffer is
    // bypassed in this path. Test path (running_==false) keeps the legacy
    // raw-TS UDP buffer behaviour so existing test seams stay deterministic.
    if (o.fec_encoder && running_.load(std::memory_order_acquire)) {
        o.fec_encoder->feedTsPacket(
            std::span<const std::uint8_t>(pkt.data(), pkt.size()));
        std::lock_guard<std::mutex> lk(stats_mu_);
        stats_.pkt_out += 1;
        return;
    }
    if (!o.buffer) o.buffer = std::make_unique<OutputBuffer>();
    o.buffer->append(pkt);
    if (o.buffer->full()) flushOutput(out_idx);
}

void DemuxGateway::flushOutput(std::size_t out_idx) {
    auto& o = outputs_[out_idx];
    if (!o.buffer || o.buffer->fill == 0) return;
    if (running_.load(std::memory_order_acquire) && sockets_) {
        const ssize_t s = ::sendto(
            sockets_->out_fds[out_idx].get(),
            o.buffer->bytes.data(), o.buffer->fill,
            MSG_DONTWAIT,
            reinterpret_cast<sockaddr*>(&sockets_->out_addrs[out_idx]),
            sizeof(sockaddr_in));
        std::lock_guard<std::mutex> lk(stats_mu_);
        if (s == static_cast<ssize_t>(o.buffer->fill)) {
            stats_.pkt_out   += o.buffer->fill / kTsPacketSize;
            stats_.bytes_out += o.buffer->fill;
        } else {
            stats_.drops += o.buffer->fill / kTsPacketSize;
        }
    } else {
        // Test path — retain bytes for inspection.
        std::vector<std::uint8_t> v(o.buffer->bytes.data(),
                                    o.buffer->bytes.data() + o.buffer->fill);
        o.testQueue.push_back(std::move(v));
    }
    o.buffer->reset();
}

// ─── PSI emission ────────────────────────────────────────────────────────────

void DemuxGateway::emitPsi() {
    auto snap = program_table_.current();
    if (!snap) return;

    for (std::size_t i = 0; i < outputs_.size(); ++i) {
        auto& o = outputs_[i];
        if (o.service_id == 0) continue;
        const auto* p = snap->find(o.service_id);
        if (!p || !p->discovered) continue;

        // --- PAT (single program)
        ts::PatBuildInput patIn;
        patIn.transport_stream_id = snap->transport_stream_id != 0 ? snap->transport_stream_id : 1;
        patIn.version_number      = o.pat_version;
        patIn.programs.push_back({o.service_id, o.output_pmt_pid});
        auto pat_sec = ts::buildPatSection(patIn);
        auto pat_pkts = ts::packetizeSection(kPidPat, pat_sec, o.pat_cc);
        for (const auto& pk : pat_pkts) {
            sendOrBuffer(i, std::span<const std::uint8_t, kTsPacketSize>(pk.data(), kTsPacketSize));
        }

        // --- PMT
        ts::PmtBuildInput pmtIn;
        pmtIn.program_number = o.service_id;
        pmtIn.version_number = o.pmt_version;
        pmtIn.pcr_pid        = p->pcr_pid;
        // Apply PCR PID remap if rule has one.
        for (auto [from, to] : o.rule.pid_remap)
            if (from == p->pcr_pid) { pmtIn.pcr_pid = to; break; }

        for (const auto& es : p->streams) {
            if (es.has_subtitling && !o.rule.preserve_subtitles) continue;
            if (es.has_teletext   && !o.rule.preserve_teletext)  continue;
            ts::PmtStream s;
            s.stream_type    = es.stream_type;
            std::uint16_t opid = es.elementary_pid;
            for (auto [from, to] : o.rule.pid_remap)
                if (from == es.elementary_pid) { opid = to; break; }
            s.elementary_pid = opid;
            // Build language descriptor 0x0A back from decoded language.
            if (!es.language.empty() && es.language.size() <= 3) {
                ts::RawDescriptor d;
                d.tag = 0x0A;
                d.body.resize(4);
                d.body[0] = static_cast<std::uint8_t>(es.language[0]);
                d.body[1] = static_cast<std::uint8_t>(es.language.size() > 1 ? es.language[1] : ' ');
                d.body[2] = static_cast<std::uint8_t>(es.language.size() > 2 ? es.language[2] : ' ');
                d.body[3] = 0x00;
                s.es_descriptors.push_back(std::move(d));
            }
            pmtIn.streams.push_back(std::move(s));
        }
        auto pmt_sec = ts::buildPmtSection(pmtIn);
        auto pmt_pkts = ts::packetizeSection(o.output_pmt_pid, pmt_sec, o.pmt_cc);
        for (const auto& pk : pmt_pkts) {
            sendOrBuffer(i, std::span<const std::uint8_t, kTsPacketSize>(pk.data(), kTsPacketSize));
        }

        // --- SDT (subset for our service)
        if (cfg_.demux.emit_sdt) {
            ts::SdtBuildInput sdtIn;
            sdtIn.transport_stream_id  = patIn.transport_stream_id;
            sdtIn.original_network_id  = snap->original_network_id;
            sdtIn.version_number       = o.sdt_version;
            ts::SdtService svc;
            svc.service_id                 = o.service_id;
            svc.eit_present_following_flag = o.rule.preserve_eit && p->eit_present;
            svc.eit_schedule_flag          = false;
            svc.running_status             = p->running_status != 0 ? p->running_status : 4;
            svc.descriptors.push_back(ts::makeServiceDescriptor(
                serviceTypeFromStreams(*p),
                p->provider_name,
                !p->service_name.empty() ? p->service_name : ("svc" + std::to_string(o.service_id))));
            sdtIn.services.push_back(std::move(svc));
            auto sdt_sec = ts::buildSdtSection(sdtIn);
            auto sdt_pkts = ts::packetizeSection(kPidSdt, sdt_sec, o.sdt_cc);
            for (const auto& pk : sdt_pkts) {
                sendOrBuffer(i, std::span<const std::uint8_t, kTsPacketSize>(pk.data(), kTsPacketSize));
            }
        }

        flushOutput(i);
    }
}

void DemuxGateway::emitPsiIfDue(std::chrono::steady_clock::time_point now) {
    using namespace std::chrono;
    if (last_pat_emit_ == steady_clock::time_point{}) {
        last_pat_emit_ = last_pmt_emit_ = last_sdt_emit_ = now - hours(1);
    }
    const auto pat_due = now - last_pat_emit_ >= milliseconds(cfg_.demux.pat_period_ms);
    const auto pmt_due = now - last_pmt_emit_ >= milliseconds(cfg_.demux.pmt_period_ms);
    const auto sdt_due = now - last_sdt_emit_ >= milliseconds(cfg_.demux.sdt_period_ms);
    if (pat_due || pmt_due || sdt_due) {
        emitPsi();
        if (pat_due) last_pat_emit_ = now;
        if (pmt_due) last_pmt_emit_ = now;
        if (sdt_due) last_sdt_emit_ = now;
    }
}

void DemuxGateway::runIoThread() {
    if (numa_node_ >= 0) numa::bindCurrentThreadToNode(numa_node_);

    auto& lg = logger_ ? *logger_ : *spdlog::default_logger();

    std::array<std::uint8_t, kRecvBufBytes> buf{};
    while (!stop_flag_.load(std::memory_order_acquire)) {
        const ssize_t n = ::recv(sockets_->in_fd.get(), buf.data(), buf.size(), 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                // heartbeat — fall through to PSI emit check
            } else {
                lg.error("DemuxGateway[{}]: recv: {}", id_, std::strerror(errno));
                break;
            }
        } else if (n > 0) {
            processUdpDatagram(std::span<const std::uint8_t>(buf.data(), static_cast<std::size_t>(n)));
        }
        emitPsiIfDue(std::chrono::steady_clock::now());
    }
    // Final flush.
    for (std::size_t i = 0; i < outputs_.size(); ++i) flushOutput(i);
}

} // namespace liveqx::gateway
