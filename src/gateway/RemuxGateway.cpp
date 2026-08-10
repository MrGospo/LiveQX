#include "gateway/RemuxGateway.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <utility>

#include <spdlog/spdlog.h>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "gateway/fec/FecEncoder.h"
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

constexpr std::size_t kPacketsPerDatagram = 7;
constexpr std::size_t kPidSpace           = 8192;
constexpr std::size_t kRecvBufBytes       = 8192;

// Default base PMT PID range for synthesized output PMTs. We allocate
// 0x0100 + input_idx; cfg.remux.pid_remap targeting an input's existing
// PMT PID can override.
constexpr std::uint16_t kBasePmtPid = 0x0100;

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

// Add `ns` nanoseconds to a CLOCK_MONOTONIC timespec.
inline timespec addNs(timespec t, std::uint64_t ns) noexcept {
    t.tv_sec  += static_cast<time_t>(ns / 1'000'000'000ull);
    const std::uint64_t rem = ns % 1'000'000'000ull;
    t.tv_nsec += static_cast<long>(rem);
    if (t.tv_nsec >= 1'000'000'000) {
        t.tv_nsec -= 1'000'000'000;
        t.tv_sec  += 1;
    }
    return t;
}

bool nicNameToIPv4(const std::string& name, in_addr& out) {
    ifaddrs* ifa = nullptr;
    if (::getifaddrs(&ifa) != 0) return false;
    bool found = false;
    for (ifaddrs* p = ifa; p; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
        if (name != p->ifa_name) continue;
        out = reinterpret_cast<sockaddr_in*>(p->ifa_addr)->sin_addr;
        found = true; break;
    }
    ::freeifaddrs(ifa);
    return found;
}

int openInputSocket(const InputCfg& cfg, int gw_id, spdlog::logger& lg) {
    in_addr group{};
    if (::inet_pton(AF_INET, cfg.address.c_str(), &group) != 1) {
        lg.error("RemuxGateway[{}]: invalid input address '{}'", gw_id, cfg.address);
        return -1;
    }
    FdGuard fd(::socket(AF_INET, SOCK_DGRAM, 0));
    if (!fd.valid()) {
        lg.error("RemuxGateway[{}]: input socket(): {}", gw_id, std::strerror(errno));
        return -1;
    }
    const int reuse = 1;
    ::setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    const int rcvbuf = cfg.recv_buffer_kb * 1024;
    ::setsockopt(fd.get(), SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    sockaddr_in bind_addr{};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port   = htons(static_cast<std::uint16_t>(cfg.port));
    bind_addr.sin_addr.s_addr = isMulticast(group) ? htonl(INADDR_ANY) : group.s_addr;
    if (::bind(fd.get(), reinterpret_cast<sockaddr*>(&bind_addr), sizeof(bind_addr)) < 0) {
        lg.error("RemuxGateway[{}]: bind({}:{}): {}",
                 gw_id, cfg.address, cfg.port, std::strerror(errno));
        return -1;
    }
    if (!cfg.interface_name.empty()) {
        ::setsockopt(fd.get(), SOL_SOCKET, SO_BINDTODEVICE,
                     cfg.interface_name.c_str(),
                     static_cast<socklen_t>(cfg.interface_name.size()));
    }
    if (isMulticast(group)) {
        ip_mreq mreq{};
        mreq.imr_multiaddr        = group;
        mreq.imr_interface.s_addr = htonl(INADDR_ANY);
        if (!cfg.interface_addr.empty()) {
            ::inet_pton(AF_INET, cfg.interface_addr.c_str(), &mreq.imr_interface);
        } else if (!cfg.interface_name.empty()) {
            in_addr nic{}; if (nicNameToIPv4(cfg.interface_name, nic)) mreq.imr_interface = nic;
        }
        if (::setsockopt(fd.get(), IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
            lg.error("RemuxGateway[{}]: IP_ADD_MEMBERSHIP({}): {}",
                     gw_id, cfg.address, std::strerror(errno));
            return -1;
        }
    }
    return fd.release();
}

int openOutputSocket(const OutputCfg& cfg, sockaddr_in& dst_out,
                     int gw_id, spdlog::logger& lg) {
    in_addr dst{};
    if (::inet_pton(AF_INET, cfg.address.c_str(), &dst) != 1) {
        lg.error("RemuxGateway[{}]: invalid output address '{}'", gw_id, cfg.address);
        return -1;
    }
    FdGuard fd(::socket(AF_INET, SOCK_DGRAM, 0));
    if (!fd.valid()) {
        lg.error("RemuxGateway[{}]: output socket(): {}", gw_id, std::strerror(errno));
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
            have_if = nicNameToIPv4(cfg.interface_name, ifaddr);
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

} // namespace

// ─── Output buffer ───────────────────────────────────────────────────────────

struct RemuxGateway::OutputBuffer {
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

struct RemuxGateway::Sockets {
    std::vector<FdGuard> in_fds;
    FdGuard              out_fd;
    sockaddr_in          out_addr{};
    sockaddr_in          out_addr_col_fec{};
    sockaddr_in          out_addr_row_fec{};
};

// ─── Lifecycle ───────────────────────────────────────────────────────────────

RemuxGateway::RemuxGateway(int id, std::string name, GatewayCfg cfg)
    : id_(id),
      name_(std::move(name)),
      cfg_(std::move(cfg)),
      pcr_restamper_(cfg_.remux.target_bitrate_bps) {
    resolveTopology();
}

RemuxGateway::~RemuxGateway() { RemuxGateway::stop(); }

void RemuxGateway::resolveTopology() {
    if (cfg_.outputs.size() != 1) {
        throw std::invalid_argument(
            "RemuxGateway: exactly one MPTS output required");
    }
    output_cfg_ = cfg_.outputs.front();
    if (!output_buffer_) output_buffer_ = std::make_unique<OutputBuffer>();

    const std::size_t n_inputs = 1 + cfg_.extra_inputs.size();
    if (n_inputs < 2)
        throw std::invalid_argument(
            "RemuxGateway: requires ≥2 inputs (cfg.input + cfg.extra_inputs)");

    inputs_.clear();
    inputs_.reserve(n_inputs);
    for (std::size_t i = 0; i < n_inputs; ++i) {
        InputState s;
        s.cfg = (i == 0) ? cfg_.input : cfg_.extra_inputs[i - 1];
        s.pid_table.assign(kPidSpace, PidEntry{});
        s.output_pmt_pid = static_cast<std::uint16_t>(kBasePmtPid + i);
        inputs_.push_back(std::move(s));
    }

    // PMT PID overrides via pid_remap: any entry whose src_pid happens to
    // match the input's discovered PMT PID can pin a deterministic PMT PID.
    // We can't know the discovered PMT PID until PAT arrives, so this is
    // applied lazily in rebuildOutputTable().
}

bool RemuxGateway::start() {
    if (running_.load(std::memory_order_acquire)) return false;
    if (inputs_.empty() || cfg_.outputs.size() != 1) {
        if (logger_) logger_->error("RemuxGateway[{}]: bad topology", id_);
        return false;
    }
    auto& lg = logger_ ? *logger_ : *spdlog::default_logger();

    auto set = std::make_unique<Sockets>();
    set->in_fds.reserve(inputs_.size());
    for (std::size_t i = 0; i < inputs_.size(); ++i) {
        const int fd = openInputSocket(inputs_[i].cfg, id_, lg);
        if (fd < 0) return false;
        set->in_fds.emplace_back(fd);
    }
    {
        sockaddr_in dst{};
        const int fd = openOutputSocket(output_cfg_, dst, id_, lg);
        if (fd < 0) return false;
        set->out_fd.reset(fd);
        set->out_addr = dst;
        if (cfg_.fec.enabled) {
            set->out_addr_col_fec = dst;
            set->out_addr_col_fec.sin_port = htons(static_cast<std::uint16_t>(
                output_cfg_.port + cfg_.fec.column_port_offset));
            set->out_addr_row_fec = dst;
            set->out_addr_row_fec.sin_port = htons(static_cast<std::uint16_t>(
                output_cfg_.port + cfg_.fec.row_port_offset));
        }
    }
    sockets_ = std::move(set);

    if (cfg_.fec.enabled) {
        const int        fd    = sockets_->out_fd.get();
        const sockaddr_in media = sockets_->out_addr;
        const sockaddr_in col   = sockets_->out_addr_col_fec;
        const sockaddr_in row   = sockets_->out_addr_row_fec;
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
        const std::uint32_t base_ssrc =
            cfg_.fec.ssrc != 0
              ? cfg_.fec.ssrc
              : static_cast<std::uint32_t>(
                    std::chrono::steady_clock::now().time_since_epoch().count());
        fec_encoder_ = std::make_unique<fec::FecEncoder>(
            cfg_.fec,
            base_ssrc,
            base_ssrc + 1u,
            base_ssrc + 2u,
            [media, sendOn](std::span<const std::uint8_t> p) { sendOn(media, p); },
            [col,   sendOn](std::span<const std::uint8_t> p) { sendOn(col,   p); },
            cfg_.fec.mode == FecCfg::Mode::TwoD
                ? fec::FecEncoder::PacketSink(
                      [row, sendOn](std::span<const std::uint8_t> p) {
                          sendOn(row, p);
                      })
                : fec::FecEncoder::PacketSink{});
    }

    // Reset PSI state for clean rediscovery.
    for (auto& s : inputs_) {
        s.psi_assembler.clearAll();
        s.program_table.clear();
        s.last_seen_snapshot.reset();
        s.pid_table.assign(kPidSpace, PidEntry{});
        s.sid_remap.clear();
    }
    output_program_table_.clear();
    bytes_in_window_ = 0;
    last_top_up_     = std::chrono::steady_clock::time_point{};

    // Restart PCR clock — bytes_emitted=0, drop all PID anchors. Bitrate may
    // have changed via setCfg() while stopped; sync from current cfg.
    pcr_restamper_.reset();
    pcr_restamper_.setBitrate(cfg_.remux.target_bitrate_bps);
    pcr_pids_registered_.clear();

    // Pacing state — captured on first send so input ramp-up doesn't poison
    // the output schedule.
    bytes_sent_total_   = 0;
    pacing_anchor_set_  = false;
    pacing_anchor_      = timespec{};

    stop_flag_.store(false, std::memory_order_release);
    running_.store(true,  std::memory_order_release);
    io_thread_ = std::thread([this] { runIoThread(); });

    if (logger_)
        logger_->info("RemuxGateway[{}/{}]: started, {} inputs → {}:{}",
                      id_, name_, inputs_.size(),
                      output_cfg_.address, output_cfg_.port);
    return true;
}

void RemuxGateway::stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) return;
    stop_flag_.store(true, std::memory_order_release);
    if (io_thread_.joinable()) io_thread_.join();
    if (fec_encoder_) {
        fec_encoder_->flush();
        fec_encoder_.reset();
    }
    sockets_.reset();
    if (logger_) logger_->info("RemuxGateway[{}/{}]: stopped", id_, name_);
}

GatewayStats RemuxGateway::getStats() const {
    std::lock_guard<std::mutex> lk(stats_mu_);
    return stats_;
}

nlohmann::json RemuxGateway::statusJson() const {
    const auto s = getStats();
    auto inputs_json = nlohmann::json::array();
    for (std::size_t i = 0; i < inputs_.size(); ++i) {
        auto snap = inputs_[i].program_table.current();
        nlohmann::json programs = nlohmann::json::array();
        if (snap) {
            for (const auto& p : snap->programs) {
                programs.push_back({
                    {"service_id",   p.service_id},
                    {"pmt_pid",      p.pmt_pid},
                    {"discovered",   p.discovered},
                    {"service_name", p.service_name},
                });
            }
        }
        inputs_json.push_back({
            {"index",     i},
            {"input",     toJson(inputs_[i].cfg)},
            {"programs",  programs},
        });
    }

    nlohmann::json out_programs = nlohmann::json::array();
    auto out_snap = output_program_table_.current();
    if (out_snap) {
        for (const auto& p : out_snap->programs) {
            nlohmann::json streams = nlohmann::json::array();
            for (const auto& es : p.streams)
                streams.push_back({
                    {"stream_type",     es.stream_type},
                    {"elementary_pid",  es.elementary_pid},
                    {"language",        es.language},
                });
            out_programs.push_back({
                {"service_id",  p.service_id},
                {"pmt_pid",     p.pmt_pid},
                {"streams",     streams},
            });
        }
    }

    nlohmann::json fec_json = toJson(cfg_.fec);
    if (cfg_.fec.enabled && fec_encoder_) {
        fec_json["media_rtp_emitted"]  = fec_encoder_->mediaRtpEmitted();
        fec_json["column_fec_emitted"] = fec_encoder_->columnFecEmitted();
        fec_json["row_fec_emitted"]    = fec_encoder_->rowFecEmitted();
    }
    return nlohmann::json{
        {"id",        id_},
        {"name",      name_},
        {"mode",      "remux"},
        {"running",   running_.load(std::memory_order_acquire)},
        {"inputs",    inputs_json},
        {"output",    toJson(output_cfg_)},
        {"remux",     toJson(cfg_.remux)},
        {"fec",       fec_json},
        {"programs",  out_programs},
        {"pkt_in",    s.pkt_in},
        {"bytes_in",  s.bytes_in},
        {"pkt_out",   s.pkt_out},
        {"bytes_out", s.bytes_out},
        {"drops",     s.drops},
    };
}

void RemuxGateway::setCfg(GatewayCfg cfg) {
    cfg_ = std::move(cfg);
    resolveTopology();
    // Hot-update bitrate. Anchors stay valid — receivers just see slightly
    // different spacing on subsequent PCRs, which is the intent of the change.
    pcr_restamper_.setBitrate(cfg_.remux.target_bitrate_bps);
}

// ─── Test seam ───────────────────────────────────────────────────────────────

void RemuxGateway::testFeedDatagram(std::size_t input_idx,
                                    std::span<const std::uint8_t> bytes) {
    if (input_idx >= inputs_.size()) return;
    processInputDatagram(input_idx, bytes);
}

void RemuxGateway::testEmitPsi() { emitPsi(); }

void RemuxGateway::testTopUpStuffing(
        std::chrono::steady_clock::time_point now) {
    topUpStuffing(now);
    flushOutput();
}

std::vector<std::vector<std::uint8_t>> RemuxGateway::testDrainOutput() {
    if (output_buffer_ && output_buffer_->fill > 0) {
        std::vector<std::uint8_t> v(
            output_buffer_->bytes.data(),
            output_buffer_->bytes.data() + output_buffer_->fill);
        output_test_queue_.push_back(std::move(v));
        output_buffer_->reset();
    }
    auto out = std::move(output_test_queue_);
    output_test_queue_.clear();
    return out;
}

std::shared_ptr<const ts::ProgramTableSnapshot>
RemuxGateway::inputPrograms(std::size_t input_idx) const {
    if (input_idx >= inputs_.size()) return nullptr;
    return inputs_[input_idx].program_table.current();
}

std::shared_ptr<const ts::ProgramTableSnapshot>
RemuxGateway::outputPrograms() const {
    return output_program_table_.current();
}

// ─── Hot path ────────────────────────────────────────────────────────────────

void RemuxGateway::processInputDatagram(std::size_t input_idx,
                                        std::span<const std::uint8_t> bytes) {
    {
        std::lock_guard<std::mutex> lk(stats_mu_);
        stats_.pkt_in   += 1;
        stats_.bytes_in += bytes.size();
    }
    const std::size_t whole = (bytes.size() / kTsPacketSize) * kTsPacketSize;
    for (std::size_t off = 0; off + kTsPacketSize <= whole; off += kTsPacketSize) {
        if (bytes[off] != ts::kTsSyncByte) continue;
        std::span<const std::uint8_t, kTsPacketSize> pkt(
            bytes.data() + off, kTsPacketSize);
        processInputTsPacket(input_idx, pkt);
    }
    flushOutput();
}

void RemuxGateway::processInputTsPacket(
        std::size_t input_idx,
        std::span<const std::uint8_t, kTsPacketSize> bytes) {
    auto& in = inputs_[input_idx];
    TsPacketView v(bytes);
    if (!v.isValidSync()) return;
    const std::uint16_t pid = v.pid();
    if (pid == kPidNull) return;

    // PSI tap: PAT/SDT/EIT and any known PMT PID feed this input's assembler.
    auto snap_for_pmt = in.program_table.current();
    bool is_pmt_pid = false;
    if (snap_for_pmt) {
        for (const auto& p : snap_for_pmt->programs) {
            if (p.pmt_pid == pid) { is_pmt_pid = true; break; }
        }
    }
    if (pid == kPidPat || pid == kPidSdt || pid == kPidEit || is_pmt_pid) {
        in.psi_assembler.feed(v, [this, input_idx](
                std::uint16_t spid, std::span<const std::uint8_t> sec) {
            onPsiSection(input_idx, spid, sec);
        });
        // PSI is regenerated for the combined MPTS — never forward source PSI.
        return;
    }

    // ES forwarding via this input's pid_table.
    const auto& e = in.pid_table[pid];
    if (e.out_pid == 0xFFFF) return;     // unrouted PID — drop

    std::array<std::uint8_t, kTsPacketSize> rewritten;
    std::memcpy(rewritten.data(), bytes.data(), kTsPacketSize);
    ts::TsPacketMut m(std::span<std::uint8_t, kTsPacketSize>(
        rewritten.data(), kTsPacketSize));
    if (e.out_pid != pid) m.setPid(e.out_pid);
    m.setContinuityCounter(e.out_cc);
    in.pid_table[pid].out_cc = static_cast<std::uint8_t>((e.out_cc + 1) & 0x0F);

    sendOrBuffer(std::span<std::uint8_t, kTsPacketSize>(
        rewritten.data(), kTsPacketSize));
}

void RemuxGateway::onPsiSection(std::size_t input_idx, std::uint16_t pid,
                                std::span<const std::uint8_t> sec) {
    auto& in = inputs_[input_idx];
    if (pid == kPidPat) {
        if (auto pat = ts::parsePat(sec)) {
            in.program_table.ingestPat(*pat);
            rebuildOutputTable();
        }
        return;
    }
    if (pid == kPidSdt) {
        if (auto sdt = ts::parseSdt(sec)) in.program_table.ingestSdt(*sdt);
        return;
    }
    if (pid == kPidEit) {
        // EIT forward (fix-A4). Apply this input's sid_remap so the rewritten
        // section advertises the service_id used in the combined MPTS, then
        // re-stamp TSID/ONID to the output's identity. Sections whose source
        // service_id is not in the input's PAT (and therefore not in
        // sid_remap) are dropped — they reference a service the output cannot
        // describe via PAT/PMT.
        auto parsed = ts::parseEit(sec);
        if (!parsed) return;
        const auto sid_it = in.sid_remap.find(parsed->service_id);
        if (sid_it == in.sid_remap.end()) return;

        ts::EitBuildInput bi;
        bi.table_id              = parsed->table_id;
        bi.service_id            = sid_it->second;
        bi.transport_stream_id   = cfg_.remux.transport_stream_id;
        bi.original_network_id   = cfg_.remux.original_network_id;
        bi.version_number        = parsed->version_number;
        bi.current_next_indicator = true;
        bi.events                = std::move(parsed->events);

        auto eit_sec  = ts::buildEitSection(bi);
        auto eit_pkts = ts::packetizeSection(kPidEit, eit_sec, eit_cc_);
        for (auto& pk : eit_pkts) {
            sendOrBuffer(std::span<std::uint8_t, kTsPacketSize>(
                pk.data(), kTsPacketSize));
        }
        return;
    }
    if (auto pmt = ts::parsePmt(sec)) {
        in.program_table.ingestPmt(*pmt);
        rebuildOutputTable();
    }
}

// ─── PID conflict resolution ─────────────────────────────────────────────────
//
// Build the synthesized output ProgramTable + per-input pid_table from the
// per-input snapshots. Pinned cfg.remux.pid_remap entries take precedence;
// unpinned PIDs default to identity, with collisions resolved by walking the
// PID space upward starting at 0x0200 (well above the kBasePmtPid range).
//
// service_id collisions follow cfg.remux.service_id_policy:
//   - AutoRenumber: subsequent collisions advance to the next free service_id
//   - Reject:       parseGatewayCfg already throws, so we trust cfg here

void RemuxGateway::rebuildOutputTable() {
    // Collect per-input snapshots; bail if none have anything yet.
    bool any_changed = false;
    bool any_present = false;
    for (auto& s : inputs_) {
        auto cur = s.program_table.current();
        if (cur) any_present = true;
        if (cur != s.last_seen_snapshot) {
            any_changed = true;
            s.last_seen_snapshot = cur;
        }
    }
    if (!any_present) return;
    if (!any_changed) return;

    // Build pin table from cfg.remux.pid_remap: (input_idx, src_pid) → dst_pid.
    auto pinKey = [](std::uint16_t input_idx, std::uint16_t pid) {
        return (std::uint32_t(input_idx) << 16) | pid;
    };
    std::unordered_map<std::uint32_t, std::uint16_t> pins;
    pins.reserve(cfg_.remux.pid_remap.size());
    for (const auto& e : cfg_.remux.pid_remap)
        pins.emplace(pinKey(e.input_idx, e.src_pid), e.dst_pid);

    // Track which output PIDs are taken so unpinned identity collisions can
    // be auto-resolved.
    std::vector<bool> pid_used(kPidSpace, false);
    pid_used[kPidPat] = true;
    pid_used[kPidSdt] = true;
    pid_used[kPidEit] = true;
    pid_used[kPidNull] = true;
    for (const auto& kv : pins)
        if (kv.second < kPidSpace) pid_used[kv.second] = true;

    // Reserve PMT PIDs for each input. If a pin refers to the input's PMT PID
    // we honour it; otherwise allocate kBasePmtPid + idx unless that slot is
    // taken (then walk upward).
    auto allocFreePid = [&](std::uint16_t start) -> std::uint16_t {
        for (std::uint32_t p = start; p < kPidSpace - 1; ++p)
            if (!pid_used[p]) { pid_used[p] = true; return static_cast<std::uint16_t>(p); }
        return 0xFFFF;
    };

    // Pass 1: assign PMT PIDs.
    for (std::size_t i = 0; i < inputs_.size(); ++i) {
        auto& in = inputs_[i];
        auto snap = in.program_table.current();
        if (!snap || snap->programs.empty()) {
            in.output_pmt_pid = 0;          // not yet ready
            continue;
        }
        // The input has a PMT PID per program; for now we only support the
        // first program per input (SPTS expectation). Multi-program SPTS is
        // pathological but tolerated by taking the first.
        const std::uint16_t input_pmt = snap->programs.front().pmt_pid;
        std::uint16_t out_pmt = static_cast<std::uint16_t>(kBasePmtPid + i);
        // Pin override: if the operator explicitly remapped this input's
        // PMT PID, honour it.
        const auto pin_it = pins.find(pinKey(static_cast<std::uint16_t>(i), input_pmt));
        if (pin_it != pins.end()) out_pmt = pin_it->second;
        else if (pid_used[out_pmt]) out_pmt = allocFreePid(0x0200);
        in.output_pmt_pid = out_pmt;
        if (out_pmt < kPidSpace) pid_used[out_pmt] = true;
    }

    // Pass 2: build per-input ES PID maps + service_id renumber.
    std::unordered_map<std::uint16_t, std::size_t> sid_owner; // sid → input_idx

    for (std::size_t i = 0; i < inputs_.size(); ++i) {
        auto& in = inputs_[i];
        // Preserve CC counters across rebuilds: keyed by output PID. Any PID
        // whose out_pid survives the rebuild keeps its CC, so receivers see
        // a monotonic sequence regardless of PSI churn.
        std::unordered_map<std::uint16_t, std::uint8_t> prev_cc;
        prev_cc.reserve(64);
        for (const auto& e : in.pid_table)
            if (e.out_pid != 0xFFFF) prev_cc.emplace(e.out_pid, e.out_cc);
        in.pid_table.assign(kPidSpace, PidEntry{});

        auto snap = in.program_table.current();
        if (!snap || snap->programs.empty()) continue;

        // service_id renumber.
        in.sid_remap.clear();
        for (const auto& p : snap->programs) {
            std::uint16_t sid = p.service_id;
            if (sid_owner.count(sid)) {
                if (cfg_.remux.service_id_policy
                        == RemuxCfg::ServiceIdPolicy::Reject) {
                    if (logger_)
                        logger_->error(
                            "RemuxGateway[{}]: service_id {} collision (policy=reject)",
                            id_, sid);
                    continue;
                }
                std::uint16_t cand = sid + 1;
                while (cand < 0xFFFF && sid_owner.count(cand)) ++cand;
                sid = cand;
            }
            sid_owner[sid] = i;
            in.sid_remap[p.service_id] = sid;
        }

        auto restoreCc = [&](std::uint16_t out_pid) -> std::uint8_t {
            auto it = prev_cc.find(out_pid);
            return it == prev_cc.end() ? 0 : it->second;
        };

        // ES PID map.
        for (const auto& p : snap->programs) {
            for (const auto& es : p.streams) {
                const auto pin_it = pins.find(
                    pinKey(static_cast<std::uint16_t>(i), es.elementary_pid));
                std::uint16_t opid = (pin_it != pins.end())
                    ? pin_it->second
                    : es.elementary_pid;
                if (pin_it == pins.end() && pid_used[opid])
                    opid = allocFreePid(0x0200);
                if (opid < kPidSpace) pid_used[opid] = true;
                auto& e = in.pid_table[es.elementary_pid];
                e.out_pid = opid;
                e.out_cc  = restoreCc(opid);
            }
            // PCR PID — same treatment, identity unless pinned or colliding.
            if (p.pcr_pid != 0x1FFF) {
                auto& e = in.pid_table[p.pcr_pid];
                if (e.out_pid == 0xFFFF) {
                    const auto pin_it = pins.find(
                        pinKey(static_cast<std::uint16_t>(i), p.pcr_pid));
                    std::uint16_t opid = (pin_it != pins.end())
                        ? pin_it->second
                        : p.pcr_pid;
                    if (pin_it == pins.end() && pid_used[opid])
                        opid = allocFreePid(0x0200);
                    if (opid < kPidSpace) pid_used[opid] = true;
                    e.out_pid = opid;
                    e.out_cc  = restoreCc(opid);
                }
            }
        }
    }

    // Pass 3: synthesize output ProgramTable.
    ts::ParsedPat pat_synth;
    pat_synth.transport_stream_id    = cfg_.remux.transport_stream_id;
    pat_synth.version_number         = pat_version_;
    pat_synth.current_next_indicator = true;
    for (std::size_t i = 0; i < inputs_.size(); ++i) {
        auto& in = inputs_[i];
        if (in.output_pmt_pid == 0) continue;
        auto snap = in.program_table.current();
        if (!snap || snap->programs.empty()) continue;
        const auto& p0 = snap->programs.front();
        const auto sid_it = in.sid_remap.find(p0.service_id);
        if (sid_it == in.sid_remap.end()) continue;
        ts::PatEntry prog;
        prog.program_number = sid_it->second;
        prog.pmt_pid        = in.output_pmt_pid;
        pat_synth.programs.push_back(prog);
    }

    output_program_table_.clear();
    output_program_table_.ingestPat(pat_synth);

    // Push synthesized PMT for each input into the output table.
    for (std::size_t i = 0; i < inputs_.size(); ++i) {
        auto& in = inputs_[i];
        if (in.output_pmt_pid == 0) continue;
        auto snap = in.program_table.current();
        if (!snap || snap->programs.empty()) continue;
        const auto& src = snap->programs.front();
        const auto sid_it = in.sid_remap.find(src.service_id);
        if (sid_it == in.sid_remap.end()) continue;

        ts::ParsedPmt pmt;
        pmt.program_number         = sid_it->second;
        pmt.version_number         = in.pmt_version;
        pmt.current_next_indicator = true;
        pmt.pcr_pid = (src.pcr_pid != 0x1FFF
                       && in.pid_table[src.pcr_pid].out_pid != 0xFFFF)
            ? in.pid_table[src.pcr_pid].out_pid
            : src.pcr_pid;
        for (const auto& es : src.streams) {
            ts::PmtStream out_es;
            out_es.stream_type    = es.stream_type;
            out_es.elementary_pid = in.pid_table[es.elementary_pid].out_pid;
            if (!es.language.empty() && es.language.size() <= 3) {
                ts::RawDescriptor d;
                d.tag = 0x0A;
                d.body.resize(4);
                d.body[0] = static_cast<std::uint8_t>(es.language[0]);
                d.body[1] = static_cast<std::uint8_t>(
                    es.language.size() > 1 ? es.language[1] : ' ');
                d.body[2] = static_cast<std::uint8_t>(
                    es.language.size() > 2 ? es.language[2] : ' ');
                d.body[3] = 0x00;
                out_es.es_descriptors.push_back(std::move(d));
            }
            pmt.streams.push_back(std::move(out_es));
        }
        output_program_table_.ingestPmt(pmt);
    }

    syncPcrPids();
}

// Diff the set of output pcr_pids against pcr_pids_registered_ and apply
// changes to the restamper. PIDs that survive a PMT-version bump keep their
// anchor; only true add/remove triggers register/unregister. This matters
// across input PMT version updates that might add/remove a PCR PID.
void RemuxGateway::syncPcrPids() {
    auto out_snap = output_program_table_.current();
    std::unordered_set<std::uint16_t> wanted;
    if (out_snap) {
        for (const auto& p : out_snap->programs) {
            if (p.pcr_pid != 0x1FFF) wanted.insert(p.pcr_pid);
        }
    }
    for (auto pid : pcr_pids_registered_) {
        if (!wanted.contains(pid)) pcr_restamper_.unregisterPid(pid);
    }
    for (auto pid : wanted) {
        if (!pcr_pids_registered_.contains(pid)) pcr_restamper_.registerPid(pid);
    }
    pcr_pids_registered_ = std::move(wanted);
}

void RemuxGateway::sendOrBuffer(
        std::span<std::uint8_t, kTsPacketSize> pkt) {
    // PCR restamping happens at the single send chokepoint so ES, PSI, and
    // stuffing all advance the same byte counter and only one path can rewrite
    // PCR. PID is read from the (possibly already-rewritten) packet header.
    ts::TsPacketMut mut(pkt);
    pcr_restamper_.onPacketOut(mut, mut.view().pid());

    // FEC live path: hand each TS to the encoder, which batches into RTP and
    // emits column / row FEC. Bypasses CBR pacing — receivers using FEC are
    // expected to absorb input-paced jitter via their own buffers.
    if (fec_encoder_ && running_.load(std::memory_order_acquire)) {
        fec_encoder_->feedTsPacket(
            std::span<const std::uint8_t>(pkt.data(), kTsPacketSize));
        std::lock_guard<std::mutex> lk(stats_mu_);
        stats_.pkt_out += 1;
        return;
    }

    if (!output_buffer_) output_buffer_ = std::make_unique<OutputBuffer>();
    output_buffer_->append(std::span<const std::uint8_t, kTsPacketSize>(
        pkt.data(), kTsPacketSize));
    if (output_buffer_->full()) flushOutput();
}

void RemuxGateway::flushOutput() {
    if (!output_buffer_ || output_buffer_->fill == 0) return;
    bytes_in_window_ += output_buffer_->fill;
    if (running_.load(std::memory_order_acquire) && sockets_) {
        const std::uint32_t bitrate = cfg_.remux.target_bitrate_bps;
        // µs-precision pacing — sleep until the wall-clock instant at which
        // bytes_sent_total_ bytes should already be on the wire at the
        // configured CBR. Only engaged when bitrate > 0; bitrate=0 means
        // best-effort pass-through and we ship as fast as input arrives.
        if (bitrate > 0) {
            if (!pacing_anchor_set_) {
                ::clock_gettime(CLOCK_MONOTONIC, &pacing_anchor_);
                pacing_anchor_set_ = true;
            } else {
                const std::uint64_t ns_target =
                    (bytes_sent_total_ * 8'000'000'000ull) / bitrate;
                const timespec target = addNs(pacing_anchor_, ns_target);
                // Past-target → returns 0 immediately, no sleep. EINTR retried.
                while (::clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME,
                                         &target, nullptr) == EINTR) {}
            }
        }
        const ssize_t s = ::sendto(
            sockets_->out_fd.get(),
            output_buffer_->bytes.data(), output_buffer_->fill,
            MSG_DONTWAIT,
            reinterpret_cast<sockaddr*>(&sockets_->out_addr),
            sizeof(sockaddr_in));
        std::lock_guard<std::mutex> lk(stats_mu_);
        if (s == static_cast<ssize_t>(output_buffer_->fill)) {
            stats_.pkt_out   += output_buffer_->fill / kTsPacketSize;
            stats_.bytes_out += output_buffer_->fill;
            bytes_sent_total_ += output_buffer_->fill;
        } else {
            stats_.drops += output_buffer_->fill / kTsPacketSize;
        }
    } else {
        std::vector<std::uint8_t> v(
            output_buffer_->bytes.data(),
            output_buffer_->bytes.data() + output_buffer_->fill);
        output_test_queue_.push_back(std::move(v));
    }
    output_buffer_->reset();
}

// ─── PSI emission ────────────────────────────────────────────────────────────
//
// Emits the regenerated PAT, per-input PMT, and (optionally) merged SDT for
// the combined MPTS. Source PSI from the inputs is dropped at the PSI tap
// in processInputTsPacket(); only these self-built sections reach the wire.
//
// Section data is built from the synthesized output_program_table_ snapshot
// so the emitted PIDs match what the ES forwarder is rewriting to.

void RemuxGateway::emitPsi() {
    auto out_snap = output_program_table_.current();
    if (!out_snap || out_snap->programs.empty()) return;

    // ── PAT ───────────────────────────────────────────────────────────────
    {
        ts::PatBuildInput pat_in;
        pat_in.transport_stream_id    = cfg_.remux.transport_stream_id;
        pat_in.version_number         = pat_version_;
        pat_in.current_next_indicator = true;
        pat_in.programs.reserve(out_snap->programs.size());
        for (const auto& p : out_snap->programs)
            pat_in.programs.push_back({p.service_id, p.pmt_pid});
        const auto sec = ts::buildPatSection(pat_in);
        auto pkts = ts::packetizeSection(kPidPat, sec, pat_cc_);
        for (auto& pk : pkts)
            sendOrBuffer(std::span<std::uint8_t, kTsPacketSize>(
                pk.data(), kTsPacketSize));
    }

    // ── PMT (one per output program) ──────────────────────────────────────
    for (std::size_t i = 0; i < inputs_.size(); ++i) {
        auto& in = inputs_[i];
        if (in.output_pmt_pid == 0) continue;

        // Find the matching output ProgramInfo for this input by walking
        // sid_remap (input service_id → output service_id).
        if (in.sid_remap.empty()) continue;
        const std::uint16_t out_sid = in.sid_remap.begin()->second;
        const ts::ProgramInfo* out_p = nullptr;
        for (const auto& p : out_snap->programs)
            if (p.service_id == out_sid) { out_p = &p; break; }
        if (!out_p) continue;

        ts::PmtBuildInput pmt_in;
        pmt_in.program_number         = out_sid;
        pmt_in.version_number         = in.pmt_version;
        pmt_in.current_next_indicator = true;
        pmt_in.pcr_pid                = out_p->pcr_pid;
        pmt_in.streams.reserve(out_p->streams.size());
        for (const auto& es : out_p->streams) {
            ts::PmtStream s;
            s.stream_type    = es.stream_type;
            s.elementary_pid = es.elementary_pid;
            if (!es.language.empty() && es.language.size() <= 3) {
                ts::RawDescriptor d;
                d.tag = 0x0A;
                d.body.resize(4);
                d.body[0] = static_cast<std::uint8_t>(es.language[0]);
                d.body[1] = static_cast<std::uint8_t>(
                    es.language.size() > 1 ? es.language[1] : ' ');
                d.body[2] = static_cast<std::uint8_t>(
                    es.language.size() > 2 ? es.language[2] : ' ');
                d.body[3] = 0x00;
                s.es_descriptors.push_back(std::move(d));
            }
            pmt_in.streams.push_back(std::move(s));
        }
        const auto sec = ts::buildPmtSection(pmt_in);
        auto pkts = ts::packetizeSection(in.output_pmt_pid, sec, in.pmt_cc);
        for (auto& pk : pkts)
            sendOrBuffer(std::span<std::uint8_t, kTsPacketSize>(
                pk.data(), kTsPacketSize));
    }

    // ── SDT (merged across inputs) ────────────────────────────────────────
    if (cfg_.remux.emit_sdt) {
        ts::SdtBuildInput sdt_in;
        sdt_in.transport_stream_id    = cfg_.remux.transport_stream_id;
        sdt_in.original_network_id    = cfg_.remux.original_network_id;
        sdt_in.version_number         = sdt_version_;
        sdt_in.current_next_indicator = true;
        sdt_in.actual                 = true;
        sdt_in.services.reserve(out_snap->programs.size());
        for (std::size_t i = 0; i < inputs_.size(); ++i) {
            auto& in = inputs_[i];
            auto in_snap = in.program_table.current();
            if (!in_snap || in_snap->programs.empty()) continue;
            const auto& src = in_snap->programs.front();
            const auto sid_it = in.sid_remap.find(src.service_id);
            if (sid_it == in.sid_remap.end()) continue;

            ts::SdtService svc;
            svc.service_id                 = sid_it->second;
            svc.eit_present_following_flag = src.eit_present;
            svc.running_status             = src.running_status ? src.running_status : 4u;
            svc.service_name               = src.service_name;
            svc.provider_name              = src.provider_name;
            // Service-type heuristic: 0x19 (H.264 HD) when any video stream is
            // present, otherwise 0x02 (digital radio). UI consumers can refine.
            std::uint8_t service_type = 0x02;
            for (const auto& es : src.streams) {
                if (es.stream_type == 0x1B || es.stream_type == 0x24
                    || es.stream_type == 0x02) {
                    service_type = 0x19;
                    break;
                }
            }
            svc.descriptors.push_back(ts::makeServiceDescriptor(
                service_type, src.provider_name, src.service_name));
            sdt_in.services.push_back(std::move(svc));
        }
        if (!sdt_in.services.empty()) {
            const auto sec = ts::buildSdtSection(sdt_in);
            auto pkts = ts::packetizeSection(kPidSdt, sec, sdt_cc_);
            for (auto& pk : pkts)
                sendOrBuffer(std::span<std::uint8_t, kTsPacketSize>(
                    pk.data(), kTsPacketSize));
        }
    }

    flushOutput();
}

void RemuxGateway::emitPsiIfDue(std::chrono::steady_clock::time_point now) {
    using namespace std::chrono;
    if (last_pat_emit_ == steady_clock::time_point{}) {
        last_pat_emit_ = last_pmt_emit_ = last_sdt_emit_ = now - hours(1);
    }
    const auto pat_due = now - last_pat_emit_ >= milliseconds(cfg_.remux.pat_period_ms);
    const auto pmt_due = now - last_pmt_emit_ >= milliseconds(cfg_.remux.pmt_period_ms);
    const auto sdt_due = now - last_sdt_emit_ >= milliseconds(cfg_.remux.sdt_period_ms);
    if (pat_due || pmt_due || sdt_due) {
        emitPsi();
        if (pat_due) last_pat_emit_ = now;
        if (pmt_due) last_pmt_emit_ = now;
        if (sdt_due) last_sdt_emit_ = now;
    }
    topUpStuffing(now);
}

// ─── Bitrate stuffing ────────────────────────────────────────────────────────
//
// When cfg.remux.target_bitrate_bps > 0 the operator wants a constant-rate
// MPTS regardless of input ES bitrate. We track bytes flushed since the last
// top-up tick; when the monotonic-clock interval suggests the rate budget
// hasn't been used, NULL packets (PID 0x1FFF) fill the deficit.
//
// flushOutput() then paces individual sendto() calls via clock_nanosleep_abs
// against the same monotonic anchor (fix-A5). Net effect: ES + PSI + NULL
// packets all hit the wire at exactly target_bitrate_bps with sub-millisecond
// jitter, which combined with PcrRestamper rewriting PCRs as
// pcr_anchor + bytes_emitted * 8 * 27e6 / bitrate keeps downstream PCR jitter
// within ISO 13818-1 Annex E bounds.

void RemuxGateway::topUpStuffing(std::chrono::steady_clock::time_point now) {
    using namespace std::chrono;
    if (cfg_.remux.target_bitrate_bps == 0) return;
    if (last_top_up_ == steady_clock::time_point{}) {
        last_top_up_      = now;
        bytes_in_window_  = 0;
        return;
    }
    const auto elapsed_us = duration_cast<microseconds>(now - last_top_up_).count();
    if (elapsed_us <= 0) return;
    const std::uint64_t expected_bytes =
        (static_cast<std::uint64_t>(cfg_.remux.target_bitrate_bps)
         * static_cast<std::uint64_t>(elapsed_us)) / 8000000ull;
    if (bytes_in_window_ < expected_bytes) {
        const std::uint64_t deficit = expected_bytes - bytes_in_window_;
        const std::size_t   nulls   =
            static_cast<std::size_t>(deficit / ts::kTsPacketSize);
        // Per-iteration copy so sendOrBuffer's mutable span argument is safe;
        // null packets carry no PCR so the restamper would only advance the
        // byte counter, but a fresh buffer keeps the contract clean.
        for (std::size_t k = 0; k < nulls; ++k) {
            auto null_pkt = ts::makeNullPacket();
            sendOrBuffer(std::span<std::uint8_t, ts::kTsPacketSize>(
                null_pkt.data(), ts::kTsPacketSize));
        }
    }
    last_top_up_     = now;
    bytes_in_window_ = 0;
}

void RemuxGateway::runIoThread() {
    if (numa_node_ >= 0) numa::bindCurrentThreadToNode(numa_node_);
    auto& lg = logger_ ? *logger_ : *spdlog::default_logger();

    std::array<std::uint8_t, kRecvBufBytes> buf{};
    std::vector<pollfd> pfds(inputs_.size());
    while (!stop_flag_.load(std::memory_order_acquire)) {
        for (std::size_t i = 0; i < inputs_.size(); ++i) {
            pfds[i].fd      = sockets_->in_fds[i].get();
            pfds[i].events  = POLLIN;
            pfds[i].revents = 0;
        }
        // 5 ms heartbeat — keeps topUpStuffing cadence tight enough that
        // the paced output never sees more than 5 ms of underrun even when
        // input is bursty. The cost is wakeup overhead (~200 µs/sec at idle),
        // negligible compared to per-packet pacing.
        const int rc = ::poll(pfds.data(), pfds.size(), 5);
        if (rc < 0) {
            if (errno == EINTR) continue;
            lg.error("RemuxGateway[{}]: poll: {}", id_, std::strerror(errno));
            break;
        }
        if (rc > 0) {
            for (std::size_t i = 0; i < inputs_.size(); ++i) {
                if (!(pfds[i].revents & POLLIN)) continue;
                const ssize_t n = ::recv(pfds[i].fd, buf.data(), buf.size(), 0);
                if (n <= 0) continue;
                processInputDatagram(i, std::span<const std::uint8_t>(
                    buf.data(), static_cast<std::size_t>(n)));
            }
        }
        emitPsiIfDue(std::chrono::steady_clock::now());
    }
    flushOutput();
}

} // namespace liveqx::gateway
