#pragma once
//
// fix21 — Native sd_notify(3) protocol implementation.
//
// Sends READY=1 / STOPPING=1 / WATCHDOG=1 / STATUS=... datagrams to the
// systemd notification socket (`$NOTIFY_SOCKET`) without linking against
// libsystemd. The protocol is one short newline-separated text frame per
// datagram, documented in `man sd_notify`.
//
// Build flag `LIVEQX_ENABLE_SYSTEMD` (driven by the CMake option
// `ENABLE_SYSTEMD`, default ON on Linux). When OFF, every method is an
// inline no-op and no syscalls happen at runtime.
//
// Outside of a systemd-managed start, `$NOTIFY_SOCKET` is empty — the
// notifier silently switches to Disabled mode. This means manual launches
// (`./liveqx config.json`) work unchanged.
//
#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <thread>

namespace liveqx::sysd {

class SystemdNotifier {
public:
    // Constructor reads the environment once. Safe to call without systemd
    // present — a missing $NOTIFY_SOCKET puts the notifier in Disabled mode.
    SystemdNotifier();
    ~SystemdNotifier();

    SystemdNotifier(const SystemdNotifier&)            = delete;
    SystemdNotifier& operator=(const SystemdNotifier&) = delete;

    // True when ENABLE_SYSTEMD=ON at build time AND $NOTIFY_SOCKET resolved
    // to a usable AF_UNIX address. False forces every emit() into a no-op.
    bool enabled() const noexcept { return enabled_; }

    // sd_notify(3) primitives. Each one sends a single datagram of the form
    // "<key>=1\nSTATUS=<message>". Failure is logged once at warn level
    // (rate-limited) and otherwise swallowed — a misbehaving notification
    // socket must never bring down the engine.
    void ready   (const std::string& status);
    void stopping(const std::string& status);
    void watchdog(const std::string& status);

    // Periodic keep-alive. Spawns a jthread that calls `status_provider()`
    // every `interval_` and emits WATCHDOG=1 + STATUS=<text>.
    //   - When $WATCHDOG_USEC is set (systemd's `WatchdogSec=` is in the
    //     unit), interval_ = WATCHDOG_USEC/2 microseconds — the documented
    //     recommendation for keep-alive cadence.
    //   - Otherwise interval_ = 30s (still gives `systemctl status` a fresh
    //     summary line every half-minute, even without a watchdog timer).
    // No-op when disabled() returns true.
    using StatusProvider = std::function<std::string()>;
    void startWatchdogLoop(StatusProvider provider);
    void stopWatchdogLoop();

    // Cadence at which startWatchdogLoop fires. Exposed for tests.
    std::chrono::microseconds watchdogInterval() const noexcept { return interval_; }

private:
    void sendRaw(const std::string& payload) noexcept;

    bool                       enabled_ = false;
    int                        sock_fd_ = -1;
    // Holds the destination sockaddr_un, but we keep it as raw bytes so the
    // header doesn't drag <sys/un.h> into every consumer.
    std::string                addr_blob_;
    int                        addr_len_  = 0;

    std::chrono::microseconds  interval_  = std::chrono::seconds(30);

    std::atomic<bool>          loop_run_{false};
    std::jthread               loop_thread_;
};

}  // namespace liveqx::sysd
