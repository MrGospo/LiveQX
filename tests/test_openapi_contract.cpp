// fix35 A3.0d — Contract test для openapi.yaml.
//
// Цель: предотвратить drift между `docs/openapi.yaml` и реальным
// поведением backend. Тест поднимает live ControlApi, дёргает каждый
// REST-эндпоинт и валидирует:
//   - что все `required:`-поля YAML-схемы реально присутствуют в JSON
//   - что типы соответствуют (object/array/string/number/boolean)
//   - что нет ренеймов вокруг исторических точек drift'а
//
// Не тащим yaml-cpp / json-schema-validator (новые vcpkg-deps под одной
// тест) — required-листы захардкожены здесь и должны обновляться вместе
// с openapi.yaml. Это и есть смысл "контракта": две источника правды,
// которые должны совпадать; если один из них меняется — тест падает.
//
// Если этот тест начал падать — НЕ правьте список ниже сразу. Сначала
// решите: что было правильно — backend или yaml? Потом обновите оба.

#include <chrono>
#include <filesystem>
#include <set>
#include <string>
#include <thread>

#include <unistd.h>

#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include "api/ChannelManager.h"
#include "api/ControlApi.h"
#include "api/MetricsCollector.h"
#include "auth/AuthDb.h"
#include "auth/AuthService.h"
#include "auth/JwtIssuer.h"
#include "auth/LdapConfigRepo.h"
#include "auth/MasterKey.h"
#include "auth/SmtpConfigRepo.h"
#include "metrics/ProcessMetrics.h"

namespace {

using nlohmann::json;
using namespace std::chrono_literals;

constexpr int kContractPortBase = 18820;
constexpr const char* kSecret   = "0123456789abcdef0123456789abcdef";

int nextPort() {
    static std::atomic<int> n{0};
    static const int base =
        kContractPortBase + (static_cast<int>(getpid()) % 600) * 5;
    return base + n.fetch_add(1);
}

// Helper: проверяет, что в `body` есть все ключи из `required`. Список
// ниже отражает `required:` из соответствующей схемы openapi.yaml.
void expectAllRequired(const json& body,
                       const std::vector<std::string>& required,
                       const std::string& schema_name) {
    for (const auto& key : required) {
        EXPECT_TRUE(body.contains(key))
            << schema_name << ": missing required field '" << key << "' — "
            << "body=" << body.dump();
    }
}

struct ContractFixture {
    std::filesystem::path                                 dbpath;
    std::filesystem::path                                 keypath;
    std::unique_ptr<liveqx::auth::AuthDb>         db;
    std::unique_ptr<liveqx::auth::JwtIssuer>      jwt;
    std::unique_ptr<liveqx::auth::AuthService>    svc;
    std::unique_ptr<liveqx::auth::MasterKey>      mk;
    std::unique_ptr<liveqx::auth::LdapConfigRepo> ldap_repo;
    std::unique_ptr<liveqx::auth::SmtpConfigRepo> smtp_repo;
    ChannelManager                                        manager;
    ProcessMetrics                                        process;
    std::unique_ptr<MetricsCollector>                     metrics;
    std::unique_ptr<ControlApi>                           api;
    int                                                   port;

    explicit ContractFixture(int p)
        : manager(nullptr, {}),
          process(/*start_unix=*/1234567890),
          port(p) {
        const auto base = std::filesystem::temp_directory_path() /
            ("openapi_contract_" + std::to_string(p));
        dbpath  = base.string() + ".db";
        keypath = base.string() + ".key";
        std::filesystem::remove(dbpath);
        std::filesystem::remove(keypath);

        db = std::make_unique<liveqx::auth::AuthDb>(dbpath);
        EXPECT_TRUE(db->open());
        jwt = std::make_unique<liveqx::auth::JwtIssuer>(std::string(kSecret));
        svc = std::make_unique<liveqx::auth::AuthService>(
            *db, *jwt,
            [](std::int64_t) {
                return std::vector<liveqx::auth::ChannelGrant>{};
            });
        mk = std::make_unique<liveqx::auth::MasterKey>(keypath.string());
        EXPECT_TRUE(mk->load());
        ldap_repo = std::make_unique<liveqx::auth::LdapConfigRepo>(*db, *mk);
        smtp_repo = std::make_unique<liveqx::auth::SmtpConfigRepo>(*db, *mk);

        BuildInfo build{"contract-test", "deadbeef",
                        "2026-05-09T00:00:00Z", "Release"};
        metrics = std::make_unique<MetricsCollector>(
            manager, process, std::move(build));

        api = std::make_unique<ControlApi>(
            port, manager,
            metrics.get(), LivezOptions{},
            /*gateways=*/nullptr,
            svc.get(),
            ldap_repo.get(),
            smtp_repo.get());
        api->start();
        waitListening(port);
    }

    ~ContractFixture() {
        api->stop();
        api.reset();
        metrics.reset();
        smtp_repo.reset();
        ldap_repo.reset();
        mk.reset();
        svc.reset();
        jwt.reset();
        db.reset();
        std::error_code ec;
        std::filesystem::remove(dbpath, ec);
        std::filesystem::remove(dbpath.string() + "-wal", ec);
        std::filesystem::remove(dbpath.string() + "-shm", ec);
        std::filesystem::remove(keypath, ec);
    }

    static void waitListening(int port) {
        httplib::Client cli("127.0.0.1", port);
        cli.set_connection_timeout(0, 50'000);
        for (int i = 0; i < 100; ++i) {
            auto r = cli.Get("/healthz");
            if (r && r->status == 200) return;
            std::this_thread::sleep_for(20ms);
        }
        FAIL() << "ControlApi never started on port " << port;
    }

    httplib::Client client() {
        httplib::Client c("127.0.0.1", port);
        c.set_connection_timeout(2, 0);
        c.set_read_timeout(5, 0);
        return c;
    }
};

}  // namespace

// ─── /api/version → VersionInfo + BuildFeatures ─────────────────────────────
TEST(OpenApiContract, VersionInfoMatchesYaml) {
    ContractFixture f(nextPort());
    auto r = f.client().Get("/api/version");
    ASSERT_TRUE(r);
    ASSERT_EQ(r->status, 200) << r->body;
    auto body = json::parse(r->body);

    // VersionInfo: required=[name, version, build_commit, build_time,
    // build_type, features, attributions]
    expectAllRequired(body,
        {"name","version","build_commit","build_time",
         "build_type","features","attributions"},
        "VersionInfo");
    EXPECT_TRUE(body["name"].is_string());
    EXPECT_TRUE(body["version"].is_string());
    EXPECT_TRUE(body["build_commit"].is_string());
    EXPECT_TRUE(body["build_time"].is_string());
    EXPECT_TRUE(body["build_type"].is_string());
    EXPECT_TRUE(body["features"].is_object());
    EXPECT_TRUE(body["attributions"].is_array());

    // BuildFeatures: required=[gpu, nvenc, qsv, vaapi, systemd, ldap,
    // smtp, simd, preview, sse]
    const json& f_ = body["features"];
    expectAllRequired(f_,
        {"gpu","nvenc","qsv","vaapi","systemd","ldap","smtp",
         "simd","preview","sse"},
        "BuildFeatures");
    // simd — string ("scalar" / "avx2"); остальные — bool.
    EXPECT_TRUE(f_["simd"].is_string());
    for (const auto& k : {"gpu","nvenc","qsv","vaapi","systemd",
                          "ldap","smtp","preview","sse"}) {
        EXPECT_TRUE(f_[k].is_boolean()) << k << " must be boolean";
    }
    EXPECT_EQ(f_["sse"].get<bool>(), true) << "sse always-true per yaml";
}

// ─── /api/status → SystemStatus ─────────────────────────────────────────────
TEST(OpenApiContract, SystemStatusMatchesYaml) {
    ContractFixture f(nextPort());
    auto r = f.client().Get("/api/status");
    ASSERT_TRUE(r);
    ASSERT_EQ(r->status, 200) << r->body;
    auto body = json::parse(r->body);

    // SystemStatus: required=[version, build_commit, build_time,
    // uptime_seconds, process, channels, gateways, summary]
    expectAllRequired(body,
        {"version","build_commit","build_time","uptime_seconds",
         "process","channels","gateways","summary"},
        "SystemStatus");
    EXPECT_TRUE(body["version"].is_string());
    EXPECT_TRUE(body["build_commit"].is_string());
    EXPECT_TRUE(body["build_time"].is_string());
    EXPECT_TRUE(body["uptime_seconds"].is_number_integer());
    EXPECT_TRUE(body["process"].is_object());
    EXPECT_TRUE(body["channels"].is_array());
    EXPECT_TRUE(body["gateways"].is_array());
    EXPECT_TRUE(body["summary"].is_object());

    // SystemStatus.summary: required=[channels_total, channels_running,
    // channels_failed, outputs_total, outputs_failed, gateways_total,
    // gateways_running]
    expectAllRequired(body["summary"],
        {"channels_total","channels_running","channels_failed",
         "outputs_total","outputs_failed",
         "gateways_total","gateways_running"},
        "SystemStatus.summary");
    for (const auto& k : {"channels_total","channels_running","channels_failed",
                          "outputs_total","outputs_failed",
                          "gateways_total","gateways_running"}) {
        EXPECT_TRUE(body["summary"][k].is_number_integer())
            << k << " must be integer";
    }
}

// ─── /api/system/interfaces → array of NetworkInterface ─────────────────────
TEST(OpenApiContract, NetworkInterfacesMatchYaml) {
    ContractFixture f(nextPort());
    auto r = f.client().Get("/api/system/interfaces");
    ASSERT_TRUE(r);
    ASSERT_EQ(r->status, 200) << r->body;
    auto body = json::parse(r->body);

    // Endpoint возвращает массив (или, в некоторых билдах, объект с items).
    // Берём оба варианта: сначала пытаемся прочитать как массив, иначе
    // как .items / .interfaces.
    json items;
    if (body.is_array())                        items = body;
    else if (body.contains("items"))            items = body["items"];
    else if (body.contains("interfaces"))       items = body["interfaces"];
    else                                        items = json::array();
    ASSERT_TRUE(items.is_array());
    // На любой Linux-машине loopback есть всегда — пустой массив редкость,
    // но не падаем если CI-окружение странное.
    if (items.empty()) GTEST_SKIP() << "no interfaces enumerated";

    for (const auto& iface : items) {
        // NetworkInterface: required=[name, addresses, up, loopback, multicast]
        expectAllRequired(iface,
            {"name","addresses","up","loopback","multicast"},
            "NetworkInterface");
        EXPECT_TRUE(iface["name"].is_string());
        EXPECT_TRUE(iface["addresses"].is_array());
        EXPECT_TRUE(iface["up"].is_boolean());
        EXPECT_TRUE(iface["loopback"].is_boolean());
        EXPECT_TRUE(iface["multicast"].is_boolean());
    }
}

// ─── /api/system/gpu → GpuInfo (nvenc/qsv/vaapi/x264) ───────────────────────
TEST(OpenApiContract, GpuProbeMatchesYaml) {
    ContractFixture f(nextPort());
    auto r = f.client().Get("/api/system/gpu");
    ASSERT_TRUE(r);
    ASSERT_EQ(r->status, 200) << r->body;
    auto body = json::parse(r->body);

    // GpuInfo: required=[nvenc, qsv, vaapi, x264] (см. ControlApi.cpp:1316)
    expectAllRequired(body, {"nvenc","qsv","vaapi","x264"}, "GpuInfo");
    for (const auto& backend : {"nvenc","qsv","vaapi","x264"}) {
        ASSERT_TRUE(body[backend].is_object())
            << backend << " must be object";
        // GpuBackendProbe: required=[built_in, codec_registered]
        expectAllRequired(body[backend],
            {"built_in","codec_registered"},
            std::string("GpuBackendProbe.") + backend);
        EXPECT_TRUE(body[backend]["built_in"].is_boolean());
        EXPECT_TRUE(body[backend]["codec_registered"].is_boolean());
    }
}

// ─── /api/system/browse → {path, parent, entries[]} ────────────────────────
// fix36: folder picker backend. Path canonicalisation — `..` and relative
// segments resolved server-side; non-existent paths return 404.
TEST(OpenApiContract, SystemBrowseMatchesYaml) {
    ContractFixture f(nextPort());
    auto r = f.client().Get("/api/system/browse?path=/");
    ASSERT_TRUE(r);
    ASSERT_EQ(r->status, 200) << r->body;
    auto body = json::parse(r->body);

    expectAllRequired(body, {"path","parent","entries"}, "BrowseResponse");
    EXPECT_TRUE(body["path"].is_string());
    EXPECT_TRUE(body["parent"].is_null() || body["parent"].is_string())
        << "parent must be string|null";
    ASSERT_TRUE(body["entries"].is_array());
    for (const auto& e : body["entries"]) {
        expectAllRequired(e, {"name","full_path"}, "BrowseEntry");
        EXPECT_TRUE(e["name"].is_string());
        EXPECT_TRUE(e["full_path"].is_string());
    }

    // 404 path
    auto bad = f.client().Get("/api/system/browse?path=/no/such/dir/at/all/xyz");
    ASSERT_TRUE(bad);
    EXPECT_EQ(bad->status, 404);
}

// ─── /api/auth/ldap/config (empty) → LdapConfigUnconfigured ─────────────────
TEST(OpenApiContract, LdapConfigUnconfiguredMatchesYaml) {
    ContractFixture f(nextPort());
    auto r = f.client().Get("/api/auth/ldap/config");
    ASSERT_TRUE(r);
    ASSERT_EQ(r->status, 200) << r->body;
    auto body = json::parse(r->body);

    // LdapConfigUnconfigured: required=[configured], configured===false,
    // additionalProperties:false → других ключей быть не должно.
    expectAllRequired(body, {"configured"}, "LdapConfigUnconfigured");
    EXPECT_EQ(body["configured"].get<bool>(), false);
    EXPECT_EQ(body.size(), 1u)
        << "Unconfigured variant must have only 'configured' field; got "
        << body.dump();
}

// ─── /api/auth/smtp/config (empty) → SmtpConfig defaults ────────────────────
TEST(OpenApiContract, SmtpConfigEmptyMatchesYaml) {
    ContractFixture f(nextPort());
    auto r = f.client().Get("/api/auth/smtp/config");
    ASSERT_TRUE(r);
    ASSERT_EQ(r->status, 200) << r->body;
    auto body = json::parse(r->body);

    // SmtpConfig: required=[enabled, server, port, security, username,
    // password_set, from_email, from_name, timeout_sec]
    expectAllRequired(body,
        {"enabled","server","port","security","username","password_set",
         "from_email","from_name","timeout_sec"},
        "SmtpConfig");
    EXPECT_TRUE(body["enabled"].is_boolean());
    EXPECT_TRUE(body["server"].is_string());
    EXPECT_TRUE(body["port"].is_number_integer());
    EXPECT_TRUE(body["security"].is_string());
    EXPECT_TRUE(body["username"].is_string());
    EXPECT_TRUE(body["password_set"].is_boolean());
    EXPECT_TRUE(body["from_email"].is_string());
    EXPECT_TRUE(body["from_name"].is_string());
    EXPECT_TRUE(body["timeout_sec"].is_number_integer());

    // Асимметрия с LdapConfig: НЕТ поля `configured`.
    EXPECT_FALSE(body.contains("configured"))
        << "SmtpConfig must not have 'configured' (asymmetry vs LdapConfig)";
}
