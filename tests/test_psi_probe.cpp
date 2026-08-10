#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

#include "gateway/probe/PsiProbe.h"
#include "gateway/ts/PsiBuilder.h"
#include "gateway/ts/PsiParser.h"
#include "gateway/ts/TsPacket.h"

using namespace liveqx::gateway;

namespace {

std::uint16_t pickFreePort() {
    int s = ::socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in a{};
    a.sin_family      = AF_INET;
    a.sin_port        = 0;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    EXPECT_EQ(::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)), 0);
    socklen_t len = sizeof(a);
    EXPECT_EQ(::getsockname(s, reinterpret_cast<sockaddr*>(&a), &len), 0);
    std::uint16_t port = ntohs(a.sin_port);
    ::close(s);
    return port;
}

// Build one PAT+PMT+SDT cycle plus a handful of null packets. Callers loop
// this stream over UDP; the probe should latch onto the tables within one
// cycle.
std::vector<std::uint8_t> buildOneCycle() {
    // ─── PAT ───────────────────────────────────────────────────────────
    ts::PatBuildInput pat;
    pat.transport_stream_id = 1;
    pat.programs.push_back({100, 0x100});
    auto pat_sec = ts::buildPatSection(pat);
    std::uint8_t cc_pat = 0;
    auto pat_pkts = ts::packetizeSection(ts::kPidPat, pat_sec, cc_pat);

    // ─── PMT ───────────────────────────────────────────────────────────
    ts::PmtBuildInput pmt;
    pmt.program_number = 100;
    pmt.pcr_pid        = 0x200;
    ts::PmtStream vid; vid.stream_type = 0x1B; vid.elementary_pid = 0x200;
    ts::PmtStream aud; aud.stream_type = 0x0F; aud.elementary_pid = 0x201;
    // ISO 639 language descriptor (tag 0x0A, len 4): "rus" + audio_type 0.
    aud.es_descriptors.push_back(
        {0x0A, {'r', 'u', 's', 0x00}});
    pmt.streams.push_back(vid);
    pmt.streams.push_back(aud);
    auto pmt_sec = ts::buildPmtSection(pmt);
    std::uint8_t cc_pmt = 0;
    auto pmt_pkts = ts::packetizeSection(0x100, pmt_sec, cc_pmt);

    // ─── SDT ───────────────────────────────────────────────────────────
    ts::SdtBuildInput sdt;
    sdt.transport_stream_id = 1;
    sdt.original_network_id = 42;
    ts::SdtService svc;
    svc.service_id    = 100;
    svc.provider_name = "Test Provider";
    svc.service_name  = "Test Channel";
    svc.descriptors.push_back(
        ts::makeServiceDescriptor(0x01, svc.provider_name, svc.service_name));
    sdt.services.push_back(svc);
    auto sdt_sec = ts::buildSdtSection(sdt);
    std::uint8_t cc_sdt = 0;
    auto sdt_pkts = ts::packetizeSection(ts::kPidSdt, sdt_sec, cc_sdt);

    auto null_pkt = ts::makeNullPacket();

    std::vector<std::uint8_t> stream;
    auto push = [&](const auto& pkt) {
        stream.insert(stream.end(), pkt.begin(), pkt.end());
    };
    for (const auto& p : pat_pkts) push(p);
    for (const auto& p : pmt_pkts) push(p);
    for (const auto& p : sdt_pkts) push(p);
    // Add stuffing so a datagram is a realistic 7×188 chunk even for small
    // cycles — mirrors what a real broadcaster sends.
    for (int i = 0; i < 4; ++i) push(null_pkt);
    return stream;
}

// Send `cycle` to loopback:port in 7-packet UDP datagrams, on a loop, until
// `stop` becomes true. Runs on a helper thread.
void streamerThread(std::uint16_t port,
                    const std::vector<std::uint8_t>& cycle,
                    std::atomic<bool>& stop) {
    int s = ::socket(AF_INET, SOCK_DGRAM, 0);
    ASSERT_GE(s, 0);
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port   = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &dst.sin_addr);

    constexpr std::size_t kTs   = ts::kTsPacketSize;
    constexpr std::size_t kPkts = 7;
    const std::size_t chunk_bytes = kTs * kPkts;

    while (!stop.load(std::memory_order_relaxed)) {
        for (std::size_t off = 0; off + chunk_bytes <= cycle.size(); off += chunk_bytes) {
            ::sendto(s, cycle.data() + off, chunk_bytes, 0,
                     reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
        }
        // If the cycle length doesn't divide evenly, pad-send the remainder.
        const std::size_t rem = cycle.size() % chunk_bytes;
        if (rem) {
            ::sendto(s, cycle.data() + (cycle.size() - rem), rem, 0,
                     reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ::close(s);
}

}  // namespace

TEST(PsiProbeTest, ParsesPatPmtSdtFromLoopbackStream) {
    const auto port = pickFreePort();
    const auto cycle = buildOneCycle();

    std::atomic<bool> stop{false};
    std::thread t(streamerThread, port, std::cref(cycle), std::ref(stop));

    probe::ProbeOptions opts;
    opts.socket.address        = "127.0.0.1";
    opts.socket.port           = port;
    opts.socket.rcv_timeout_ms = 100;
    opts.duration_ms           = 500;

    auto res = probe::probe(opts);
    stop.store(true, std::memory_order_relaxed);
    t.join();

    ASSERT_TRUE(res.success) << res.error;
    EXPECT_GT(res.packets_received, 0u);
    EXPECT_GT(res.total_bitrate_bps, 0u);
    EXPECT_EQ(res.transport_stream_id, 1);
    EXPECT_EQ(res.original_network_id, 42);

    ASSERT_EQ(res.programs.size(), 1u);
    const auto& p = res.programs[0];
    EXPECT_EQ(p.program_number, 100);
    EXPECT_EQ(p.pmt_pid, 0x100);
    EXPECT_EQ(p.pcr_pid, 0x200);
    EXPECT_EQ(p.service_name,  "Test Channel");
    EXPECT_EQ(p.provider_name, "Test Provider");

    ASSERT_EQ(p.streams.size(), 2u);
    // Order preserved from PMT.
    EXPECT_EQ(p.streams[0].pid, 0x200);
    EXPECT_EQ(p.streams[0].stream_type, 0x1B);
    EXPECT_EQ(p.streams[0].codec, "H.264");

    EXPECT_EQ(p.streams[1].pid, 0x201);
    EXPECT_EQ(p.streams[1].stream_type, 0x0F);
    EXPECT_EQ(p.streams[1].codec, "AAC");
    EXPECT_EQ(p.streams[1].language, "rus");
}

TEST(PsiProbeTest, EmptyStreamReturnsSuccessWithZeroPackets) {
    const auto port = pickFreePort();

    probe::ProbeOptions opts;
    opts.socket.address        = "127.0.0.1";
    opts.socket.port           = port;
    opts.socket.rcv_timeout_ms = 50;
    opts.duration_ms           = 200;

    auto res = probe::probe(opts);

    EXPECT_TRUE(res.success);
    EXPECT_EQ(res.packets_received, 0u);
    EXPECT_TRUE(res.programs.empty());
    EXPECT_GE(res.duration_ms, 150);
}

TEST(PsiProbeTest, InvalidAddressReportsError) {
    probe::ProbeOptions opts;
    opts.socket.address = "not-an-ip";
    opts.socket.port    = 5000;
    opts.duration_ms    = 100;

    auto res = probe::probe(opts);

    EXPECT_FALSE(res.success);
    EXPECT_FALSE(res.error.empty());
    EXPECT_NE(res.error.find("invalid address"), std::string::npos);
}

TEST(PsiProbeTest, StreamTypeLabelHandlesUnknown) {
    EXPECT_EQ(probe::streamTypeLabel(0x1B), "H.264");
    EXPECT_EQ(probe::streamTypeLabel(0x24), "H.265");
    EXPECT_EQ(probe::streamTypeLabel(0x0F), "AAC");
    // Unknown types still render as something the UI can print.
    EXPECT_EQ(probe::streamTypeLabel(0xFE), "type 0xFE");
}
