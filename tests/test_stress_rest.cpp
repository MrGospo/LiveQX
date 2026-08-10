// fix26 c11 — REST integration tests for /api/stress/*.
//
// We construct a real ControlApi with a real StressService backed by a
// temp report dir, but we avoid kicking actual stress runs (channels=0)
// so the tests stay <1s. Coverage focuses on the REST contract:
//   GET  /api/stress/config        — returns the in-memory config
//   PUT  /api/stress/config        — validates + swaps
//   POST /api/stress/start         — refuses when enabled=false
//   GET  /api/stress/status        — runner state surfaces correctly
//   GET  /api/stress/reports       — empty array when no runs yet
//   GET  /api/stress/reports/{id}  — 404 for unknown id
//
// 503 fallback is covered by the no-StressService nullptr path.

#include <atomic>
#include <chrono>
#include <filesystem>
#include <thread>

#include <unistd.h>

#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include "api/ChannelManager.h"
#include "api/ControlApi.h"
#include "stress/StressConfig.h"
#include "stress/StressService.h"

namespace fs = std::filesystem;
using nlohmann::json;
using namespace std::chrono_literals;
using liveqx::stress::StressConfig;
using liveqx::stress::StressService;

namespace {

constexpr int kStressPortBase = 21900;

int nextPort() {
    static std::atomic<int> n{0};
    static const int base = kStressPortBase + (static_cast<int>(::getpid()) % 600) * 5;
    return base + n.fetch_add(1);
}

fs::path uniqueDir(const std::string& tag) {
    auto base = fs::temp_directory_path() /
                ("stress_rest_" + tag + "_" + std::to_string(::getpid()));
    std::error_code ec;
    fs::remove_all(base, ec);
    fs::create_directories(base);
    return base;
}

void waitListening(int port) {
    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(0, 50'000);
    for (int i = 0; i < 100; ++i) {
        auto r = cli.Get("/healthz");
        if (r && r->status == 200) return;
        std::this_thread::sleep_for(20ms);
    }
}

struct Fixture {
    ChannelManager  manager;
    fs::path        report_dir;
    StressConfig    cfg;
    StressService   service;
    ControlApi      api;
    int             port;

    Fixture(int p, const fs::path& dir)
        : manager(nullptr),
          report_dir(dir),
          cfg(makeCfg(dir)),
          service(manager, cfg),
          api(p, manager,
              /*metrics=*/nullptr, LivezOptions{},
              /*gateways=*/nullptr,
              /*auth=*/nullptr, /*ldap=*/nullptr, /*smtp=*/nullptr,
              /*rbac=*/nullptr, /*events=*/nullptr,
              /*preview=*/nullptr,
              /*stress=*/&service),
          port(p) {
        api.start();
        waitListening(port);
    }

    ~Fixture() {
        api.stop();
        std::error_code ec;
        fs::remove_all(report_dir, ec);
    }

    static StressConfig makeCfg(const fs::path& dir) {
        StressConfig c;
        c.enabled            = false;
        c.schedule_cron      = "";       // disable scheduler
        c.duration_sec       = 1;
        c.channels           = 0;
        c.outputs_per_channel= 0;
        c.report_dir         = dir.string();
        return c;
    }
};

struct NullFixture {
    ChannelManager manager;
    ControlApi     api;
    int            port;

    explicit NullFixture(int p) : manager(nullptr), api(p, manager), port(p) {
        api.start();
        waitListening(port);
    }
    ~NullFixture() { api.stop(); }
};

}  // namespace

TEST(StressRest, GetConfigReturnsDefaults) {
    auto dir = uniqueDir("get_config");
    Fixture f(nextPort(), dir);
    httplib::Client cli("127.0.0.1", f.port);

    auto r = cli.Get("/api/stress/config");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 200);
    auto j = json::parse(r->body);
    EXPECT_TRUE(j.contains("enabled"));
    EXPECT_FALSE(j["enabled"].get<bool>());
    EXPECT_EQ(j["report_dir"].get<std::string>(), dir.string());
}

TEST(StressRest, PutConfigSwapsAndPersists) {
    auto dir = uniqueDir("put_config");
    Fixture f(nextPort(), dir);
    httplib::Client cli("127.0.0.1", f.port);

    json patch = f.service.configJson();
    patch["enabled"]      = true;
    patch["duration_sec"] = 5;
    auto r = cli.Put("/api/stress/config", patch.dump(), "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 200);
    auto j = json::parse(r->body);
    EXPECT_TRUE(j["enabled"].get<bool>());
    EXPECT_EQ(j["duration_sec"].get<int>(), 5);

    // Re-fetch to confirm.
    auto r2 = cli.Get("/api/stress/config");
    ASSERT_TRUE(r2);
    auto j2 = json::parse(r2->body);
    EXPECT_TRUE(j2["enabled"].get<bool>());
    EXPECT_EQ(j2["duration_sec"].get<int>(), 5);
}

TEST(StressRest, PutConfigInvalidReturns400) {
    auto dir = uniqueDir("put_invalid");
    Fixture f(nextPort(), dir);
    httplib::Client cli("127.0.0.1", f.port);

    json patch = f.service.configJson();
    patch["duration_sec"] = -1;   // invalid
    auto r = cli.Put("/api/stress/config", patch.dump(), "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400);
    auto j = json::parse(r->body);
    EXPECT_EQ(j["error"].get<std::string>(), "invalid_config");
}

TEST(StressRest, StartRefusesWhenDisabled) {
    auto dir = uniqueDir("start_disabled");
    Fixture f(nextPort(), dir);
    httplib::Client cli("127.0.0.1", f.port);

    // Default cfg has enabled=false.
    auto r = cli.Post("/api/stress/start", "", "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 409);
    auto j = json::parse(r->body);
    EXPECT_EQ(j["error"].get<std::string>(), "cannot_start");
    EXPECT_NE(j["detail"].get<std::string>().find("disabled"), std::string::npos);
}

TEST(StressRest, StatusShapeIsStable) {
    auto dir = uniqueDir("status_shape");
    Fixture f(nextPort(), dir);
    httplib::Client cli("127.0.0.1", f.port);

    auto r = cli.Get("/api/stress/status");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 200);
    auto j = json::parse(r->body);
    EXPECT_TRUE(j.contains("enabled"));
    EXPECT_TRUE(j.contains("schedule_cron"));
    EXPECT_TRUE(j.contains("scheduler_armed"));
    EXPECT_TRUE(j.contains("runner"));
    EXPECT_TRUE(j["runner"].is_object());
    EXPECT_FALSE(j["scheduler_armed"].get<bool>());   // no cron set
}

TEST(StressRest, ReportsListEmptyByDefault) {
    auto dir = uniqueDir("reports_empty");
    Fixture f(nextPort(), dir);
    httplib::Client cli("127.0.0.1", f.port);

    auto r = cli.Get("/api/stress/reports");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 200);
    auto j = json::parse(r->body);
    ASSERT_TRUE(j.is_array());
    EXPECT_EQ(j.size(), 0u);
}

TEST(StressRest, ReadUnknownReportReturns404) {
    auto dir = uniqueDir("report_404");
    Fixture f(nextPort(), dir);
    httplib::Client cli("127.0.0.1", f.port);

    auto r = cli.Get("/api/stress/reports/does-not-exist");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 404);
}

TEST(StressRest, NoServiceReturns503) {
    NullFixture f(nextPort());
    httplib::Client cli("127.0.0.1", f.port);

    for (const auto& path : {
        std::string{"/api/stress/config"},
        std::string{"/api/stress/status"},
        std::string{"/api/stress/reports"},
    }) {
        auto r = cli.Get(path.c_str());
        ASSERT_TRUE(r) << path;
        EXPECT_EQ(r->status, 503) << path;
    }
}
