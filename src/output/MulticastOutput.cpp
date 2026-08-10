#include "output/MulticastOutput.h"
#include "utils/CpuAffinity.h"
#include "utils/Log.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>

#include <spdlog/spdlog.h>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
   using socklen_t = int;
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

using namespace liveqx::multicast;

// MPEG-TS over UDP de-facto datagram size: 7 × 188-byte TS packets = 1316 B.
// Same constant as SrtOutput uses for SRT.
static constexpr int kChunkBytes = 1316;

// ─── Impl ────────────────────────────────────────────────────────────────────

struct MulticastOutput::Impl {
    SpscQueue<Packet, 64> queue;

    int  fd = -1;
    sockaddr_in dst{};

    std::atomic<bool> running   { false };
    std::atomic<bool> healthy   { false };
    std::jthread      thread;

    std::atomic<uint64_t> packets_sent    { 0 };
    std::atomic<uint64_t> bytes_sent      { 0 };
    std::atomic<uint64_t> packets_dropped { 0 };
    std::atomic<uint64_t> send_errors     { 0 };

    mutable std::mutex   bps_mu;
    mutable uint64_t     bps_prev_bytes { 0 };
    mutable int64_t      bps_prev_ns    { 0 };
    mutable double       bitrate_bps    { 0.0 };
};

// ─── MulticastOutput ─────────────────────────────────────────────────────────

MulticastOutput::MulticastOutput(OutputCfg cfg)
    : cfg_(std::move(cfg)),
      impl_(std::make_unique<Impl>()) {}

MulticastOutput::~MulticastOutput() { stop(); }

spdlog::logger& MulticastOutput::lg() noexcept {
    return logger_ ? *logger_ : *spdlog::default_logger();
}

bool MulticastOutput::start() {
    if (impl_->running.load()) return false;

    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        lg().error("MulticastOutput: socket() failed: {}", std::strerror(errno));
        return false;
    }

    // SO_SNDBUF — bigger buffer absorbs encoder bursts (I-frames) before we
    // drop on overflow. setsockopt is best-effort; kernel may cap below
    // request via /proc/sys/net/core/wmem_max.
    const int sndbuf = cfg_.send_buffer_kb * 1024;
    if (::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf)) < 0)
        lg().warn("MulticastOutput: SO_SNDBUF={} failed: {}",
                  sndbuf, std::strerror(errno));

    // IP_MULTICAST_TTL controls hop count for outgoing datagrams.
    const int ttl = cfg_.ttl;
    if (::setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl)) < 0) {
        lg().error("MulticastOutput: IP_MULTICAST_TTL={} failed: {}",
                   ttl, std::strerror(errno));
        ::close(fd);
        return false;
    }
    if (ttl < 4)
        lg().warn("MulticastOutput: ttl={} is unusually low — "
                  "packets may not cross routers", ttl);

    // IP_MULTICAST_IF binds outgoing multicast to a specific NIC. Without it
    // the kernel picks the interface matching the default route, which on
    // multi-NIC servers (e.g. broadcast EPYC boxes) is the wrong choice.
    if (!cfg_.bind_address.empty()) {
        in_addr ifaddr{};
        if (::inet_pton(AF_INET, cfg_.bind_address.c_str(), &ifaddr) != 1) {
            lg().error("MulticastOutput: invalid interface address '{}'",
                       cfg_.bind_address);
            ::close(fd);
            return false;
        }
        if (::setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF,
                         &ifaddr, sizeof(ifaddr)) < 0) {
            lg().error("MulticastOutput: IP_MULTICAST_IF={} failed: {}",
                       cfg_.bind_address, std::strerror(errno));
            ::close(fd);
            return false;
        }
    }

    // Resolve destination once — addr never changes on a running output.
    impl_->dst = sockaddr_in{};
    impl_->dst.sin_family = AF_INET;
    impl_->dst.sin_port   = htons(static_cast<uint16_t>(cfg_.port));
    if (::inet_pton(AF_INET, cfg_.address.c_str(), &impl_->dst.sin_addr) != 1) {
        lg().error("MulticastOutput: invalid group address '{}'", cfg_.address);
        ::close(fd);
        return false;
    }

    impl_->fd = fd;
    impl_->running.store(true);
    impl_->healthy.store(true);
    impl_->thread = std::jthread([this](std::stop_token st) { runThread(st); });

    lg().info("MulticastOutput: started → udp://{}:{}{} ttl={} sndbuf={}K",
              cfg_.address, cfg_.port,
              cfg_.bind_address.empty() ? std::string()
                                          : " via " + cfg_.bind_address,
              cfg_.ttl, cfg_.send_buffer_kb);
    return true;
}

void MulticastOutput::stop() {
    if (!impl_->running.exchange(false)) return;
    impl_->thread.request_stop();
    if (impl_->thread.joinable()) impl_->thread.join();
    if (impl_->fd >= 0) {
        ::close(impl_->fd);
        impl_->fd = -1;
    }
    impl_->healthy.store(false);
    lg().info("MulticastOutput: stopped");
}

void MulticastOutput::send(const Packet& pkt) {
    if (!impl_->running.load(std::memory_order_relaxed)) return;
    if (!impl_->queue.push(pkt)) {
        impl_->packets_dropped.fetch_add(1, std::memory_order_relaxed);
        // Multicast has no flow control — log periodically, not per-packet.
        // Same SrtOutput-style behaviour: caller already debounces via SPSC fullness.
        lg().warn("MulticastOutput: queue full, dropping packet ({}B)",
                  pkt.data.size());
    }
}

bool MulticastOutput::isHealthy() const {
    return impl_->healthy.load(std::memory_order_acquire);
}

OutputStats MulticastOutput::getStats() const {
    OutputStats s;
    s.packets_sent = impl_->packets_sent.load(std::memory_order_relaxed);
    s.bytes_sent   = impl_->bytes_sent.load(std::memory_order_relaxed);

    using clock = std::chrono::steady_clock;
    const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        clock::now().time_since_epoch()).count();
    std::lock_guard lk(impl_->bps_mu);
    if (impl_->bps_prev_ns > 0) {
        const double dt = (now_ns - impl_->bps_prev_ns) / 1e9;
        if (dt >= 1.0) {
            impl_->bitrate_bps    = (s.bytes_sent - impl_->bps_prev_bytes) * 8.0 / dt;
            impl_->bps_prev_bytes = s.bytes_sent;
            impl_->bps_prev_ns    = now_ns;
        }
    } else {
        impl_->bps_prev_bytes = s.bytes_sent;
        impl_->bps_prev_ns    = now_ns;
    }
    s.bitrate_bps = impl_->bitrate_bps;
    return s;
}

nlohmann::json MulticastOutput::statusJson() const {
    auto j = IOutput::statusJson();
    j["transport"]        = "multicast";
    j["address"]          = cfg_.address;
    j["port"]             = cfg_.port;
    j["bind_address"]     = cfg_.bind_address;
    j["ttl"]              = cfg_.ttl;
    j["packets_dropped"]  = impl_->packets_dropped.load(std::memory_order_relaxed);
    j["send_errors"]      = impl_->send_errors.load(std::memory_order_relaxed);
    return j;
}

// ─── I/O thread ──────────────────────────────────────────────────────────────

void MulticastOutput::runThread(std::stop_token st) {
    using namespace std::chrono_literals;
    numa::bindCurrentThreadToNode(numa_node_);
    Log::setThreadName("ch" + (channel_id_.empty() ? std::string("?") : channel_id_) + "-mc-tx");

    while (!st.stop_requested() && impl_->running.load(std::memory_order_relaxed)) {
        auto opt_pkt = impl_->queue.pop();
        if (!opt_pkt) {
            std::this_thread::sleep_for(500us);
            continue;
        }

        const uint8_t* ptr       = opt_pkt->data.data();
        size_t         remaining = opt_pkt->data.size();
        bool           any_error = false;

        while (remaining > 0) {
            const size_t chunk = std::min<size_t>(remaining, kChunkBytes);
            const ssize_t sent = ::sendto(
                impl_->fd, reinterpret_cast<const char*>(ptr),
                chunk, 0,
                reinterpret_cast<const sockaddr*>(&impl_->dst),
                sizeof(impl_->dst));
            if (sent < 0) {
                impl_->send_errors.fetch_add(1, std::memory_order_relaxed);
                any_error = true;
                // Don't kill the thread on transient ENOBUFS/EAGAIN — bursts
                // are normal on UDP. Log throttled and bail this packet.
                lg().warn("MulticastOutput: sendto() failed: {}", std::strerror(errno));
                break;
            }
            impl_->bytes_sent.fetch_add(static_cast<uint64_t>(sent),
                                        std::memory_order_relaxed);
            ptr       += sent;
            remaining -= static_cast<size_t>(sent);
        }
        if (!any_error)
            impl_->packets_sent.fetch_add(1, std::memory_order_relaxed);
    }
}
