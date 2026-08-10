#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <spdlog/sinks/null_sink.h>
#include <spdlog/spdlog.h>

#include "gateway/Gateway.h"
#include "gateway/GatewayCfg.h"

using namespace liveqx::gateway;

namespace {

// We deliberately use unicast loopback for these tests — multicast on
// loopback in CI containers is brittle (kernel must have the iface enabled
// for multicast). The unicast path covers all of: socket open + bind + recv
// loop + fan-out sendto + stats accounting. Multicast wiring is the same
// IP_MULTICAST_TTL/IF setsockopts as MulticastOutput already validates in
// test_multicast_loopback.

struct RecvSocket {
    int fd = -1;

    explicit RecvSocket(int port) {
        fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        EXPECT_GE(fd, 0);

        const int reuse = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        timeval tv{};
        tv.tv_sec  = 1;
        tv.tv_usec = 0;
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        sockaddr_in a{};
        a.sin_family      = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port        = htons(static_cast<uint16_t>(port));
        EXPECT_EQ(::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)), 0)
            << strerror(errno);
    }

    ~RecvSocket() { if (fd >= 0) ::close(fd); }

    // Returns bytes read, 0 on timeout, -1 on error.
    ssize_t recvOnce(char* buf, size_t cap) {
        return ::recv(fd, buf, cap, 0);
    }
};

// Sender — send N datagrams to 127.0.0.1:port with a recognisable payload.
void sendN(int port, int count, const std::string& payload) {
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    ASSERT_GE(fd, 0);

    sockaddr_in a{};
    a.sin_family      = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port        = htons(static_cast<uint16_t>(port));

    for (int i = 0; i < count; ++i) {
        ssize_t s = ::sendto(fd, payload.data(), payload.size(), 0,
                             reinterpret_cast<sockaddr*>(&a), sizeof(a));
        ASSERT_EQ(s, static_cast<ssize_t>(payload.size())) << strerror(errno);
        // Tiny breathing room so the kernel doesn't drop in burst.
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ::close(fd);
}

// Pick a random-ish high port for tests. We avoid hard-coding the same port
// for every test so parallel ctest runs don't clash.
int pickPort(int seed) { return 47600 + seed; }

GatewayCfg makeCfg(int in_port, std::vector<int> out_ports) {
    GatewayCfg cfg;
    cfg.input.address = "127.0.0.1";
    cfg.input.port    = in_port;
    for (size_t i = 0; i < out_ports.size(); ++i) {
        OutputCfg o;
        o.id      = "out" + std::to_string(i);
        o.address = "127.0.0.1";
        o.port    = out_ports[i];
        cfg.outputs.push_back(o);
    }
    return cfg;
}

std::shared_ptr<spdlog::logger> nullLogger() {
    auto sink = std::make_shared<spdlog::sinks::null_sink_mt>();
    auto lg   = std::make_shared<spdlog::logger>("gw-test", sink);
    lg->set_level(spdlog::level::off);
    return lg;
}

// ─── Tests ───────────────────────────────────────────────────────────────────

TEST(GatewayLoopback, OneToOneForwardsAllPackets) {
    const int in_port  = pickPort(0);
    const int out_port = pickPort(1);

    RecvSocket rcv(out_port);

    Gateway gw(1, "test", makeCfg(in_port, {out_port}));
    gw.setLogger(nullLogger());
    ASSERT_TRUE(gw.start());

    constexpr int kCount  = 50;
    const std::string pl  = "hello-gateway";
    sendN(in_port, kCount, pl);

    int received = 0;
    char buf[2048];
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (received < kCount && std::chrono::steady_clock::now() < deadline) {
        ssize_t n = rcv.recvOnce(buf, sizeof(buf));
        if (n > 0) {
            EXPECT_EQ(std::string(buf, n), pl);
            ++received;
        }
    }
    EXPECT_GE(received, kCount - 2);

    gw.stop();

    const auto s = gw.getStats();
    EXPECT_GE(s.pkt_in,  static_cast<uint64_t>(received));
    EXPECT_GE(s.pkt_out, static_cast<uint64_t>(received));
    EXPECT_EQ(s.drops,   0u);
}

TEST(GatewayLoopback, OneToFiveFansOutToAllOutputs) {
    const int in_port = pickPort(10);
    std::vector<int> out_ports = {
        pickPort(11), pickPort(12), pickPort(13), pickPort(14), pickPort(15),
    };
    std::vector<std::unique_ptr<RecvSocket>> rcvs;
    for (int p : out_ports) rcvs.emplace_back(std::make_unique<RecvSocket>(p));

    Gateway gw(2, "fanout", makeCfg(in_port, out_ports));
    gw.setLogger(nullLogger());
    ASSERT_TRUE(gw.start());

    constexpr int kCount = 30;
    const std::string pl = "fanout-payload";
    sendN(in_port, kCount, pl);

    char buf[2048];
    std::vector<int> got(out_ports.size(), 0);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        bool any = false;
        for (size_t i = 0; i < rcvs.size(); ++i) {
            ssize_t n = rcvs[i]->recvOnce(buf, sizeof(buf));
            if (n > 0) {
                EXPECT_EQ(std::string(buf, n), pl);
                ++got[i];
                any = true;
            }
        }
        if (!any) {
            bool all_done = true;
            for (int g : got) if (g < kCount) { all_done = false; break; }
            if (all_done) break;
        }
    }

    for (size_t i = 0; i < got.size(); ++i) {
        EXPECT_GE(got[i], kCount - 2)
            << "output[" << i << "] received only " << got[i] << "/" << kCount;
    }

    gw.stop();

    const auto s = gw.getStats();
    EXPECT_GE(s.pkt_out, static_cast<uint64_t>(kCount * out_ports.size() / 2));
}

TEST(GatewayLoopback, StartIsIdempotentAndStopIsIdempotent) {
    const int in_port  = pickPort(20);
    const int out_port = pickPort(21);
    Gateway gw(3, "idem", makeCfg(in_port, {out_port}));
    gw.setLogger(nullLogger());

    ASSERT_TRUE(gw.start());
    EXPECT_FALSE(gw.start());      // already running → reject
    gw.stop();
    gw.stop();                     // double-stop → no-op
    EXPECT_FALSE(gw.isRunning());
}

TEST(GatewayLoopback, RejectsStartWithNoOutputs) {
    GatewayCfg cfg;
    cfg.input.address = "127.0.0.1";
    cfg.input.port    = pickPort(30);
    Gateway gw(4, "no-outs", cfg);
    gw.setLogger(nullLogger());
    EXPECT_FALSE(gw.start());
}

TEST(GatewayLoopback, RejectsStartOnInvalidInputAddress) {
    GatewayCfg cfg = makeCfg(pickPort(40), {pickPort(41)});
    cfg.input.address = "not-an-ipv4";
    Gateway gw(5, "bad-in", cfg);
    gw.setLogger(nullLogger());
    EXPECT_FALSE(gw.start());
}

} // namespace
