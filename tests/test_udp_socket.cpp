#include <gtest/gtest.h>

#include <chrono>
#include <cstring>
#include <string>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "gateway/net/UdpSocket.h"

using liveqx::gateway::net::openRxSocket;
using liveqx::gateway::net::nicNameToIPv4;
using liveqx::gateway::net::UdpRxOptions;

namespace {

// Bind a temporary sender to send a couple of bytes to the receiver on
// loopback. Kept minimal — we're not exercising the sender helpers here.
void sendToLoopback(std::uint16_t port, const std::string& payload) {
    int s = ::socket(AF_INET, SOCK_DGRAM, 0);
    ASSERT_GE(s, 0);
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port   = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &dst.sin_addr);
    ssize_t n = ::sendto(s, payload.data(), payload.size(), 0,
                         reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
    EXPECT_EQ(n, static_cast<ssize_t>(payload.size()));
    ::close(s);
}

std::uint16_t pickFreePort() {
    // Ask the kernel to allocate an ephemeral UDP port, then close and reuse
    // it. Cheaper than hard-coding a port and hoping.
    int s = ::socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port   = 0;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    EXPECT_EQ(::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)), 0);
    socklen_t len = sizeof(a);
    EXPECT_EQ(::getsockname(s, reinterpret_cast<sockaddr*>(&a), &len), 0);
    std::uint16_t port = ntohs(a.sin_port);
    ::close(s);
    return port;
}

} // namespace

TEST(UdpSocketTest, OpenRxOnLoopbackAndReceive) {
    const std::uint16_t port = pickFreePort();
    UdpRxOptions opts{
        .address        = "127.0.0.1",
        .port           = port,
        .interface_name = {},
        .interface_addr = {},
        .recv_buffer_kb = 64,
        .rcv_timeout_ms = 500,
    };
    std::string err;
    int fd = openRxSocket(opts, &err);
    ASSERT_GE(fd, 0) << "openRxSocket failed: " << err;

    const std::string payload = "hello-udp";
    sendToLoopback(port, payload);

    char buf[256] = {};
    ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
    ASSERT_GT(n, 0);
    EXPECT_EQ(std::string(buf, static_cast<std::size_t>(n)), payload);
    ::close(fd);
}

TEST(UdpSocketTest, RcvTimeoutFiresWhenNoTraffic) {
    const std::uint16_t port = pickFreePort();
    UdpRxOptions opts{
        .address        = "127.0.0.1",
        .port           = port,
        .rcv_timeout_ms = 100,
    };
    int fd = openRxSocket(opts);
    ASSERT_GE(fd, 0);

    auto t0 = std::chrono::steady_clock::now();
    char buf[16] = {};
    ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
    auto elapsed = std::chrono::steady_clock::now() - t0;
    ::close(fd);

    EXPECT_LT(n, 0);
    EXPECT_TRUE(errno == EAGAIN || errno == EWOULDBLOCK)
        << "unexpected errno " << errno << " (" << std::strerror(errno) << ")";
    // 100 ms timeout should return between 80 and 500 ms (loose bounds so we
    // don't flake on loaded CI).
    EXPECT_GE(elapsed, std::chrono::milliseconds(80));
    EXPECT_LT(elapsed, std::chrono::milliseconds(500));
}

TEST(UdpSocketTest, RejectsInvalidAddress) {
    UdpRxOptions opts{ .address = "not-an-ip", .port = 5000 };
    std::string err;
    int fd = openRxSocket(opts, &err);
    EXPECT_LT(fd, 0);
    EXPECT_NE(err.find("invalid address"), std::string::npos) << err;
}

TEST(UdpSocketTest, NicNameLoopbackResolves) {
    in_addr a{};
    // "lo" is virtually guaranteed to be present with 127.0.0.1 on any Linux
    // box that can run tests at all.
    ASSERT_TRUE(nicNameToIPv4("lo", a));
    char buf[INET_ADDRSTRLEN] = {};
    ::inet_ntop(AF_INET, &a, buf, sizeof(buf));
    EXPECT_STREQ(buf, "127.0.0.1");
}

TEST(UdpSocketTest, NicNameUnknownReturnsFalse) {
    in_addr a{};
    EXPECT_FALSE(nicNameToIPv4("this-nic-does-not-exist-xx", a));
}
