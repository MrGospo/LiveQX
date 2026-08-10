// fix16 step 5 — /healthz, /readyz, /livez integration tests.
//
// Spins up ControlApi on an ephemeral high port + httplib::Client to make
// real HTTP calls. Stuck-channel /livez behaviour is exercised by a
// follow-up integration test in step 7; this file covers handler routing,
// readyz gating logic, and the no-channel /livez happy path.

#include <chrono>
#include <filesystem>
#include <thread>

#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include "api/ChannelManager.h"
#include "api/ControlApi.h"
#include "api/MetricsCollector.h"
#include "gateway/GatewayManager.h"
#include "metrics/ProcessMetrics.h"
#include "plugins/PluginManager.h"

namespace {

using nlohmann::json;
using namespace std::chrono_literals;

// Port pool — picked high enough not to clash with anything else but
// distinct per test so parallel ctest runs don't fight.
constexpr int kHealthzPort  = 18091;
constexpr int kReadyzPort   = 18092;
constexpr int kLivezPort    = 18093;
constexpr int kReadyz2Port  = 18094;
constexpr int kVersionPort  = 18095;
constexpr int kStatusPort   = 18096;

// Tiny RAII wrapper. ControlApi::start() spawns a thread and the server
// usually accepts within a few ms; we poll the /healthz endpoint until it
// answers so tests don't race the listener.
struct ApiFixture {
    ChannelManager manager;
    ControlApi     api;

    explicit ApiFixture(int port,
                        LivezOptions livez = {},
                        std::filesystem::path root = {})
        : manager(nullptr, std::move(root)),
          api(port, manager, /*metrics=*/nullptr, livez) {
        api.start();
        waitListening(port);
    }
    ~ApiFixture() { api.stop(); }

    static void waitListening(int port) {
        httplib::Client cli("127.0.0.1", port);
        cli.set_connection_timeout(0, 50'000);  // 50ms
        for (int i = 0; i < 100; ++i) {
            auto r = cli.Get("/healthz");
            if (r && r->status == 200) return;
            std::this_thread::sleep_for(20ms);
        }
        FAIL() << "ControlApi never started listening on port " << port;
    }
};

}  // namespace

TEST(HealthEndpoints, HealthzAlwaysOk) {
    ApiFixture f(kHealthzPort);
    httplib::Client cli("127.0.0.1", kHealthzPort);

    auto r = cli.Get("/healthz");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 200);
    auto body = json::parse(r->body);
    EXPECT_EQ(body["status"], "ok");
}

TEST(HealthEndpoints, ReadyzReportsLoadingThenReady) {
    ApiFixture f(kReadyzPort);
    httplib::Client cli("127.0.0.1", kReadyzPort);

    // Before loadFromRoot(): channels_loading wins.
    {
        auto r = cli.Get("/readyz");
        ASSERT_TRUE(r);
        EXPECT_EQ(r->status, 503);
        auto body = json::parse(r->body);
        EXPECT_EQ(body["status"], "not_ready");
        ASSERT_TRUE(body["reasons"].is_array());
        bool has_channels_loading = false;
        for (const auto& reason : body["reasons"])
            if (reason == "channels_loading") has_channels_loading = true;
        EXPECT_TRUE(has_channels_loading);
    }

    // loadFromRoot() over an empty path is still a valid "loaded" pass —
    // exercising fix16's guarantee that empty-channel-root flips loaded.
    f.manager.loadFromRoot();
    // fix17: state_persistence_loaded_ now defaults to false. Bootstrap is
    // responsible for flipping it after every per-channel state.db has been
    // opened. Here we mimic that flip directly so the readyz happy-path
    // assertion still holds.
    f.manager.setStatePersistenceLoaded(true);

    {
        auto r = cli.Get("/readyz");
        ASSERT_TRUE(r);
        EXPECT_EQ(r->status, 200);
        auto body = json::parse(r->body);
        EXPECT_EQ(body["status"], "ready");
    }
}

TEST(HealthEndpoints, ReadyzGatesOnFutureSubsystems) {
    ApiFixture f(kReadyz2Port);
    httplib::Client cli("127.0.0.1", kReadyz2Port);

    f.manager.loadFromRoot();
    f.manager.setStatePersistenceLoaded(false);

    auto r = cli.Get("/readyz");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 503);
    auto body = json::parse(r->body);
    bool found = false;
    for (const auto& reason : body["reasons"])
        if (reason == "state_persistence_loading") found = true;
    EXPECT_TRUE(found);

    f.manager.setStatePersistenceLoaded(true);
    auto r2 = cli.Get("/readyz");
    ASSERT_TRUE(r2);
    EXPECT_EQ(r2->status, 200);
}

TEST(HealthEndpoints, LivezGreenWithNoChannels) {
    ApiFixture f(kLivezPort, LivezOptions{0.5});
    httplib::Client cli("127.0.0.1", kLivezPort);

    auto r = cli.Get("/livez");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 200);
    auto body = json::parse(r->body);
    EXPECT_EQ(body["status"], "alive");
}

namespace {

// Variant of ApiFixture that wires a MetricsCollector — required for
// /api/version and /api/status, which delegate to it.
struct ApiWithMetricsFixture {
    ChannelManager   manager;
    ProcessMetrics   process;
    MetricsCollector mc;
    ControlApi       api;

    ApiWithMetricsFixture(int port, BuildInfo b)
        : manager(nullptr, {}),
          process(/*start_unix=*/1234567890),
          mc(manager, process, std::move(b)),
          api(port, manager, &mc) {
        api.start();
        ApiFixture::waitListening(port);
    }
    ~ApiWithMetricsFixture() { api.stop(); }
};

}  // namespace

static void expectBaselineCoreAttributions(const nlohmann::json& body);

TEST(VersionEndpoint, ReturnsBuildIdentityAndFeatureFlags) {
    BuildInfo build{"9.9.9", "deadbeef", "2026-05-06T10:00:00Z", "Release"};
    ApiWithMetricsFixture f(kVersionPort, build);
    httplib::Client cli("127.0.0.1", kVersionPort);

    auto r = cli.Get("/api/version");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 200);
    auto body = json::parse(r->body);
    EXPECT_EQ(body["name"],         "liveqx");
    EXPECT_EQ(body["version"],      "9.9.9");
    EXPECT_EQ(body["build_commit"], "deadbeef");
    EXPECT_EQ(body["build_time"],   "2026-05-06T10:00:00Z");
    EXPECT_EQ(body["build_type"],   "Release");
    ASSERT_TRUE(body["features"].is_object());
    // All feature flags should default to false on unconfigured builds.
    // simd is a string; it's "scalar" until fix28 swaps in a real value.
#ifdef LIVEQX_FEATURE_GPU
    EXPECT_EQ(body["features"]["gpu"],     true);
#else
    EXPECT_EQ(body["features"]["gpu"],     false);
#endif
#ifdef LIVEQX_ENABLE_SYSTEMD
    EXPECT_EQ(body["features"]["systemd"], true);
#else
    EXPECT_EQ(body["features"]["systemd"], false);
#endif
    EXPECT_EQ(body["features"]["ldap"],    false);
    EXPECT_EQ(body["features"]["smtp"],    false);
    EXPECT_TRUE(body["features"]["simd"].is_string());
    // attributions всегда содержит baseline core-зависимости
    // (FFmpeg / libsrt / OpenSSL); плагины добавляются сверху.
    ASSERT_TRUE(body["attributions"].is_array());
    expectBaselineCoreAttributions(body);
}

// Helper: assert the three baseline core-library attribution entries are
// present in the rendered version body. We don't pin version numbers —
// they come from av_version_info / srt_getversion / OPENSSL_VERSION_TEXT
// at runtime and shift with vcpkg/build_ffmpeg.sh updates.
static void expectBaselineCoreAttributions(const nlohmann::json& body) {
    ASSERT_TRUE(body["attributions"].is_array());
    bool has_ffmpeg = false, has_libsrt = false, has_openssl = false;
    for (const auto& e : body["attributions"]) {
        const auto s = e.get<std::string>();
        if (s.rfind("FFmpeg ",  0) == 0) has_ffmpeg  = true;
        if (s.rfind("libsrt ",  0) == 0) has_libsrt  = true;
        if (s.rfind("OpenSSL ", 0) == 0) has_openssl = true;
    }
    EXPECT_TRUE(has_ffmpeg)  << body["attributions"].dump();
    EXPECT_TRUE(has_libsrt)  << body["attributions"].dump();
    EXPECT_TRUE(has_openssl) << body["attributions"].dump();
}

TEST(MetricsCollectorRender, BaselineAttributionsPresentWhenNoPluginManager) {
    ChannelManager   mgr(nullptr, {});
    ProcessMetrics   pm(/*start_unix=*/0);
    BuildInfo        b{"0.19.0", "deadbeef", "2026-05-06T10:00:00Z", "Release"};
    MetricsCollector mc(mgr, pm, b);

    expectBaselineCoreAttributions(mc.renderVersion());
}

TEST(MetricsCollectorRender, BaselineAttributionsPresentWhenPluginManagerEmpty) {
    // Empty PluginManager (no plugins installed). Baseline core attributions
    // are still emitted; plugin attributions append on top when present —
    // populated case covered by c11 plugin manager tests.
    auto root = std::filesystem::temp_directory_path()
                / ("liveqx_attr_" +
                   std::to_string(::getpid()) + "_" +
                   std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);

    liveqx::plugins::PluginManager pm_mgr(root, /*log=*/{});
    ChannelManager   mgr(nullptr, {});
    ProcessMetrics   pm(/*start_unix=*/0);
    BuildInfo        b{"0.19.0", "deadbeef", "2026-05-06T10:00:00Z", "Release"};
    MetricsCollector mc(mgr, pm, b, /*gateways=*/nullptr,
                        /*stress=*/nullptr, &pm_mgr);

    expectBaselineCoreAttributions(mc.renderVersion());

    std::filesystem::remove_all(root);
}

TEST(MetricsCollectorRender, EmptyManagerEmitsBuildAndSummary) {
    ChannelManager   mgr(nullptr, {});
    ProcessMetrics   pm(/*start_unix=*/1700000000);
    BuildInfo        b{"7.7.7", "feedface", "2026-05-06T10:00:00Z", "Release"};
    MetricsCollector mc(mgr, pm, b);

    const std::string out = mc.renderPrometheus();
    // Build identity always present (the gauge=1 const-labelled line).
    EXPECT_NE(out.find("liveqx_build_info{"), std::string::npos);
    EXPECT_NE(out.find("version=\"7.7.7\""),         std::string::npos);
    EXPECT_NE(out.find("commit=\"feedface\""),       std::string::npos);
    EXPECT_NE(out.find("build_type=\"Release\""),    std::string::npos);
    // Summary present even with 0 channels.
    EXPECT_NE(out.find("liveqx_channels_total 0"),   std::string::npos);
    EXPECT_NE(out.find("liveqx_channels_running 0"), std::string::npos);
    EXPECT_NE(out.find("liveqx_outputs_total 0"),    std::string::npos);
    // Every # TYPE line has a matching # HELP. Count them so a broken
    // refactor that drops one doesn't sneak through.
    size_t help_count = 0, type_count = 0, p = 0;
    while ((p = out.find("# HELP ", p)) != std::string::npos) { ++help_count; ++p; }
    p = 0;
    while ((p = out.find("# TYPE ", p)) != std::string::npos) { ++type_count; ++p; }
    EXPECT_EQ(help_count, type_count);
    EXPECT_GT(help_count, 10u);  // build + process + per-family headers
}

TEST(MetricsCollectorRender, BuildInfoEscapesQuotesAndBackslashesInLabels) {
    ChannelManager   mgr(nullptr, {});
    ProcessMetrics   pm(/*start_unix=*/0);
    // Pathological build identity — \, ", and \n must all escape.
    BuildInfo        b{"v1\\test", "with\"quote", "line1\nline2", "Debug"};
    MetricsCollector mc(mgr, pm, b);

    const std::string out = mc.renderPrometheus();
    EXPECT_NE(out.find("version=\"v1\\\\test\""),        std::string::npos);
    EXPECT_NE(out.find("commit=\"with\\\"quote\""),      std::string::npos);
    EXPECT_NE(out.find("build_time=\"line1\\nline2\""),  std::string::npos);
}

TEST(MetricsCollectorRender, GatewayMetricsAppearWhenManagerProvided) {
    auto root = std::filesystem::temp_directory_path()
                / ("liveqx_metrics_gw_" +
                   std::to_string(::getpid()) + "_" +
                   std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);

    liveqx::gateway::GatewayManager gw(root);
    int gid = -1;
    nlohmann::json cfg{
        {"name",   "metrics-gw"},
        {"input",  {{"address","127.0.0.1"},{"port",49600}}},
        {"outputs", nlohmann::json::array({
            nlohmann::json{{"address","127.0.0.1"},{"port",49601}},
        })},
    };
    ASSERT_EQ(gw.create(cfg, &gid),
              liveqx::gateway::GatewayManager::Result::Ok);

    ChannelManager   mgr(nullptr, {});
    ProcessMetrics   pm(/*start_unix=*/0);
    BuildInfo        b{"0.18.0", "deadbeef", "2026-05-06T10:00:00Z", "Release"};
    MetricsCollector mc(mgr, pm, b, &gw);

    const std::string out = mc.renderPrometheus();
    EXPECT_NE(out.find("liveqx_gateways_total 1"),        std::string::npos);
    EXPECT_NE(out.find("liveqx_gateways_running 0"),      std::string::npos);
    EXPECT_NE(out.find("liveqx_gateway_running{"),        std::string::npos);
    EXPECT_NE(out.find("gateway_name=\"metrics-gw\""),            std::string::npos);
    EXPECT_NE(out.find("liveqx_gateway_packets_in_total{"), std::string::npos);
    EXPECT_NE(out.find("liveqx_gateway_outputs{"),        std::string::npos);

    // /api/status mirror — gateways[] populated, summary.gateways_total set.
    auto status = mc.renderStatus();
    ASSERT_TRUE(status["gateways"].is_array());
    EXPECT_EQ(status["gateways"].size(), 1u);
    EXPECT_EQ(status["summary"]["gateways_total"],   1);
    EXPECT_EQ(status["summary"]["gateways_running"], 0);

    std::filesystem::remove_all(root);
}

TEST(MetricsCollectorRender, GatewayMetricsOmittedWhenManagerNull) {
    ChannelManager   mgr(nullptr, {});
    ProcessMetrics   pm(/*start_unix=*/0);
    BuildInfo        b{"0.18.0", "deadbeef", "2026-05-06T10:00:00Z", "Release"};
    MetricsCollector mc(mgr, pm, b, /*gateways=*/nullptr);

    const std::string out = mc.renderPrometheus();
    EXPECT_EQ(out.find("liveqx_gateways_total"), std::string::npos);
    EXPECT_EQ(out.find("liveqx_gateway_running"), std::string::npos);

    // /api/status mirror — gateways[] is an empty array; no summary entries.
    auto status = mc.renderStatus();
    ASSERT_TRUE(status["gateways"].is_array());
    EXPECT_EQ(status["gateways"].size(), 0u);
    EXPECT_EQ(status["summary"]["gateways_total"], 0);
}

TEST(StatusEndpoint, EmptyManagerProducesShellSummary) {
    BuildInfo build{"0.16.0", "abc1234", "2026-05-06T10:00:00Z", "Release"};
    ApiWithMetricsFixture f(kStatusPort, build);
    httplib::Client cli("127.0.0.1", kStatusPort);

    auto r = cli.Get("/api/status");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 200);
    auto body = json::parse(r->body);
    EXPECT_EQ(body["version"],      "0.16.0");
    EXPECT_EQ(body["build_commit"], "abc1234");
    EXPECT_TRUE(body["channels"].is_array());
    EXPECT_EQ(body["channels"].size(), 0u);
    ASSERT_TRUE(body["summary"].is_object());
    EXPECT_EQ(body["summary"]["channels_total"],   0);
    EXPECT_EQ(body["summary"]["channels_running"], 0);
    EXPECT_EQ(body["summary"]["outputs_total"],    0);
}
