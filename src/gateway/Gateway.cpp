#include "gateway/Gateway.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <utility>

#include <spdlog/spdlog.h>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include "utils/CpuAffinity.h"

namespace liveqx::gateway {

namespace {

// ─── RAII fd ─────────────────────────────────────────────────────────────────

class FdGuard {
public:
    FdGuard() = default;
    explicit FdGuard(int fd) noexcept : fd_(fd) {}
    FdGuard(const FdGuard&) = delete;
    FdGuard& operator=(const FdGuard&) = delete;
    FdGuard(FdGuard&& o) noexcept : fd_(std::exchange(o.fd_, -1)) {}
    FdGuard& operator=(FdGuard&& o) noexcept {
        reset();
        fd_ = std::exchange(o.fd_, -1);
        return *this;
    }
    ~FdGuard() { reset(); }

    int  get() const noexcept { return fd_; }
    bool valid() const noexcept { return fd_ >= 0; }
    int  release() noexcept { return std::exchange(fd_, -1); }
    void reset(int fd = -1) noexcept {
        if (fd_ >= 0) ::close(fd_);
        fd_ = fd;
    }

private:
    int fd_ = -1;
};

// Resolve a NIC name (eth0/lo/wlp3s0) to its first IPv4 address. Used both
// for IP_MULTICAST_IF (which requires an in_addr) and for IP_ADD_MEMBERSHIP
// (which encodes the join-interface as IPv4 too).
bool nicNameToIPv4(const std::string& name, in_addr& out, std::string* err) {
    ifaddrs* ifa = nullptr;
    if (::getifaddrs(&ifa) != 0) {
        if (err) *err = std::string("getifaddrs: ") + std::strerror(errno);
        return false;
    }
    bool found = false;
    for (ifaddrs* p = ifa; p; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
        if (name != p->ifa_name) continue;
        out = reinterpret_cast<sockaddr_in*>(p->ifa_addr)->sin_addr;
        found = true;
        break;
    }
    ::freeifaddrs(ifa);
    if (!found && err) *err = "NIC '" + name + "' has no IPv4";
    return found;
}

bool isMulticast(const in_addr& a) noexcept {
    const auto host = ntohl(a.s_addr);
    return (host & 0xF0000000u) == 0xE0000000u;     // 224.0.0.0/4
}

// Open + bind input socket. For multicast we IP_ADD_MEMBERSHIP on the
// chosen NIC; for unicast we just bind. Returns -1 on failure (logged via
// `lg`). SO_RCVTIMEO=200ms lets the recv loop check stop_flag without
// poll/epoll machinery.
int openInputSocket(const InputCfg& cfg, spdlog::logger& lg) {
    in_addr group{};
    if (::inet_pton(AF_INET, cfg.address.c_str(), &group) != 1) {
        lg.error("Gateway: invalid input address '{}'", cfg.address);
        return -1;
    }

    FdGuard fd(::socket(AF_INET, SOCK_DGRAM, 0));
    if (!fd.valid()) {
        lg.error("Gateway: input socket() failed: {}", std::strerror(errno));
        return -1;
    }

    const int reuse = 1;
    if (::setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR,
                     &reuse, sizeof(reuse)) < 0) {
        lg.warn("Gateway: SO_REUSEADDR failed: {}", std::strerror(errno));
    }

    const int rcvbuf = cfg.recv_buffer_kb * 1024;
    if (::setsockopt(fd.get(), SOL_SOCKET, SO_RCVBUF,
                     &rcvbuf, sizeof(rcvbuf)) < 0) {
        lg.warn("Gateway: SO_RCVBUF={} failed: {}",
                rcvbuf, std::strerror(errno));
    }

    timeval tv{};
    tv.tv_sec  = 0;
    tv.tv_usec = 200 * 1000;
    if (::setsockopt(fd.get(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        lg.warn("Gateway: SO_RCVTIMEO failed: {}", std::strerror(errno));
    }

    sockaddr_in bind_addr{};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port   = htons(static_cast<uint16_t>(cfg.port));
    // Multicast bind: bind to ANY so we receive on the joined group; the
    // membership step routes only datagrams of that group to us.
    // Unicast bind: bind to the actual address (caller's "listen on this NIC
    // address"); 0.0.0.0 means any.
    if (isMulticast(group)) {
        bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else {
        bind_addr.sin_addr = group;
    }
    if (::bind(fd.get(), reinterpret_cast<sockaddr*>(&bind_addr),
               sizeof(bind_addr)) < 0) {
        lg.error("Gateway: bind({}:{}) failed: {}",
                 cfg.address, cfg.port, std::strerror(errno));
        return -1;
    }

    // SO_BINDTODEVICE gives the strongest NIC pinning (kernel routing
    // ignores the rest of the table). It needs CAP_NET_RAW. If we don't
    // have caps, we still set IP_MULTICAST_IF / IP_ADD_MEMBERSHIP on the
    // chosen NIC, which is enough for multicast traffic.
    if (!cfg.interface_name.empty()) {
        if (::setsockopt(fd.get(), SOL_SOCKET, SO_BINDTODEVICE,
                         cfg.interface_name.c_str(),
                         static_cast<socklen_t>(cfg.interface_name.size())) < 0) {
            lg.warn("Gateway: SO_BINDTODEVICE='{}' failed (need CAP_NET_RAW?): {}",
                    cfg.interface_name, std::strerror(errno));
        }
    }

    if (isMulticast(group)) {
        ip_mreq mreq{};
        mreq.imr_multiaddr = group;
        mreq.imr_interface.s_addr = htonl(INADDR_ANY);
        if (!cfg.interface_addr.empty()) {
            if (::inet_pton(AF_INET, cfg.interface_addr.c_str(),
                            &mreq.imr_interface) != 1) {
                lg.error("Gateway: invalid interface_address '{}'",
                         cfg.interface_addr);
                return -1;
            }
        } else if (!cfg.interface_name.empty()) {
            std::string err;
            in_addr nic_addr{};
            if (nicNameToIPv4(cfg.interface_name, nic_addr, &err))
                mreq.imr_interface = nic_addr;
            else
                lg.warn("Gateway: NIC '{}' lookup failed ({}), joining on default route",
                        cfg.interface_name, err);
        }
        if (::setsockopt(fd.get(), IPPROTO_IP, IP_ADD_MEMBERSHIP,
                         &mreq, sizeof(mreq)) < 0) {
            lg.error("Gateway: IP_ADD_MEMBERSHIP({}) failed: {}",
                     cfg.address, std::strerror(errno));
            return -1;
        }
    }

    return fd.release();
}

// Open one output socket. Returns -1 on failure. `dst_out` is the resolved
// sockaddr_in cached for sendto().
int openOutputSocket(const OutputCfg& cfg,
                     sockaddr_in& dst_out,
                     spdlog::logger& lg) {
    in_addr dst{};
    if (::inet_pton(AF_INET, cfg.address.c_str(), &dst) != 1) {
        lg.error("Gateway output[{}]: invalid address '{}'",
                 cfg.id, cfg.address);
        return -1;
    }

    FdGuard fd(::socket(AF_INET, SOCK_DGRAM, 0));
    if (!fd.valid()) {
        lg.error("Gateway output[{}]: socket() failed: {}",
                 cfg.id, std::strerror(errno));
        return -1;
    }

    const int sndbuf = cfg.send_buffer_kb * 1024;
    if (::setsockopt(fd.get(), SOL_SOCKET, SO_SNDBUF,
                     &sndbuf, sizeof(sndbuf)) < 0) {
        lg.warn("Gateway output[{}]: SO_SNDBUF={} failed: {}",
                cfg.id, sndbuf, std::strerror(errno));
    }

    if (isMulticast(dst)) {
        const int ttl = cfg.ttl;
        if (::setsockopt(fd.get(), IPPROTO_IP, IP_MULTICAST_TTL,
                         &ttl, sizeof(ttl)) < 0) {
            lg.error("Gateway output[{}]: IP_MULTICAST_TTL={} failed: {}",
                     cfg.id, ttl, std::strerror(errno));
            return -1;
        }

        in_addr ifaddr{};
        bool have_if = false;
        if (!cfg.interface_addr.empty()) {
            if (::inet_pton(AF_INET, cfg.interface_addr.c_str(), &ifaddr) != 1) {
                lg.error("Gateway output[{}]: invalid interface_address '{}'",
                         cfg.id, cfg.interface_addr);
                return -1;
            }
            have_if = true;
        } else if (!cfg.interface_name.empty()) {
            std::string err;
            if (nicNameToIPv4(cfg.interface_name, ifaddr, &err)) {
                have_if = true;
            } else {
                lg.warn("Gateway output[{}]: NIC '{}' lookup failed ({}), "
                        "using default route", cfg.id, cfg.interface_name, err);
            }
        }
        if (have_if) {
            if (::setsockopt(fd.get(), IPPROTO_IP, IP_MULTICAST_IF,
                             &ifaddr, sizeof(ifaddr)) < 0) {
                lg.error("Gateway output[{}]: IP_MULTICAST_IF failed: {}",
                         cfg.id, std::strerror(errno));
                return -1;
            }
        }
    } else if (!cfg.interface_name.empty()) {
        if (::setsockopt(fd.get(), SOL_SOCKET, SO_BINDTODEVICE,
                         cfg.interface_name.c_str(),
                         static_cast<socklen_t>(cfg.interface_name.size())) < 0) {
            lg.warn("Gateway output[{}]: SO_BINDTODEVICE='{}' failed "
                    "(need CAP_NET_RAW?): {}",
                    cfg.id, cfg.interface_name, std::strerror(errno));
        }
    }

    dst_out = sockaddr_in{};
    dst_out.sin_family = AF_INET;
    dst_out.sin_port   = htons(static_cast<uint16_t>(cfg.port));
    dst_out.sin_addr   = dst;

    return fd.release();
}

constexpr size_t kPacketBufBytes = 1600;   // > MTU + headroom

} // namespace

// ─── Gateway::Impl ───────────────────────────────────────────────────────────

struct Gateway::SocketSet {
    FdGuard                  in_fd;
    std::vector<FdGuard>     out_fds;
    std::vector<sockaddr_in> out_addrs;
};

Gateway::Gateway(int id, std::string name, GatewayCfg cfg)
    : id_(id), name_(std::move(name)), cfg_(std::move(cfg)) {}

Gateway::~Gateway() {
    Gateway::stop();
}

bool Gateway::start() {
    if (running_.load(std::memory_order_acquire)) return false;
    if (cfg_.outputs.empty()) {
        if (logger_) logger_->error("Gateway[{}]: cannot start — no outputs", id_);
        return false;
    }

    auto set = std::make_unique<SocketSet>();

    auto& lg = logger_ ? *logger_ : *spdlog::default_logger();

    const int in_fd_raw = openInputSocket(cfg_.input, lg);
    if (in_fd_raw < 0) return false;
    set->in_fd.reset(in_fd_raw);

    set->out_fds.reserve(cfg_.outputs.size());
    set->out_addrs.reserve(cfg_.outputs.size());
    for (const auto& ocfg : cfg_.outputs) {
        sockaddr_in dst{};
        const int out_fd = openOutputSocket(ocfg, dst, lg);
        if (out_fd < 0) {
            // SocketSet dtor closes already-opened fds in declared order.
            return false;
        }
        set->out_fds.emplace_back(out_fd);
        set->out_addrs.push_back(dst);
    }

    sockets_ = std::move(set);

    stop_flag_.store(false, std::memory_order_release);
    running_.store(true,  std::memory_order_release);
    io_thread_ = std::thread([this] { runIoThread(); });

    if (logger_) {
        logger_->info("Gateway[{}/{}]: started, in={}:{} → {} outputs",
                      id_, name_, cfg_.input.address, cfg_.input.port,
                      cfg_.outputs.size());
    }
    return true;
}

void Gateway::stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) return;
    stop_flag_.store(true, std::memory_order_release);
    if (io_thread_.joinable()) io_thread_.join();
    sockets_.reset();
    if (logger_) logger_->info("Gateway[{}/{}]: stopped", id_, name_);
}

GatewayStats Gateway::getStats() const {
    std::lock_guard<std::mutex> lk(stats_mu_);
    return stats_;
}

nlohmann::json Gateway::statusJson() const {
    const auto s = getStats();
    return nlohmann::json{
        {"id",        id_},
        {"name",      name_},
        {"running",   running_.load(std::memory_order_acquire)},
        {"input",     toJson(cfg_.input)},
        {"outputs",   toJson(cfg_).at("outputs")},
        {"pkt_in",    s.pkt_in},
        {"bytes_in",  s.bytes_in},
        {"pkt_out",   s.pkt_out},
        {"bytes_out", s.bytes_out},
        {"drops",     s.drops},
    };
}

void Gateway::setCfg(GatewayCfg cfg) {
    cfg_ = std::move(cfg);
}

void Gateway::runIoThread() {
    if (numa_node_ >= 0) numa::bindCurrentThreadToNode(numa_node_);

    char buf[kPacketBufBytes];
    auto& lg = logger_ ? *logger_ : *spdlog::default_logger();

    uint64_t local_pkt_in    = 0;
    uint64_t local_bytes_in  = 0;
    uint64_t local_pkt_out   = 0;
    uint64_t local_bytes_out = 0;
    uint64_t local_drops     = 0;

    auto last_flush = std::chrono::steady_clock::now();
    constexpr auto kFlushInterval = std::chrono::milliseconds(200);

    while (!stop_flag_.load(std::memory_order_acquire)) {
        const ssize_t n = ::recv(sockets_->in_fd.get(), buf, sizeof(buf), 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                // SO_RCVTIMEO firing is the loop's heartbeat; let stop_flag
                // re-check happen on next iteration.
            } else {
                lg.error("Gateway[{}]: recv failed: {}", id_, std::strerror(errno));
                break;
            }
        } else if (n == 0) {
            // Empty datagram — UDP allows it. Count as in but don't fan out.
            ++local_pkt_in;
        } else {
            ++local_pkt_in;
            local_bytes_in += static_cast<uint64_t>(n);
            for (size_t i = 0; i < sockets_->out_fds.size(); ++i) {
                const ssize_t s = ::sendto(
                    sockets_->out_fds[i].get(), buf, static_cast<size_t>(n),
                    MSG_DONTWAIT,
                    reinterpret_cast<sockaddr*>(&sockets_->out_addrs[i]),
                    sizeof(sockaddr_in));
                if (s == n) {
                    ++local_pkt_out;
                    local_bytes_out += static_cast<uint64_t>(n);
                } else {
                    ++local_drops;
                }
            }
        }

        const auto now = std::chrono::steady_clock::now();
        if (now - last_flush >= kFlushInterval) {
            std::lock_guard<std::mutex> lk(stats_mu_);
            stats_.pkt_in    += local_pkt_in;
            stats_.bytes_in  += local_bytes_in;
            stats_.pkt_out   += local_pkt_out;
            stats_.bytes_out += local_bytes_out;
            stats_.drops     += local_drops;
            local_pkt_in = local_bytes_in = 0;
            local_pkt_out = local_bytes_out = local_drops = 0;
            last_flush = now;
        }
    }

    {
        std::lock_guard<std::mutex> lk(stats_mu_);
        stats_.pkt_in    += local_pkt_in;
        stats_.bytes_in  += local_bytes_in;
        stats_.pkt_out   += local_pkt_out;
        stats_.bytes_out += local_bytes_out;
        stats_.drops     += local_drops;
    }
}

} // namespace liveqx::gateway
