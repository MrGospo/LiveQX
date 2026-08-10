// fix21 — SystemdNotifier unit tests.
//
// We bind an AF_UNIX datagram receiver in the test process, point
// $NOTIFY_SOCKET at it, and assert that ready/stopping/watchdog send the
// exact payloads documented in sd_notify(3). This works without any
// systemd present and runs in CI containers.

#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "utils/SystemdNotifier.h"

#ifdef LIVEQX_ENABLE_SYSTEMD

namespace {

class NotifyReceiver {
public:
    explicit NotifyReceiver(const std::string& path) : path_(path) {
        ::unlink(path_.c_str());
        fd_ = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (fd_ < 0) throw std::runtime_error("socket() failed");

        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, path_.c_str(), sizeof(addr.sun_path) - 1);
        if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            ::close(fd_);
            throw std::runtime_error("bind() failed");
        }
    }

    ~NotifyReceiver() {
        if (fd_ >= 0) ::close(fd_);
        ::unlink(path_.c_str());
    }

    // Returns "" on timeout; the timeout is generous so flaky CI doesn't
    // false-negative.
    std::string recv(int timeout_ms = 1000) {
        struct timeval tv;
        tv.tv_sec  = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        char buf[256];
        const ssize_t n = ::recv(fd_, buf, sizeof(buf), 0);
        if (n <= 0) return {};
        return std::string(buf, buf + n);
    }

private:
    std::string path_;
    int         fd_ = -1;
};

// Deterministic socket path that survives parallel ctest — gtest's
// per-test pid + timer disambiguates concurrent test_systemd_notifier
// processes.
std::string makeSocketPath(const std::string& tag) {
    return std::string("/tmp/sc-notify-") + tag + "-" +
           std::to_string(::getpid()) + ".sock";
}

class SystemdNotifierTest : public ::testing::Test {
protected:
    void SetUp() override {
        ::unsetenv("NOTIFY_SOCKET");
        ::unsetenv("WATCHDOG_USEC");
    }

    void TearDown() override {
        ::unsetenv("NOTIFY_SOCKET");
        ::unsetenv("WATCHDOG_USEC");
    }
};

}  // namespace

TEST_F(SystemdNotifierTest, DisabledWhenNotifySocketUnset) {
    liveqx::sysd::SystemdNotifier n;
    EXPECT_FALSE(n.enabled());
    // Calling methods on a disabled notifier must be a no-op (no syscalls,
    // no crashes, no network).
    n.ready("hi");
    n.stopping("bye");
    n.watchdog("status");
}

TEST_F(SystemdNotifierTest, EmitsReadyMessage) {
    const std::string path = makeSocketPath("ready");
    NotifyReceiver rx(path);
    ::setenv("NOTIFY_SOCKET", path.c_str(), 1);

    liveqx::sysd::SystemdNotifier n;
    ASSERT_TRUE(n.enabled());

    n.ready("started, channels=2");
    EXPECT_EQ(rx.recv(), "READY=1\nSTATUS=started, channels=2");
}

TEST_F(SystemdNotifierTest, EmitsStoppingMessage) {
    const std::string path = makeSocketPath("stop");
    NotifyReceiver rx(path);
    ::setenv("NOTIFY_SOCKET", path.c_str(), 1);

    liveqx::sysd::SystemdNotifier n;
    ASSERT_TRUE(n.enabled());

    n.stopping("draining");
    EXPECT_EQ(rx.recv(), "STOPPING=1\nSTATUS=draining");
}

TEST_F(SystemdNotifierTest, EmitsWatchdogMessage) {
    const std::string path = makeSocketPath("wd");
    NotifyReceiver rx(path);
    ::setenv("NOTIFY_SOCKET", path.c_str(), 1);

    liveqx::sysd::SystemdNotifier n;
    ASSERT_TRUE(n.enabled());

    n.watchdog("ch=2 ok");
    EXPECT_EQ(rx.recv(), "WATCHDOG=1\nSTATUS=ch=2 ok");
}

TEST_F(SystemdNotifierTest, WatchdogIntervalDefaultsTo30sWhenEnvMissing) {
    const std::string path = makeSocketPath("interval-default");
    NotifyReceiver rx(path);
    ::setenv("NOTIFY_SOCKET", path.c_str(), 1);

    liveqx::sysd::SystemdNotifier n;
    EXPECT_EQ(n.watchdogInterval(), std::chrono::seconds(30));
}

TEST_F(SystemdNotifierTest, WatchdogIntervalReadsHalfOfWatchdogUsec) {
    const std::string path = makeSocketPath("interval-half");
    NotifyReceiver rx(path);
    ::setenv("NOTIFY_SOCKET", path.c_str(), 1);
    // 60s in microseconds → notifier should pick 30s.
    ::setenv("WATCHDOG_USEC", "60000000", 1);

    liveqx::sysd::SystemdNotifier n;
    EXPECT_EQ(n.watchdogInterval(), std::chrono::seconds(30));
}

TEST_F(SystemdNotifierTest, WatchdogIntervalIgnoresGarbage) {
    const std::string path = makeSocketPath("interval-garbage");
    NotifyReceiver rx(path);
    ::setenv("NOTIFY_SOCKET", path.c_str(), 1);
    ::setenv("WATCHDOG_USEC", "not-a-number", 1);

    liveqx::sysd::SystemdNotifier n;
    EXPECT_EQ(n.watchdogInterval(), std::chrono::seconds(30));
}

TEST_F(SystemdNotifierTest, AbstractNamespaceSocketAccepted) {
    // Linux abstract-namespace sockets (path starts with @, kernel-side
    // sun_path[0] = '\0'). Construct one in this process, point the
    // notifier at it via @-prefix, and verify the datagram lands.
    const std::string abs_name = std::string("@/tmp/sc-notify-abs-") +
                                  std::to_string(::getpid());

    int rxfd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    ASSERT_GE(rxfd, 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    addr.sun_path[0] = '\0';
    std::memcpy(addr.sun_path + 1,
                abs_name.data() + 1, abs_name.size() - 1);
    socklen_t alen = static_cast<socklen_t>(
        offsetof(sockaddr_un, sun_path) + abs_name.size());
    ASSERT_EQ(::bind(rxfd, reinterpret_cast<sockaddr*>(&addr), alen), 0)
        << "errno=" << errno;

    ::setenv("NOTIFY_SOCKET", abs_name.c_str(), 1);
    liveqx::sysd::SystemdNotifier n;
    ASSERT_TRUE(n.enabled());
    n.ready("abs ok");

    char buf[128];
    struct timeval tv{1, 0};
    ::setsockopt(rxfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    const ssize_t got = ::recv(rxfd, buf, sizeof(buf), 0);
    ::close(rxfd);
    ASSERT_GT(got, 0);
    EXPECT_EQ(std::string(buf, buf + got), "READY=1\nSTATUS=abs ok");
}

TEST_F(SystemdNotifierTest, WatchdogLoopFiresProvider) {
    const std::string path = makeSocketPath("loop");
    NotifyReceiver rx(path);
    ::setenv("NOTIFY_SOCKET", path.c_str(), 1);
    // Fast cadence: 200ms → interval = 100ms, so two pings inside ~300ms.
    ::setenv("WATCHDOG_USEC", "200000", 1);

    liveqx::sysd::SystemdNotifier n;
    ASSERT_TRUE(n.enabled());
    EXPECT_EQ(n.watchdogInterval(), std::chrono::microseconds(100000));

    int hits = 0;
    n.startWatchdogLoop([&hits] {
        return std::string("ch=") + std::to_string(++hits);
    });

    // First ping is immediate (loop emits before sleeping).
    auto first = rx.recv(800);
    ASSERT_FALSE(first.empty()) << "no datagram within 800ms";
    EXPECT_EQ(first, "WATCHDOG=1\nSTATUS=ch=1");

    auto second = rx.recv(800);
    ASSERT_FALSE(second.empty()) << "second datagram never arrived";
    EXPECT_EQ(second, "WATCHDOG=1\nSTATUS=ch=2");

    n.stopWatchdogLoop();
}

#else  // !LIVEQX_ENABLE_SYSTEMD

TEST(SystemdNotifierBuildOff, AlwaysDisabled) {
    liveqx::sysd::SystemdNotifier n;
    EXPECT_FALSE(n.enabled());
    n.ready("noop");
    n.stopping("noop");
    n.watchdog("noop");
}

#endif
