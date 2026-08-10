#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

#include "output/MulticastOutput.h"
#include "output/MulticastOutputCfg.h"

using namespace liveqx::multicast;

namespace {

// Test scope: prove MulticastOutput actually delivers data over the kernel's
// multicast plumbing on loopback. We don't pull MulticastInput in here because
// that path requires FFmpeg + a real MPEG-TS stream — test_multicast_input_cfg
// already covers the FFmpeg-free input contract. Here we only need a raw UDP
// receiver subscribed to the same group.

constexpr const char* kTestGroup = "239.255.99.1";
constexpr int         kTestPort  = 47554;   // arbitrary high port unlikely to clash

// Joins kTestGroup on the loopback interface and listens for one datagram
// on kTestPort. Returns the populated buffer + bytes received, or 0/empty on
// timeout. Used by both multicastAvailable() and the actual test bodies.
struct RecvSocket {
    int fd = -1;
    ~RecvSocket() { if (fd >= 0) ::close(fd); }
};

RecvSocket openMulticastReceiver(int timeout_ms = 1000) {
    RecvSocket s;
    s.fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (s.fd < 0) return s;

    int reuse = 1;
    ::setsockopt(s.fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in bind_addr{};
    bind_addr.sin_family      = AF_INET;
    bind_addr.sin_port        = htons(kTestPort);
    bind_addr.sin_addr.s_addr = INADDR_ANY;
    if (::bind(s.fd, reinterpret_cast<sockaddr*>(&bind_addr), sizeof(bind_addr)) < 0) {
        ::close(s.fd); s.fd = -1; return s;
    }

    ip_mreq mreq{};
    ::inet_pton(AF_INET, kTestGroup, &mreq.imr_multiaddr);
    mreq.imr_interface.s_addr = htonl(INADDR_LOOPBACK);
    if (::setsockopt(s.fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        ::close(s.fd); s.fd = -1; return s;
    }

    timeval tv{};
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    ::setsockopt(s.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return s;
}

// CI / sandbox detection. WSL2 + GitHub runners may forbid multicast even on
// loopback; we GTEST_SKIP rather than fail. Probe is cheap (open + close).
bool multicastAvailable() {
    auto recv = openMulticastReceiver(50);
    return recv.fd >= 0;
}

OutputCfg makeOutCfg() {
    OutputCfg c;
    c.address        = kTestGroup;
    c.port           = kTestPort;
    c.bind_address   = "127.0.0.1";  // pin to loopback so the test doesn't
                                     // depend on the host's default route
    c.ttl            = 1;             // loopback only — don't escape lo
    c.send_buffer_kb = 64;
    return c;
}

} // namespace

// ─── tests ───────────────────────────────────────────────────────────────────

TEST(MulticastLoopback, SendReceivesOnLocalGroup) {
    if (!multicastAvailable()) GTEST_SKIP() << "multicast not supported in env";

    auto recv = openMulticastReceiver(2000);
    ASSERT_GE(recv.fd, 0);

    MulticastOutput out{makeOutCfg()};
    ASSERT_TRUE(out.start());

    // Build a small Packet — MulticastOutput chunks at 1316 bytes; this fits
    // in one datagram so we don't have to reassemble in the receiver.
    Packet pkt;
    pkt.data.resize(512);
    for (size_t i = 0; i < pkt.data.size(); ++i)
        pkt.data[i] = static_cast<uint8_t>(i & 0xFF);

    out.send(pkt);

    uint8_t buf[2048];
    sockaddr_in src{};
    socklen_t   src_len = sizeof(src);
    const ssize_t got = ::recvfrom(recv.fd, buf, sizeof(buf), 0,
                                   reinterpret_cast<sockaddr*>(&src), &src_len);
    ASSERT_GT(got, 0) << "recvfrom timed out: " << std::strerror(errno);
    EXPECT_EQ(static_cast<size_t>(got), pkt.data.size());
    EXPECT_EQ(0, std::memcmp(buf, pkt.data.data(), static_cast<size_t>(got)));

    out.stop();
}

TEST(MulticastLoopback, FragmentsLargePacket) {
    if (!multicastAvailable()) GTEST_SKIP() << "multicast not supported in env";

    auto recv = openMulticastReceiver(2000);
    ASSERT_GE(recv.fd, 0);

    MulticastOutput out{makeOutCfg()};
    ASSERT_TRUE(out.start());

    // 4 × 1316 = 5264 — must arrive as 4 separate datagrams.
    Packet pkt;
    pkt.data.resize(1316 * 4);
    for (size_t i = 0; i < pkt.data.size(); ++i)
        pkt.data[i] = static_cast<uint8_t>((i / 1316) + 1);  // 1,2,3,4 per chunk
    out.send(pkt);

    int chunks_received = 0;
    std::vector<uint8_t> assembled;
    for (int i = 0; i < 4; ++i) {
        uint8_t buf[2048];
        const ssize_t got = ::recvfrom(recv.fd, buf, sizeof(buf), 0, nullptr, nullptr);
        if (got <= 0) break;
        assembled.insert(assembled.end(), buf, buf + got);
        ++chunks_received;
    }

    EXPECT_EQ(chunks_received, 4);
    EXPECT_EQ(assembled.size(), pkt.data.size());
    EXPECT_EQ(assembled, pkt.data);

    out.stop();
}

TEST(MulticastLoopback, StatusJsonSurfacesTransportFields) {
    if (!multicastAvailable()) GTEST_SKIP() << "multicast not supported in env";

    MulticastOutput out{makeOutCfg()};
    ASSERT_TRUE(out.start());

    const auto j = out.statusJson();
    EXPECT_EQ(j["transport"], "multicast");
    EXPECT_EQ(j["address"],   kTestGroup);
    EXPECT_EQ(j["port"],      kTestPort);
    EXPECT_EQ(j["healthy"],   true);

    out.stop();
}
