// REST integration test for POST /api/streams/probe.
//
// Spins up ControlApi on an ephemeral port, streams a synthetic MPEG-TS
// (PAT+PMT+SDT) to loopback UDP, calls the probe endpoint, asserts that
// the JSON payload reflects the injected programs/services.

#include <atomic>
#include <chrono>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include "api/ChannelManager.h"
#include "api/ControlApi.h"
#include "gateway/ts/PsiBuilder.h"
#include "gateway/ts/PsiParser.h"
#include "gateway/ts/TsPacket.h"

using nlohmann::json;
using namespace std::chrono_literals;
namespace ts = liveqx::gateway::ts;

namespace {

std::uint16_t pickFreeUdpPort() {
    int s = ::socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in a{};
    a.sin_family      = AF_INET;
    a.sin_port        = 0;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    EXPECT_EQ(::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)), 0);
    socklen_t len = sizeof(a);
    EXPECT_EQ(::getsockname(s, reinterpret_cast<sockaddr*>(&a), &len), 0);
    std::uint16_t p = ntohs(a.sin_port);
    ::close(s);
    return p;
}

std::vector<std::uint8_t> buildCycle() {
    ts::PatBuildInput pat;
    pat.transport_stream_id = 7;
    pat.programs.push_back({200, 0x100});
    auto pat_sec = ts::buildPatSection(pat);
    std::uint8_t cc = 0;
    auto pat_pkts = ts::packetizeSection(ts::kPidPat, pat_sec, cc);

    ts::PmtBuildInput pmt;
    pmt.program_number = 200;
    pmt.pcr_pid        = 0x300;
    ts::PmtStream v; v.stream_type = 0x1B; v.elementary_pid = 0x300;
    ts::PmtStream a; a.stream_type = 0x0F; a.elementary_pid = 0x301;
    a.es_descriptors.push_back({0x0A, {'e', 'n', 'g', 0x00}});
    pmt.streams.push_back(v);
    pmt.streams.push_back(a);
    auto pmt_sec = ts::buildPmtSection(pmt);
    std::uint8_t cc_pmt = 0;
    auto pmt_pkts = ts::packetizeSection(0x100, pmt_sec, cc_pmt);

    ts::SdtBuildInput sdt;
    sdt.transport_stream_id = 7;
    sdt.original_network_id = 99;
    ts::SdtService svc;
    svc.service_id    = 200;
    svc.service_name  = "Probe Ch";
    svc.provider_name = "Probe Prov";
    svc.descriptors.push_back(
        ts::makeServiceDescriptor(0x01, svc.provider_name, svc.service_name));
    sdt.services.push_back(svc);
    auto sdt_sec = ts::buildSdtSection(sdt);
    std::uint8_t cc_sdt = 0;
    auto sdt_pkts = ts::packetizeSection(ts::kPidSdt, sdt_sec, cc_sdt);

    auto null_pkt = ts::makeNullPacket();

    std::vector<std::uint8_t> out;
    auto push = [&](const auto& pk) {
        out.insert(out.end(), pk.begin(), pk.end());
    };
    for (const auto& p : pat_pkts) push(p);
    for (const auto& p : pmt_pkts) push(p);
    for (const auto& p : sdt_pkts) push(p);
    for (int i = 0; i < 4; ++i) push(null_pkt);
    return out;
}

void streamer(std::uint16_t port,
              const std::vector<std::uint8_t>& cycle,
              std::atomic<bool>& stop) {
    int s = ::socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port   = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &dst.sin_addr);

    constexpr std::size_t kChunk = ts::kTsPacketSize * 7;
    while (!stop.load()) {
        for (std::size_t off = 0; off + kChunk <= cycle.size(); off += kChunk) {
            ::sendto(s, cycle.data() + off, kChunk, 0,
                     reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
        }
        const std::size_t rem = cycle.size() % kChunk;
        if (rem) {
            ::sendto(s, cycle.data() + (cycle.size() - rem), rem, 0,
                     reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
        }
        std::this_thread::sleep_for(10ms);
    }
    ::close(s);
}

int kApiBasePort = 18420;

struct Fixture {
    ChannelManager manager;
    ControlApi     api;
    int            port;
    explicit Fixture(int p)
        : manager(nullptr, std::filesystem::path{}), api(p, manager), port(p) {
        api.start();
        httplib::Client cli("127.0.0.1", port);
        cli.set_connection_timeout(0, 50'000);
        for (int i = 0; i < 100; ++i) {
            auto r = cli.Get("/healthz");
            if (r && r->status == 200) return;
            std::this_thread::sleep_for(20ms);
        }
        ADD_FAILURE() << "ControlApi never started listening";
    }
    ~Fixture() { api.stop(); }
};

}  // namespace

TEST(StreamProbeRestTest, ReturnsProgramsAndServiceNames) {
    Fixture fx(kApiBasePort++);
    const auto udp_port = pickFreeUdpPort();
    const auto cycle    = buildCycle();

    std::atomic<bool> stop{false};
    std::thread th(streamer, udp_port, std::cref(cycle), std::ref(stop));

    httplib::Client cli("127.0.0.1", fx.port);
    cli.set_read_timeout(5, 0);
    json body = {
        {"address",     "127.0.0.1"},
        {"port",        udp_port},
        {"duration_ms", 500},
    };
    auto r = cli.Post("/api/streams/probe", body.dump(), "application/json");
    stop.store(true);
    th.join();

    ASSERT_TRUE(r != nullptr);
    ASSERT_EQ(r->status, 200) << r->body;
    auto j = json::parse(r->body);
    EXPECT_EQ(j["transport_stream_id"].get<int>(), 7);
    EXPECT_EQ(j["original_network_id"].get<int>(), 99);
    EXPECT_GT(j["packets_received"].get<std::uint64_t>(), 0u);
    EXPECT_GT(j["total_bitrate_bps"].get<std::uint64_t>(), 0u);

    ASSERT_EQ(j["programs"].size(), 1u);
    const auto& p = j["programs"][0];
    EXPECT_EQ(p["program_number"].get<int>(), 200);
    EXPECT_EQ(p["pmt_pid"].get<int>(), 0x100);
    EXPECT_EQ(p["pcr_pid"].get<int>(), 0x300);
    EXPECT_EQ(p["service_name"].get<std::string>(), "Probe Ch");
    EXPECT_EQ(p["provider_name"].get<std::string>(), "Probe Prov");

    ASSERT_EQ(p["streams"].size(), 2u);
    EXPECT_EQ(p["streams"][0]["pid"].get<int>(), 0x300);
    EXPECT_EQ(p["streams"][0]["stream_type"].get<int>(), 0x1B);
    EXPECT_EQ(p["streams"][0]["codec"].get<std::string>(), "H.264");
    EXPECT_EQ(p["streams"][1]["pid"].get<int>(), 0x301);
    EXPECT_EQ(p["streams"][1]["stream_type"].get<int>(), 0x0F);
    EXPECT_EQ(p["streams"][1]["codec"].get<std::string>(), "AAC");
    EXPECT_EQ(p["streams"][1]["language"].get<std::string>(), "eng");
}

TEST(StreamProbeRestTest, RejectsInvalidAddress) {
    Fixture fx(kApiBasePort++);
    httplib::Client cli("127.0.0.1", fx.port);
    cli.set_read_timeout(5, 0);
    json body = {{"address", "not-an-ip"}, {"port", 5000}, {"duration_ms", 200}};
    auto r = cli.Post("/api/streams/probe", body.dump(), "application/json");
    ASSERT_TRUE(r != nullptr);
    EXPECT_EQ(r->status, 502) << r->body;
    auto j = json::parse(r->body);
    EXPECT_EQ(j["error"].get<std::string>(), "probe_failed");
    EXPECT_NE(j["detail"].get<std::string>().find("invalid address"),
              std::string::npos);
}

TEST(StreamProbeRestTest, RejectsMissingAddressOrPort) {
    Fixture fx(kApiBasePort++);
    httplib::Client cli("127.0.0.1", fx.port);
    cli.set_read_timeout(5, 0);
    json body = {{"port", 0}};
    auto r = cli.Post("/api/streams/probe", body.dump(), "application/json");
    ASSERT_TRUE(r != nullptr);
    EXPECT_EQ(r->status, 400);
    auto j = json::parse(r->body);
    EXPECT_EQ(j["error"].get<std::string>(), "invalid_address_or_port");
}
