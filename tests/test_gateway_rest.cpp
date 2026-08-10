// fix18 step 5 — REST CRUD integration tests for /api/gateways.
//
// Spins up ControlApi on an ephemeral port with a real GatewayManager
// (gateway_root in TMPDIR) and drives create/list/get/patch/delete via
// httplib::Client. Gateway play/stop are exercised over loopback unicast
// to keep CI sandbox-friendly.

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <thread>

#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "api/ChannelManager.h"
#include "api/ControlApi.h"
#include "gateway/GatewayManager.h"

namespace {

namespace fs = std::filesystem;
using nlohmann::json;
using namespace std::chrono_literals;

fs::path makeTmpRoot(const std::string& tag) {
    auto base = fs::temp_directory_path() / "liveqx_gw_rest";
    fs::create_directories(base);
    static std::atomic<uint64_t> seq{0};
    auto stamp = std::to_string(
        std::chrono::system_clock::now().time_since_epoch().count());
    auto root = base / (tag + "_" + stamp + "_" + std::to_string(seq++));
    fs::create_directories(root);
    return root;
}

struct ApiFixture {
    liveqx::gateway::GatewayManager gw;
    ChannelManager                          manager;
    ControlApi                              api;

    ApiFixture(int port, fs::path gw_root)
        : gw(std::move(gw_root)),
          manager(nullptr, fs::path{}),
          api(port, manager, /*metrics=*/nullptr, {}, &gw) {
        // Mirror main.cpp bootstrap: pick up any pre-existing gw{id}-{name}
        // dirs from the root so reload-style tests exercise the real path.
        gw.loadFromRoot();
        api.start();
        waitListening(port);
    }
    ~ApiFixture() { api.stop(); }

    static void waitListening(int port) {
        httplib::Client cli("127.0.0.1", port);
        cli.set_connection_timeout(0, 50'000);
        for (int i = 0; i < 100; ++i) {
            auto r = cli.Get("/healthz");
            if (r && r->status == 200) return;
            std::this_thread::sleep_for(20ms);
        }
        FAIL() << "ControlApi never started listening on port " << port;
    }
};

// Each test picks a unique pair of ports so parallel ctest doesn't clash.
constexpr int kBasePort   = 18200;
constexpr int kBaseInPort = 49300;

json simpleCfg(int in_port, int out_port) {
    return json{
        {"name",    "gw-rest"},
        {"input",   {{"address", "127.0.0.1"}, {"port", in_port}}},
        {"outputs", json::array({
            json{{"address", "127.0.0.1"}, {"port", out_port}},
        })},
    };
}

// ─── UDP helpers for fan-out/E2E tests ───────────────────────────────────────

void sendUdp(int port, int count, const std::string& payload) {
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    ASSERT_GE(fd, 0);
    sockaddr_in a{};
    a.sin_family      = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port        = htons(static_cast<uint16_t>(port));
    for (int i = 0; i < count; ++i) {
        ssize_t s = ::sendto(fd, payload.data(), payload.size(), 0,
                             reinterpret_cast<sockaddr*>(&a), sizeof(a));
        ASSERT_EQ(s, static_cast<ssize_t>(payload.size())) << ::strerror(errno);
        std::this_thread::sleep_for(1ms);
    }
    ::close(fd);
}

struct UdpReceiver {
    int fd = -1;
    explicit UdpReceiver(int port) {
        fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        EXPECT_GE(fd, 0);
        const int reuse = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        timeval tv{}; tv.tv_sec = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        sockaddr_in a{};
        a.sin_family      = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port        = htons(static_cast<uint16_t>(port));
        EXPECT_EQ(::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)), 0);
    }
    ~UdpReceiver() { if (fd >= 0) ::close(fd); }

    int drain(int max_pkts, std::chrono::milliseconds budget) {
        char buf[2048];
        int got = 0;
        const auto deadline = std::chrono::steady_clock::now() + budget;
        while (got < max_pkts && std::chrono::steady_clock::now() < deadline) {
            ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
            if (n > 0) ++got;
        }
        return got;
    }
};

// ─── Tests ───────────────────────────────────────────────────────────────────

TEST(GatewayRest, ListIsInitiallyEmpty) {
    ApiFixture f(kBasePort + 0, makeTmpRoot("rest-empty"));
    httplib::Client cli("127.0.0.1", kBasePort + 0);
    auto r = cli.Get("/api/gateways");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 200);
    auto body = json::parse(r->body);
    EXPECT_TRUE(body.is_array());
    EXPECT_EQ(body.size(), 0u);
}

TEST(GatewayRest, CreateReturns201AndIdAssigned) {
    ApiFixture f(kBasePort + 1, makeTmpRoot("rest-create"));
    httplib::Client cli("127.0.0.1", kBasePort + 1);

    auto cfg = simpleCfg(kBaseInPort + 1, kBaseInPort + 101);
    auto r = cli.Post("/api/gateways", cfg.dump(), "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 201);
    auto body = json::parse(r->body);
    EXPECT_EQ(body["id"], 1);
}

TEST(GatewayRest, CreateBadJsonReturns400) {
    ApiFixture f(kBasePort + 2, makeTmpRoot("rest-bad"));
    httplib::Client cli("127.0.0.1", kBasePort + 2);

    auto r = cli.Post("/api/gateways", "{ not json", "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400);
}

TEST(GatewayRest, CreateMissingInputReturns400) {
    ApiFixture f(kBasePort + 3, makeTmpRoot("rest-noinput"));
    httplib::Client cli("127.0.0.1", kBasePort + 3);

    auto r = cli.Post("/api/gateways",
                      json{{"name","x"},{"outputs",json::array()}}.dump(),
                      "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400);
}

TEST(GatewayRest, GetByIdReturnsStatus) {
    ApiFixture f(kBasePort + 4, makeTmpRoot("rest-get"));
    httplib::Client cli("127.0.0.1", kBasePort + 4);

    auto cfg = simpleCfg(kBaseInPort + 4, kBaseInPort + 104);
    cli.Post("/api/gateways", cfg.dump(), "application/json");

    auto r = cli.Get("/api/gateways/1");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 200);
    auto body = json::parse(r->body);
    EXPECT_EQ(body["id"], 1);
    EXPECT_EQ(body["name"], "gw-rest");
    EXPECT_EQ(body["running"], false);
    ASSERT_EQ(body["outputs"].size(), 1u);
    EXPECT_EQ(body["outputs"][0]["id"], "out0");
}

TEST(GatewayRest, GetByIdUnknownReturns404) {
    ApiFixture f(kBasePort + 5, makeTmpRoot("rest-404"));
    httplib::Client cli("127.0.0.1", kBasePort + 5);
    auto r = cli.Get("/api/gateways/999");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 404);
}

TEST(GatewayRest, PlayThenStop) {
    ApiFixture f(kBasePort + 6, makeTmpRoot("rest-play"));
    httplib::Client cli("127.0.0.1", kBasePort + 6);

    auto cfg = simpleCfg(kBaseInPort + 6, kBaseInPort + 106);
    cli.Post("/api/gateways", cfg.dump(), "application/json");

    auto r1 = cli.Post("/api/gateways/1/play", "", "application/json");
    ASSERT_TRUE(r1);
    EXPECT_EQ(r1->status, 200);

    auto r2 = cli.Get("/api/gateways/1");
    ASSERT_TRUE(r2);
    EXPECT_EQ(json::parse(r2->body)["running"], true);

    auto r3 = cli.Post("/api/gateways/1/stop", "", "application/json");
    ASSERT_TRUE(r3);
    EXPECT_EQ(r3->status, 200);

    auto r4 = cli.Get("/api/gateways/1");
    EXPECT_EQ(json::parse(r4->body)["running"], false);
}

TEST(GatewayRest, PatchOutputsHotSwap) {
    ApiFixture f(kBasePort + 7, makeTmpRoot("rest-patch"));
    httplib::Client cli("127.0.0.1", kBasePort + 7);

    auto cfg = simpleCfg(kBaseInPort + 7, kBaseInPort + 107);
    cli.Post("/api/gateways", cfg.dump(), "application/json");
    cli.Post("/api/gateways/1/play", "", "application/json");

    json patch_body{
        {"outputs", json::array({
            json{{"address","127.0.0.1"},{"port", kBaseInPort + 207}},
            json{{"address","127.0.0.1"},{"port", kBaseInPort + 208}},
        })},
    };
    auto r = cli.Patch("/api/gateways/1", patch_body.dump(), "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 200);
    auto body = json::parse(r->body);
    ASSERT_EQ(body["outputs"].size(), 2u);
    EXPECT_EQ(body["outputs"][0]["port"], kBaseInPort + 207);
    EXPECT_EQ(body["running"], true);   // hot-swap kept it running
}

TEST(GatewayRest, PatchInputReturns400) {
    ApiFixture f(kBasePort + 8, makeTmpRoot("rest-bad-patch"));
    httplib::Client cli("127.0.0.1", kBasePort + 8);

    auto cfg = simpleCfg(kBaseInPort + 8, kBaseInPort + 108);
    cli.Post("/api/gateways", cfg.dump(), "application/json");

    json patch_body{{"input", {{"address","127.0.0.1"},{"port", 60000}}}};
    auto r = cli.Patch("/api/gateways/1", patch_body.dump(), "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400);
}

TEST(GatewayRest, DeleteRemovesGateway) {
    ApiFixture f(kBasePort + 9, makeTmpRoot("rest-delete"));
    httplib::Client cli("127.0.0.1", kBasePort + 9);

    auto cfg = simpleCfg(kBaseInPort + 9, kBaseInPort + 109);
    cli.Post("/api/gateways", cfg.dump(), "application/json");

    auto r = cli.Delete("/api/gateways/1");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 200);

    auto r2 = cli.Get("/api/gateways");
    EXPECT_EQ(json::parse(r2->body).size(), 0u);
}

TEST(GatewayRest, EndToEndFanOutOneToFiveOverRest) {
    // Drive a 1→5 forwarder entirely through REST: create, play, pump UDP
    // through the input port, drain each receiver, GET status to confirm
    // stats were updated. Exercises the full happy path that operators hit.
    ApiFixture f(kBasePort + 12, makeTmpRoot("rest-e2e"));
    httplib::Client cli("127.0.0.1", kBasePort + 12);

    constexpr int in_port    = kBaseInPort + 312;
    const std::vector<int> outs = {
        kBaseInPort + 313, kBaseInPort + 314, kBaseInPort + 315,
        kBaseInPort + 316, kBaseInPort + 317,
    };

    json cfg{
        {"name",    "fanout-rest"},
        {"input",   {{"address","127.0.0.1"},{"port",in_port}}},
        {"outputs", json::array()},
    };
    for (int p : outs)
        cfg["outputs"].push_back(json{{"address","127.0.0.1"},{"port",p}});

    auto created = cli.Post("/api/gateways", cfg.dump(), "application/json");
    ASSERT_TRUE(created);
    ASSERT_EQ(created->status, 201);

    // Bind receivers BEFORE play so we don't lose the first packets.
    std::vector<std::unique_ptr<UdpReceiver>> rcvs;
    for (int p : outs) rcvs.emplace_back(std::make_unique<UdpReceiver>(p));

    auto played = cli.Post("/api/gateways/1/play", "", "application/json");
    ASSERT_TRUE(played);
    ASSERT_EQ(played->status, 200);

    constexpr int kPkts = 25;
    sendUdp(in_port, kPkts, "fan-payload");

    int total = 0;
    for (auto& r : rcvs) total += r->drain(kPkts, 1500ms);
    EXPECT_GE(total, static_cast<int>(outs.size()) * (kPkts - 2));

    // Stop so getStats flushes the io_thread's local counters under mu.
    cli.Post("/api/gateways/1/stop", "", "application/json");

    auto status = cli.Get("/api/gateways/1");
    ASSERT_TRUE(status);
    auto body = json::parse(status->body);
    EXPECT_GE(body["pkt_in"].get<uint64_t>(),  static_cast<uint64_t>(kPkts - 2));
    EXPECT_GE(body["pkt_out"].get<uint64_t>(), static_cast<uint64_t>((kPkts - 2) * outs.size() / 2));
    EXPECT_EQ(body["running"], false);
}

TEST(GatewayRest, PatchHotSwapRoutesTrafficToNewOutputs) {
    // Start running with one output, hot-swap to a different port via PATCH,
    // then verify packets land on the new port (not the old one).
    ApiFixture f(kBasePort + 13, makeTmpRoot("rest-hot"));
    httplib::Client cli("127.0.0.1", kBasePort + 13);

    constexpr int in_port  = kBaseInPort + 320;
    constexpr int old_port = kBaseInPort + 321;
    constexpr int new_port = kBaseInPort + 322;

    json cfg{
        {"name",    "hot-rest"},
        {"input",   {{"address","127.0.0.1"},{"port",in_port}}},
        {"outputs", json::array({
            json{{"address","127.0.0.1"},{"port",old_port}},
        })},
    };
    cli.Post("/api/gateways", cfg.dump(), "application/json");

    UdpReceiver old_rcv(old_port);
    UdpReceiver new_rcv(new_port);

    cli.Post("/api/gateways/1/play", "", "application/json");
    sendUdp(in_port, 10, "pre-patch");
    EXPECT_GE(old_rcv.drain(10, 800ms), 5);

    json patch_body{{"outputs", json::array({
        json{{"address","127.0.0.1"},{"port",new_port}},
    })}};
    auto r = cli.Patch("/api/gateways/1", patch_body.dump(), "application/json");
    ASSERT_TRUE(r);
    ASSERT_EQ(r->status, 200);

    sendUdp(in_port, 10, "post-patch");
    EXPECT_GE(new_rcv.drain(10, 800ms), 5);
    // Old output should no longer receive — drain briefly to confirm 0.
    EXPECT_EQ(old_rcv.drain(10, 200ms), 0);
}

TEST(GatewayRest, ReloadAfterRestartReconstructsGateway) {
    // Persistence smoke test from the REST POV: first ApiFixture creates a
    // gateway, then we tear it down and spin up a *fresh* GatewayManager on
    // the same root. The reload must pick up the gw1-* directory.
    auto root = makeTmpRoot("rest-reload");

    {
        ApiFixture f(kBasePort + 14, root);
        httplib::Client cli("127.0.0.1", kBasePort + 14);
        auto cfg = simpleCfg(kBaseInPort + 330, kBaseInPort + 331);
        cfg["name"] = "persisted";
        ASSERT_EQ(
            cli.Post("/api/gateways", cfg.dump(), "application/json")->status,
            201);
    }

    {
        ApiFixture f(kBasePort + 15, root);
        httplib::Client cli("127.0.0.1", kBasePort + 15);
        auto list = cli.Get("/api/gateways");
        ASSERT_TRUE(list);
        auto body = json::parse(list->body);
        ASSERT_EQ(body.size(), 1u);
        EXPECT_EQ(body[0]["name"], "persisted");
        // Reloaded gateways start stopped (operator paused-intent semantic).
        EXPECT_EQ(body[0]["running"], false);
    }
}

TEST(GatewayRest, SystemInterfacesReturnsLoopback) {
    // Available regardless of whether GatewayManager is wired in.
    ChannelManager     mgr(nullptr, fs::path{});
    constexpr int port = kBasePort + 11;
    ControlApi api(port, mgr, /*metrics=*/nullptr, {}, /*gateways=*/nullptr);
    api.start();
    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(0, 50'000);
    for (int i = 0; i < 100; ++i) {
        auto r = cli.Get("/healthz");
        if (r && r->status == 200) break;
        std::this_thread::sleep_for(20ms);
    }

    auto r = cli.Get("/api/system/interfaces");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 200);
    auto body = json::parse(r->body);
    ASSERT_TRUE(body.is_array());
    // Loopback is always present on Linux test hosts; assert its shape.
    bool found_lo = false;
    for (const auto& nic : body) {
        EXPECT_TRUE(nic.contains("name"));
        EXPECT_TRUE(nic["addresses"].is_array());
        EXPECT_TRUE(nic.contains("up"));
        EXPECT_TRUE(nic.contains("loopback"));
        EXPECT_TRUE(nic.contains("multicast"));
        if (nic["loopback"] == true) {
            found_lo = true;
            // 127.0.0.1 must be among the addresses for the loopback NIC.
            bool has_127 = false;
            for (const auto& a : nic["addresses"])
                if (a == "127.0.0.1") has_127 = true;
            EXPECT_TRUE(has_127);
        }
    }
    EXPECT_TRUE(found_lo);
    api.stop();
}

// fix29 c12: /api/system/gpu must always be available (independent of any
// GPU hardware) and present a stable per-backend shape. On a stock CPU
// build all GPU backends are built_in=false; only x264 is true.
TEST(GatewayRest, SystemGpuReportsAllBackends) {
    ChannelManager     mgr(nullptr, fs::path{});
    constexpr int port = kBasePort + 16;
    ControlApi api(port, mgr, /*metrics=*/nullptr, {}, /*gateways=*/nullptr);
    api.start();
    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(0, 50'000);
    for (int i = 0; i < 100; ++i) {
        auto r = cli.Get("/healthz");
        if (r && r->status == 200) break;
        std::this_thread::sleep_for(20ms);
    }

    auto r = cli.Get("/api/system/gpu");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 200);
    auto body = json::parse(r->body);
    ASSERT_TRUE(body.is_object());
    for (const char* k : {"nvenc", "qsv", "vaapi", "x264"}) {
        ASSERT_TRUE(body.contains(k)) << "missing key: " << k;
        ASSERT_TRUE(body[k].is_object());
        ASSERT_TRUE(body[k].contains("built_in"));
        ASSERT_TRUE(body[k].contains("codec_registered"));
        EXPECT_TRUE(body[k]["built_in"].is_boolean());
        EXPECT_TRUE(body[k]["codec_registered"].is_boolean());
    }
    // x264 is always linked; libx264 codec must be registered too.
    EXPECT_EQ(body["x264"]["built_in"],         true);
    EXPECT_EQ(body["x264"]["codec_registered"], true);
    api.stop();
}

TEST(GatewayRest, GatewayManagerNullSurfaces503) {
    // Build ControlApi with gateways=nullptr and ensure /api/gateways
    // returns a 503 with explicit reason (handler still registered, not 404).
    ChannelManager     mgr(nullptr, fs::path{});
    constexpr int port = kBasePort + 10;
    ControlApi api(port, mgr, /*metrics=*/nullptr, {}, /*gateways=*/nullptr);
    api.start();
    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(0, 50'000);
    for (int i = 0; i < 100; ++i) {
        auto r = cli.Get("/healthz");
        if (r && r->status == 200) break;
        std::this_thread::sleep_for(20ms);
    }
    auto r = cli.Get("/api/gateways");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 503);
    auto body = json::parse(r->body);
    EXPECT_EQ(body["error"], "gateways_not_configured");
    api.stop();
}

}  // namespace
