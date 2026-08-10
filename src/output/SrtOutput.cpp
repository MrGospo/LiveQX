#include <srt/srt.h>

#include "output/SrtOutput.h"
#include "utils/CpuAffinity.h"
#include "utils/Log.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string_view>
#include <thread>

// Global SRT init reference count — srt_startup/srt_cleanup are expensive
// (~2s each) and must be called only once across all SrtOutput instances.
static std::atomic<int> g_srt_refcount{0};

static void srtGlobalStart() {
    if (g_srt_refcount.fetch_add(1, std::memory_order_relaxed) == 0)
        srt_startup();
}

static void srtGlobalStop() {
    if (g_srt_refcount.fetch_sub(1, std::memory_order_relaxed) == 1)
        srt_cleanup();
}

#include <spdlog/spdlog.h>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#endif

// Maximum bytes per srt_sendmsg() call.
// 7 × 188 = 1316 bytes — the de-facto MPEG-TS over SRT payload size.
static constexpr int kChunkBytes = 1316;

// ─── Impl ─────────────────────────────────────────────────────────────────────

struct SrtOutput::Impl {
    SpscQueue<Packet, 64> queue;

    std::atomic<bool>     connected { false };
    std::atomic<bool>     running   { false };
    std::jthread          thread;

    std::function<void()> on_client_connected;

    // Cumulative stats (written only by the I/O thread).
    std::atomic<uint64_t> packets_sent    { 0 };
    std::atomic<uint64_t> bytes_sent      { 0 };
    std::atomic<double>   rtt_ms          { 0.0 };
    std::atomic<int64_t>  packets_dropped { 0 };  // cumulative SRT pktSndLoss

    mutable std::mutex   bps_mu;
    mutable uint64_t     bps_prev_bytes { 0 };
    mutable int64_t      bps_prev_ns    { 0 };
    mutable double       bitrate_bps    { 0.0 };
};

// ─── SrtOutput ────────────────────────────────────────────────────────────────

SrtOutput::SrtOutput(int port, int latency_ms, std::string bind_address)
    : port_(port), latency_ms_(latency_ms),
      bind_address_(std::move(bind_address)),
      impl_(std::make_unique<Impl>()) {}

SrtOutput::~SrtOutput() { stop(); }

spdlog::logger& SrtOutput::lg() noexcept {
    return logger_ ? *logger_ : *spdlog::default_logger();
}

void SrtOutput::onClientConnected(std::function<void()> cb) {
    impl_->on_client_connected = std::move(cb);
}

bool SrtOutput::start() {
    if (impl_->running.load()) return false; // already running

    srtGlobalStart();

    srt_setloghandler(nullptr, [](void*, int level, const char* /*file*/,
        int /*line*/, const char* area, const char* msg) {
        std::string_view sv(msg);
        if (sv.find("no pending connection") != std::string_view::npos) return;
        if      (level <= LOG_CRIT)    spdlog::critical("[SRT:{}] {}", area, msg);
        else if (level <= LOG_ERR)     spdlog::error   ("[SRT:{}] {}", area, msg);
        else if (level <= LOG_WARNING) spdlog::warn    ("[SRT:{}] {}", area, msg);
    });
    srt_setloglevel(LOG_WARNING);

    // Probe-bind to verify the port is available before starting the thread.
    SRTSOCKET probe = srt_create_socket();
    if (probe == SRT_INVALID_SOCK) {
        lg().error("SrtOutput: srt_create_socket failed");
        return false;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(static_cast<uint16_t>(port_));
    // Source-NIC pin: bind to a specific local IPv4 when configured, else
    // INADDR_ANY (kernel picks via routing table). Validating here means a
    // bogus bind_address fails start() rather than booting a half-broken
    // listener thread.
    if (bind_address_.empty()) {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else if (::inet_pton(AF_INET, bind_address_.c_str(), &addr.sin_addr) != 1) {
        lg().error("SrtOutput: invalid bind_address '{}'", bind_address_);
        srt_close(probe);
        return false;
    }

    if (srt_bind(probe, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        lg().error("SrtOutput: cannot bind {}:{}: {}",
                   bind_address_.empty() ? "0.0.0.0" : bind_address_,
                   port_, srt_getlasterror_str());
        srt_close(probe);
        return false;
    }
    srt_close(probe); // the thread will re-create the socket

    impl_->running.store(true);
    impl_->thread = std::jthread([this](std::stop_token st) { runThread(st); });

    lg().info("SrtOutput: started, listening on {}:{}",
              bind_address_.empty() ? "0.0.0.0" : bind_address_, port_);
    return true;
}

void SrtOutput::stop() {
    if (!impl_->running.exchange(false)) return; // уже остановлен
    impl_->thread.request_stop();
    if (impl_->thread.joinable()) impl_->thread.join();
    srtGlobalStop();
    lg().info("SrtOutput: stopped");
}

void SrtOutput::send(const Packet& pkt) {
    if (!impl_->running.load(std::memory_order_relaxed)) return;
    if (!impl_->queue.push(pkt) && impl_->connected.load(std::memory_order_relaxed))
        lg().warn("SrtOutput: queue full, dropping packet ({}B)", pkt.data.size());
}

bool SrtOutput::isHealthy() const {
    return impl_->connected.load(std::memory_order_acquire);
}

OutputStats SrtOutput::getStats() const {
    OutputStats s;
    s.packets_sent = impl_->packets_sent.load(std::memory_order_relaxed);
    s.bytes_sent   = impl_->bytes_sent.load(std::memory_order_relaxed);
    s.rtt_ms       = impl_->rtt_ms.load(std::memory_order_relaxed);

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

nlohmann::json SrtOutput::statusJson() const {
    auto j = IOutput::statusJson();
    j["transport"]        = "srt";
    j["port"]             = port_;
    j["latency_ms"]       = latency_ms_;
    j["bind_address"]     = bind_address_;
    j["connected"]        = impl_->connected.load(std::memory_order_acquire);
    j["packets_dropped"]  = impl_->packets_dropped.load(std::memory_order_relaxed);
    return j;
}

// ─── I/O thread ───────────────────────────────────────────────────────────────

void SrtOutput::runThread(std::stop_token st) {
    using namespace std::chrono_literals;
    numa::bindCurrentThreadToNode(numa_node_);
    Log::setThreadName("ch" + (channel_id_.empty() ? std::string("?") : channel_id_) + "-srt");

    // ── Create + configure server socket ─────────────────────────────────────
    SRTSOCKET srv = srt_create_socket();
    if (srv == SRT_INVALID_SOCK) {
        lg().error("SrtOutput::runThread: srt_create_socket failed");
        impl_->running.store(false);
        return;
    }

    // Non-blocking accept so we can honour stop requests.
    int non_block = 0; // false
    srt_setsockflag(srv, SRTO_RCVSYN, &non_block, sizeof(non_block));

    // LIVE mode — one srt_sendmsg = one MPEG-TS chunk.
    SRT_TRANSTYPE tt = SRTT_LIVE;
    srt_setsockopt(srv, 0, SRTO_TRANSTYPE, &tt, sizeof(tt));

    int lat = latency_ms_;
    srt_setsockopt(srv, 0, SRTO_LATENCY, &lat, sizeof(lat));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(static_cast<uint16_t>(port_));
    // start() already validated bind_address_ via inet_pton+probe-bind, so
    // a parse failure here is a programmer bug — assert via fallthrough to
    // INADDR_ANY rather than crash the I/O thread.
    if (bind_address_.empty()
        || ::inet_pton(AF_INET, bind_address_.c_str(), &addr.sin_addr) != 1) {
        addr.sin_addr.s_addr = INADDR_ANY;
    }

    if (srt_bind(srv, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0
        || srt_listen(srv, 1) < 0) {
        lg().error("SrtOutput::runThread: bind/listen failed: {}", srt_getlasterror_str());
        srt_close(srv);
        impl_->running.store(false);
        return;
    }

    // ── Accept loop ───────────────────────────────────────────────────────────
    while (!st.stop_requested() && impl_->running.load(std::memory_order_relaxed)) {
        sockaddr_in client_addr{};
        int addr_len = sizeof(client_addr);

        SRTSOCKET cli = srt_accept(
            srv,
            reinterpret_cast<sockaddr*>(&client_addr),
            &addr_len);

        if (cli == SRT_INVALID_SOCK) {
            std::this_thread::sleep_for(100ms);
            continue;
        }

        lg().info("SrtOutput: client connected");
        impl_->connected.store(true, std::memory_order_release);

        // Drain stale packets accumulated while no client was connected,
        // so the new client starts from a clean IDR without old PTS discontinuity.
        { std::size_t n = 0; while (impl_->queue.pop()) ++n;
          if (n) lg().debug("SrtOutput: drained {} stale packets on reconnect", n); }

        if (impl_->on_client_connected) impl_->on_client_connected();

        auto last_stats_time = std::chrono::steady_clock::now();

        // ── Send loop ─────────────────────────────────────────────────────────
        while (!st.stop_requested() && impl_->running.load(std::memory_order_relaxed)) {
            // Refresh RTT stats every second.
            auto now = std::chrono::steady_clock::now();
            if (now - last_stats_time >= 1s) {
                SRT_TRACEBSTATS perf{};
                if (srt_bistats(cli, &perf, /*clear=*/0, /*instantaneous=*/0) == 0) {
                    impl_->rtt_ms.store(perf.msRTT, std::memory_order_relaxed);
                    impl_->packets_dropped.store(perf.pktSndLoss, std::memory_order_relaxed);
                }
                last_stats_time = now;
            }

            auto opt_pkt = impl_->queue.pop();
            if (!opt_pkt) {
                std::this_thread::sleep_for(500us);
                continue;
            }

            // Fragment into kChunkBytes chunks and send each via srt_sendmsg.
            const uint8_t* ptr       = opt_pkt->data.data();
            size_t         remaining = opt_pkt->data.size();
            bool           send_ok   = true;

            while (remaining > 0 && send_ok) {
                const int chunk = static_cast<int>(
                    std::min<size_t>(remaining, kChunkBytes));

                const int sent = srt_sendmsg(
                    cli,
                    reinterpret_cast<const char*>(ptr),
                    chunk,
                    /*ttl=*/-1,
                    /*inorder=*/1);

                if (sent < 0) {
                    lg().warn("SrtOutput: srt_sendmsg failed: {}", srt_getlasterror_str());
                    send_ok = false;
                } else {
                    ptr       += sent;
                    remaining -= static_cast<size_t>(sent);
                    impl_->bytes_sent.fetch_add(static_cast<uint64_t>(sent),
                                                std::memory_order_relaxed);
                }
            }

            if (send_ok)
                impl_->packets_sent.fetch_add(1, std::memory_order_relaxed);
            else
                break; // connection broken — go back to accept
        }

        srt_close(cli);
        impl_->connected.store(false, std::memory_order_release);
        lg().info("SrtOutput: client disconnected");
    }

    srt_close(srv);
    impl_->running.store(false);
}
