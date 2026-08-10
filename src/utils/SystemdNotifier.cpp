#include "utils/SystemdNotifier.h"

#include "utils/Log.h"
#include "utils/RateLimitedLog.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>

#ifdef LIVEQX_ENABLE_SYSTEMD
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace liveqx::sysd {

namespace {

#ifdef LIVEQX_ENABLE_SYSTEMD

// $NOTIFY_SOCKET may be:
//   /run/systemd/notify         — absolute filesystem path
//   @abstract-name              — Linux abstract namespace (sun_path[0]=\0)
// Both are documented in sd_notify(3). Returns false if `path` is unusable.
bool buildSockaddr(const std::string& path,
                   sockaddr_un& addr,
                   socklen_t& out_len) {
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;

    if (path.empty()) return false;
    // sun_path on Linux is 108 bytes including the optional leading NUL for
    // the abstract namespace.
    if (path.size() >= sizeof(addr.sun_path)) return false;

    if (path.front() == '@') {
        addr.sun_path[0] = '\0';
        std::memcpy(addr.sun_path + 1, path.data() + 1, path.size() - 1);
        out_len = static_cast<socklen_t>(
            offsetof(sockaddr_un, sun_path) + path.size());
    } else {
        std::memcpy(addr.sun_path, path.data(), path.size());
        out_len = static_cast<socklen_t>(
            offsetof(sockaddr_un, sun_path) + path.size() + 1);
    }
    return true;
}

std::chrono::microseconds parseWatchdogUsec(const char* raw) {
    if (!raw || !*raw) return std::chrono::seconds(30);
    char* end = nullptr;
    errno = 0;
    const long long val = std::strtoll(raw, &end, 10);
    if (errno != 0 || end == raw || val <= 0) return std::chrono::seconds(30);
    // sd_notify cadence recommendation: WATCHDOG_USEC / 2.
    return std::chrono::microseconds(val / 2);
}

#endif  // LIVEQX_ENABLE_SYSTEMD

}  // namespace

SystemdNotifier::SystemdNotifier() {
#ifdef LIVEQX_ENABLE_SYSTEMD
    const char* env = std::getenv("NOTIFY_SOCKET");
    if (!env || !*env) {
        LOG_DEBUG("sd_notify: NOTIFY_SOCKET unset, running in disabled mode");
        return;
    }

    sockaddr_un addr{};
    socklen_t   len = 0;
    if (!buildSockaddr(env, addr, len)) {
        LOG_WARN("sd_notify: NOTIFY_SOCKET='{}' is too long or empty — "
                 "disabling notifications", env);
        return;
    }

    int fd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        LOG_WARN("sd_notify: socket() failed: {} — disabling notifications",
                 std::strerror(errno));
        return;
    }

    // Stash the address bytes. We reuse them on every send.
    addr_blob_.assign(reinterpret_cast<const char*>(&addr), len);
    addr_len_ = static_cast<int>(len);
    sock_fd_  = fd;
    enabled_  = true;

    interval_ = parseWatchdogUsec(std::getenv("WATCHDOG_USEC"));
    LOG_INFO("sd_notify: socket='{}' watchdog_interval={}ms",
             env, std::chrono::duration_cast<std::chrono::milliseconds>(
                       interval_).count());
#else
    // Build-time disabled — every method is a no-op below.
#endif
}

SystemdNotifier::~SystemdNotifier() {
    stopWatchdogLoop();
#ifdef LIVEQX_ENABLE_SYSTEMD
    if (sock_fd_ >= 0) ::close(sock_fd_);
#endif
}

void SystemdNotifier::sendRaw(const std::string& payload) noexcept {
#ifdef LIVEQX_ENABLE_SYSTEMD
    if (!enabled_ || sock_fd_ < 0) return;
    const auto* sa = reinterpret_cast<const sockaddr*>(addr_blob_.data());
    const ssize_t n = ::sendto(sock_fd_, payload.data(), payload.size(),
                               MSG_NOSIGNAL, sa, addr_len_);
    if (n < 0) {
        // Single warn line, rate-limited — a journal-spammer is worse than
        // silent loss when the notify socket goes flaky.
        LOG_WARN_RL(60, "sd_notify: sendto() failed: {}",
                    std::strerror(errno));
    }
#else
    (void)payload;
#endif
}

void SystemdNotifier::ready(const std::string& status) {
    sendRaw("READY=1\nSTATUS=" + status);
}

void SystemdNotifier::stopping(const std::string& status) {
    sendRaw("STOPPING=1\nSTATUS=" + status);
}

void SystemdNotifier::watchdog(const std::string& status) {
    sendRaw("WATCHDOG=1\nSTATUS=" + status);
}

void SystemdNotifier::startWatchdogLoop(StatusProvider provider) {
    if (!enabled_ || !provider) return;
    if (loop_run_.exchange(true)) return;  // already running

    loop_thread_ = std::jthread([this, p = std::move(provider)](
                                    std::stop_token st) {
        while (!st.stop_requested() &&
               loop_run_.load(std::memory_order_acquire)) {
            // Send first, then sleep — guarantees an early ping during boot.
            std::string status;
            try { status = p(); } catch (...) { status = "snapshot failed"; }
            watchdog(status);

            // Sleep in small chunks so stop_requested() unblocks promptly.
            const auto deadline = std::chrono::steady_clock::now() + interval_;
            while (!st.stop_requested() &&
                   loop_run_.load(std::memory_order_acquire) &&
                   std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    });
}

void SystemdNotifier::stopWatchdogLoop() {
    if (!loop_run_.exchange(false)) return;
    if (loop_thread_.joinable()) {
        loop_thread_.request_stop();
        loop_thread_.join();
    }
}

}  // namespace liveqx::sysd
