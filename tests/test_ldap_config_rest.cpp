// fix22 commit 17/24 — REST integration test для LDAP config endpoints.
//
// GET  /api/auth/ldap/config
// PUT  /api/auth/ldap/config

#include <chrono>
#include <filesystem>
#include <thread>

#include <unistd.h>

#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include "api/ChannelManager.h"
#include "api/ControlApi.h"
#include "auth/AuthDb.h"
#include "auth/AuthService.h"
#include "auth/JwtIssuer.h"
#include "auth/LdapConfigRepo.h"
#include "auth/MasterKey.h"

namespace {

using nlohmann::json;
using namespace std::chrono_literals;

constexpr int kLdapCfgPortBase = 18960;
constexpr const char* kSecret = "0123456789abcdef0123456789abcdef";

int nextPort() {
    static std::atomic<int> n{0};
    static const int base = kLdapCfgPortBase + (static_cast<int>(getpid()) % 800) * 5;
    return base + n.fetch_add(1);
}

struct LdapCfgRestFixture {
    std::filesystem::path                                  dbpath;
    std::filesystem::path                                  keypath;
    std::unique_ptr<liveqx::auth::AuthDb>          db;
    std::unique_ptr<liveqx::auth::JwtIssuer>       jwt;
    std::unique_ptr<liveqx::auth::AuthService>     svc;
    std::unique_ptr<liveqx::auth::MasterKey>       mk;
    std::unique_ptr<liveqx::auth::LdapConfigRepo>  repo;
    ChannelManager                                         manager;
    std::unique_ptr<ControlApi>                            api;
    int                                                    port;

    explicit LdapCfgRestFixture(int p) : manager(nullptr), port(p) {
        const auto base = std::filesystem::temp_directory_path() /
            ("ldap_cfg_rest_" + std::to_string(p));
        dbpath  = base.string() + ".db";
        keypath = base.string() + ".key";
        std::filesystem::remove(dbpath);
        std::filesystem::remove(keypath);

        db  = std::make_unique<liveqx::auth::AuthDb>(dbpath);
        EXPECT_TRUE(db->open());
        jwt = std::make_unique<liveqx::auth::JwtIssuer>(std::string(kSecret));
        svc = std::make_unique<liveqx::auth::AuthService>(
            *db, *jwt,
            [](std::int64_t) { return std::vector<liveqx::auth::ChannelGrant>{}; });
        mk  = std::make_unique<liveqx::auth::MasterKey>(keypath.string());
        EXPECT_TRUE(mk->load());
        repo = std::make_unique<liveqx::auth::LdapConfigRepo>(*db, *mk);

        api = std::make_unique<ControlApi>(
            port, manager,
            /*metrics=*/nullptr, LivezOptions{},
            /*gateways=*/nullptr,
            svc.get(),
            repo.get());
        api->start();
        waitListening(port);
    }

    ~LdapCfgRestFixture() {
        api->stop();
        api.reset();
        repo.reset();
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

TEST(LdapConfigRest, GetEmptyReturnsConfiguredFalse) {
    LdapCfgRestFixture f(nextPort());
    auto cli = f.client();
    auto r = cli.Get("/api/auth/ldap/config");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 200);
    auto body = json::parse(r->body);
    EXPECT_EQ(body["configured"], false);
}

TEST(LdapConfigRest, PutSavesDisabledConfigWithoutValidation) {
    LdapCfgRestFixture f(nextPort());
    auto cli = f.client();
    json req = {
        {"enabled", false},
        {"server",  "dc.example.com"},
    };
    auto r = cli.Put("/api/auth/ldap/config", req.dump(), "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 200) << r->body;
    auto body = json::parse(r->body);
    EXPECT_EQ(body["configured"], true);
    EXPECT_EQ(body["enabled"],    false);
    EXPECT_EQ(body["server"],     "dc.example.com");
    EXPECT_EQ(body["bind_password"], "");
    EXPECT_EQ(body["bind_password_set"], false);
}

TEST(LdapConfigRest, PutEnabledRequiresValidatedFields) {
    LdapCfgRestFixture f(nextPort());
    auto cli = f.client();
    json req = {
        {"enabled", true},
        {"server",  "dc.example.com"},
        // отсутствует base_dn — validate должен ругнуться.
    };
    auto r = cli.Put("/api/auth/ldap/config", req.dump(), "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400);
    auto body = json::parse(r->body);
    EXPECT_EQ(body["error"], "invalid_config");
}

TEST(LdapConfigRest, PutFullConfigRoundTripsWithMaskedPassword) {
    LdapCfgRestFixture f(nextPort());
    auto cli = f.client();

    json req = {
        {"enabled", true},
        {"server",  "ldap.corp.example.com"},
        {"tls_mode","starttls"},
        {"base_dn", "ou=people,dc=corp,dc=example,dc=com"},
        {"bind_dn", "cn=svc,dc=corp,dc=example,dc=com"},
        {"bind_password", "service-pass-123"},
        {"user_filter", "(uid=%s)"},
        {"group_attribute", "memberOf"},
        {"group_role_map", {
            {"CN=streamops,OU=groups,DC=corp,DC=example,DC=com", "operator"},
            {"CN=admins,OU=groups,DC=corp,DC=example,DC=com",    "admin"},
        }},
    };
    auto r = cli.Put("/api/auth/ldap/config", req.dump(), "application/json");
    ASSERT_TRUE(r);
    ASSERT_EQ(r->status, 200) << r->body;
    auto body = json::parse(r->body);
    // Password всегда замаскирован в ответе.
    EXPECT_EQ(body["bind_password"], "***");
    EXPECT_EQ(body["bind_password_set"], true);
    EXPECT_EQ(body["server"], "ldap.corp.example.com");
    EXPECT_EQ(body["tls_mode"], "starttls");
    EXPECT_EQ(body["group_role_map"]["CN=admins,OU=groups,DC=corp,DC=example,DC=com"], "admin");

    // GET после PUT даёт то же самое (маска сохраняется).
    auto g = cli.Get("/api/auth/ldap/config");
    ASSERT_TRUE(g);
    auto gb = json::parse(g->body);
    EXPECT_EQ(gb["configured"],   true);
    EXPECT_EQ(gb["bind_password"],"***");
    EXPECT_EQ(gb["bind_dn"], "cn=svc,dc=corp,dc=example,dc=com");

    // На уровне Repo проверяем, что plaintext действительно расшифровывается.
    auto cfg = f.repo->load();
    ASSERT_TRUE(cfg.has_value());
    EXPECT_EQ(cfg->bind_password, "service-pass-123");
}

TEST(LdapConfigRest, PutOmittingPasswordPreservesPrevious) {
    LdapCfgRestFixture f(nextPort());
    auto cli = f.client();
    {
        json init = {
            {"enabled", true}, {"server","x"}, {"tls_mode","plain"},
            {"base_dn","dc=x"}, {"bind_dn","cn=svc,dc=x"},
            {"bind_password","initial-pw"},
            {"user_filter","(uid=%s)"},
        };
        auto r = cli.Put("/api/auth/ldap/config", init.dump(), "application/json");
        ASSERT_EQ(r->status, 200) << r->body;
    }
    {
        // Body без bind_password — пароль не должен меняться.
        json patch = {
            {"server", "x.changed"},
        };
        auto r = cli.Put("/api/auth/ldap/config", patch.dump(), "application/json");
        ASSERT_EQ(r->status, 200);
    }
    auto cfg = f.repo->load();
    ASSERT_TRUE(cfg.has_value());
    EXPECT_EQ(cfg->bind_password, "initial-pw");
    EXPECT_EQ(cfg->server, "x.changed");
}

TEST(LdapConfigRest, BindPasswordUnsetClearsIt) {
    LdapCfgRestFixture f(nextPort());
    auto cli = f.client();
    {
        json init = {
            {"enabled", true}, {"server","x"}, {"tls_mode","plain"},
            {"base_dn","dc=x"}, {"bind_password","to-be-cleared"},
            {"user_filter","(uid=%s)"},
        };
        cli.Put("/api/auth/ldap/config", init.dump(), "application/json");
    }
    json patch = {
        {"bind_password_unset", true},
    };
    auto r = cli.Put("/api/auth/ldap/config", patch.dump(), "application/json");
    ASSERT_TRUE(r);
    ASSERT_EQ(r->status, 200) << r->body;
    auto body = json::parse(r->body);
    EXPECT_EQ(body["bind_password_set"], false);
    EXPECT_EQ(body["bind_password"],     "");

    auto cfg = f.repo->load();
    ASSERT_TRUE(cfg.has_value());
    EXPECT_TRUE(cfg->bind_password.empty());
}

TEST(LdapConfigRest, PutInvalidTlsModeReturns400) {
    LdapCfgRestFixture f(nextPort());
    auto cli = f.client();
    json req = {
        {"enabled", false},
        {"tls_mode", "tls12-only"},  // не из {plain, starttls, ldaps}
    };
    auto r = cli.Put("/api/auth/ldap/config", req.dump(), "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400);
    auto body = json::parse(r->body);
    EXPECT_EQ(body["error"], "invalid_tls_mode");
}

TEST(LdapConfigRest, PutInvalidRoleInMapReturns400) {
    LdapCfgRestFixture f(nextPort());
    auto cli = f.client();
    json req = {
        {"enabled", false},
        {"group_role_map", { {"CN=foo,DC=x", "supervisor"} }},
    };
    auto r = cli.Put("/api/auth/ldap/config", req.dump(), "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400);
    auto body = json::parse(r->body);
    EXPECT_EQ(body["error"], "invalid_role_in_map");
}

TEST(LdapConfigRest, PutNetworkTimeoutOutOfRange) {
    LdapCfgRestFixture f(nextPort());
    auto cli = f.client();
    json req = {
        {"enabled", false},
        {"network_timeout_sec", 0},
    };
    auto r = cli.Put("/api/auth/ldap/config", req.dump(), "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400);
    auto body = json::parse(r->body);
    EXPECT_EQ(body["error"], "network_timeout_sec_out_of_range");
}

// До добавления schema v6 поле network_timeout_sec не сохранялось в БД:
// LdapConfigRow не имел колонки, load() возвращал default 5s, и тест
// LdapTestEndpoint.UseSavedFallsBackToRepoConfig упирался в httplib
// read_timeout. Регрессионный тест: PUT с network_timeout_sec=1
// должен переживать round-trip через repo.
TEST(LdapConfigRest, NetworkTimeoutSecPersistsAcrossLoad) {
    LdapCfgRestFixture f(nextPort());
    auto cli = f.client();
    json req = {
        {"enabled", false},
        {"server",  "127.0.0.1:39999"},
        {"tls_mode","plain"},
        {"base_dn", "dc=x"},
        {"user_filter", "(uid=%s)"},
        {"network_timeout_sec", 1},
    };
    auto r = cli.Put("/api/auth/ldap/config", req.dump(), "application/json");
    ASSERT_TRUE(r);
    ASSERT_EQ(r->status, 200) << r->body;

    auto cfg = f.repo->load();
    ASSERT_TRUE(cfg.has_value());
    EXPECT_EQ(cfg->network_timeout_sec, 1);
}

// ── fix22 commit 21/24 — POST /api/auth/ldap/test ─────────────────────
//
// Идея тестов: каждое обращение endpoint'а ходит в ldap (через
// LdapClient::ping/authenticate). Чтобы не зависеть от живого DC и не
// мариноваться по таймауту, используем 127.0.0.1 + заведомо непривязанный
// порт — так connect мгновенно падает с ECONNREFUSED, а валидация и
// маппинг ролей всё равно прогоняются. Контракт endpoint'а:
//   - 503, если LdapConfigRepo не подключён (smoke-defence).
//   - 400, если итоговый cfg не валиден (например base_dn пуст).
//   - 200 + ping_obj.ok=false для unreachable host'а — именно это admin
//     и хочет увидеть в реальном debug-сценарии.
//   - 200 + bind block, если username/password переданы (но bind тоже
//     даст ok=false для unreachable host).

TEST(LdapTestEndpoint, ReturnsPingFailureForUnreachableHost) {
    LdapCfgRestFixture f(nextPort());
    auto cli = f.client();

    json req = {
        {"use_saved", false},
        {"server",  "127.0.0.1:39998"},
        {"tls_mode","plain"},
        {"base_dn", "dc=corp,dc=example,dc=com"},
        {"bind_dn", "cn=svc,dc=corp,dc=example,dc=com"},
        {"bind_password", "svc"},
        {"user_filter", "(uid=%s)"},
        {"network_timeout_sec", 1},
    };
    auto r = cli.Post("/api/auth/ldap/test", req.dump(), "application/json");
    ASSERT_TRUE(r);
    ASSERT_EQ(r->status, 200) << r->body;
    auto body = json::parse(r->body);
    ASSERT_TRUE(body.contains("ping"));
    EXPECT_EQ(body["ping"]["ok"], false);
    EXPECT_FALSE(body["ping"]["error"].get<std::string>().empty());
    // bind block отсутствует, потому что username/password не переданы.
    EXPECT_FALSE(body.contains("bind"));
}

TEST(LdapTestEndpoint, ReturnsBindBlockWhenCredsProvided) {
    LdapCfgRestFixture f(nextPort());
    auto cli = f.client();

    json req = {
        {"use_saved", false},
        {"server",  "127.0.0.1:39997"},
        {"tls_mode","plain"},
        {"base_dn", "dc=corp,dc=example,dc=com"},
        {"bind_dn", "cn=svc,dc=corp,dc=example,dc=com"},
        {"bind_password", "svc"},
        {"user_filter", "(uid=%s)"},
        {"network_timeout_sec", 1},
        {"username", "alice"},
        {"password", "hunter2"},
    };
    auto r = cli.Post("/api/auth/ldap/test", req.dump(), "application/json");
    ASSERT_TRUE(r);
    ASSERT_EQ(r->status, 200) << r->body;
    auto body = json::parse(r->body);
    ASSERT_TRUE(body.contains("bind"));
    EXPECT_EQ(body["bind"]["ok"], false);
    // Для unreachable host'а ожидаем connection_failed либо service_bind_failed
    // (libldap иногда лениво переключает их местами).
    const auto reason = body["bind"]["reason"].get<std::string>();
    EXPECT_TRUE(reason == "connection_failed" || reason == "service_bind_failed")
        << "reason=" << reason;
    // mapped_role / grants появляются только при ok=true — здесь их нет.
    EXPECT_FALSE(body["bind"].contains("mapped_role"));
    EXPECT_FALSE(body["bind"].contains("grants"));
}

TEST(LdapTestEndpoint, EmptyCredsRunOnlyPing) {
    LdapCfgRestFixture f(nextPort());
    auto cli = f.client();

    json req = {
        {"use_saved", false},
        {"server",  "127.0.0.1:39996"},
        {"tls_mode","plain"},
        {"base_dn", "dc=x"},
        {"user_filter", "(uid=%s)"},
        {"network_timeout_sec", 1},
        {"username", "alice"},
        {"password", ""},  // пустой → bind не должен запускаться
    };
    auto r = cli.Post("/api/auth/ldap/test", req.dump(), "application/json");
    ASSERT_TRUE(r);
    ASSERT_EQ(r->status, 200) << r->body;
    auto body = json::parse(r->body);
    EXPECT_TRUE(body.contains("ping"));
    EXPECT_FALSE(body.contains("bind"));
}

TEST(LdapTestEndpoint, RejectsInvalidConfig) {
    LdapCfgRestFixture f(nextPort());
    auto cli = f.client();
    // user_filter без %s → validate упадёт.
    json req = {
        {"use_saved", false},
        {"server",  "127.0.0.1:39995"},
        {"tls_mode","plain"},
        {"base_dn", "dc=x"},
        {"user_filter", "(uid=alice)"},
    };
    auto r = cli.Post("/api/auth/ldap/test", req.dump(), "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400);
    auto body = json::parse(r->body);
    EXPECT_EQ(body["error"], "invalid_config");
    EXPECT_TRUE(body.contains("detail"));
}

TEST(LdapTestEndpoint, RejectsInvalidTlsModeOverride) {
    LdapCfgRestFixture f(nextPort());
    auto cli = f.client();
    json req = {
        {"use_saved", false},
        {"tls_mode", "garbage"},
    };
    auto r = cli.Post("/api/auth/ldap/test", req.dump(), "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400);
    EXPECT_EQ(json::parse(r->body)["error"], "invalid_tls_mode");
}

TEST(LdapTestEndpoint, RejectsInvalidNetworkTimeoutOverride) {
    LdapCfgRestFixture f(nextPort());
    auto cli = f.client();
    json req = {
        {"use_saved", false},
        {"network_timeout_sec", 9999},  // >60 → out_of_range
    };
    auto r = cli.Post("/api/auth/ldap/test", req.dump(), "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400);
    EXPECT_EQ(json::parse(r->body)["error"],
              "network_timeout_sec_out_of_range");
}

TEST(LdapTestEndpoint, RejectsInvalidRoleOverride) {
    LdapCfgRestFixture f(nextPort());
    auto cli = f.client();
    json req = {
        {"use_saved", false},
        {"group_role_map", { {"CN=foo,DC=x", "warlord"} }},
    };
    auto r = cli.Post("/api/auth/ldap/test", req.dump(), "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400);
    EXPECT_EQ(json::parse(r->body)["error"], "invalid_role_in_map");
}

TEST(LdapTestEndpoint, RejectsInvalidPermissionOverride) {
    LdapCfgRestFixture f(nextPort());
    auto cli = f.client();
    json req = {
        {"use_saved", false},
        {"channel_acl", json::array({
            {{"channel_id", 1},
             {"groups", { {"CN=ops,DC=x", "godmode"} }}},
        })},
    };
    auto r = cli.Post("/api/auth/ldap/test", req.dump(), "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400);
    EXPECT_EQ(json::parse(r->body)["error"], "invalid_permission_in_acl");
}

TEST(LdapTestEndpoint, UseSavedFallsBackToRepoConfig) {
    LdapCfgRestFixture f(nextPort());
    auto cli = f.client();

    // Сначала сохраняем валидный (но disabled — это стандартный сценарий
    // у админа: «настроил всё, но ещё не включил»).
    json save = {
        {"enabled", false},
        {"server",  "127.0.0.1:39994"},
        {"tls_mode","plain"},
        {"base_dn", "dc=corp,dc=example,dc=com"},
        {"bind_dn", "cn=svc,dc=corp,dc=example,dc=com"},
        {"bind_password", "svc"},
        {"user_filter", "(uid=%s)"},
        {"network_timeout_sec", 1},
    };
    auto put = cli.Put("/api/auth/ldap/config", save.dump(), "application/json");
    ASSERT_TRUE(put);
    ASSERT_EQ(put->status, 200) << put->body;

    // POST /test без body → use_saved=true дефолт, ping должен прогнаться.
    auto r = cli.Post("/api/auth/ldap/test", "", "application/json");
    ASSERT_TRUE(r);
    ASSERT_EQ(r->status, 200) << r->body;
    auto body = json::parse(r->body);
    EXPECT_TRUE(body.contains("ping"));
    EXPECT_EQ(body["ping"]["ok"], false);
}

TEST(LdapTestEndpoint, AuditEventEmitted) {
    LdapCfgRestFixture f(nextPort());
    auto cli = f.client();
    json req = {
        {"use_saved", false},
        {"server",  "127.0.0.1:39993"},
        {"tls_mode","plain"},
        {"base_dn", "dc=x"},
        {"user_filter", "(uid=%s)"},
        {"network_timeout_sec", 1},
        {"username", "carol"},
        {"password", "pw"},
    };
    auto r = cli.Post("/api/auth/ldap/test", req.dump(), "application/json");
    ASSERT_TRUE(r);
    ASSERT_EQ(r->status, 200) << r->body;

    liveqx::auth::AuditFilter af;
    af.event = "admin.ldap.test";
    af.limit = 16;
    auto events = f.db->listAuditEvents(af);
    ASSERT_FALSE(events.empty());
    bool found = false;
    for (const auto& e : events) {
        if (e.details_json.find("\"username\":\"carol\"") != std::string::npos) {
            found = true;
            EXPECT_NE(e.details_json.find("\"ran_bind\":true"),
                      std::string::npos) << e.details_json;
            break;
        }
    }
    EXPECT_TRUE(found);
}

TEST(LdapTestEndpoint, ReturnsServiceUnavailableWithoutRepo) {
    // Обходим стандартный fixture и поднимаем ControlApi БЕЗ LdapConfigRepo:
    // endpoint должен 503'нуть, а не уйти в segfault/мусор.
    namespace sa = liveqx::auth;
    const int port = nextPort();
    auto dbpath = std::filesystem::temp_directory_path() /
        ("ldap_test_no_repo_" + std::to_string(port) + ".db");
    std::filesystem::remove(dbpath);

    sa::AuthDb db(dbpath);
    ASSERT_TRUE(db.open());
    sa::JwtIssuer jwt{std::string(kSecret)};
    sa::AuthService::GrantsResolver grants =
        [](std::int64_t) { return std::vector<sa::ChannelGrant>{}; };
    sa::AuthService svc(db, jwt, grants);
    ChannelManager mgr(nullptr);
    ControlApi api(port, mgr,
                   /*metrics=*/nullptr, LivezOptions{},
                   /*gateways=*/nullptr,
                   &svc,
                   /*ldap_repo=*/nullptr);
    api.start();
    {
        httplib::Client probe("127.0.0.1", port);
        probe.set_connection_timeout(0, 50'000);
        for (int i = 0; i < 100; ++i) {
            auto rr = probe.Get("/healthz");
            if (rr && rr->status == 200) break;
            std::this_thread::sleep_for(20ms);
        }
    }

    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(2, 0);
    cli.set_read_timeout(2, 0);
    auto r = cli.Post("/api/auth/ldap/test", "{}", "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 503);
    EXPECT_EQ(json::parse(r->body)["error"], "ldap_repo_unavailable");

    api.stop();
    std::error_code ec;
    std::filesystem::remove(dbpath, ec);
    std::filesystem::remove(dbpath.string() + "-wal", ec);
    std::filesystem::remove(dbpath.string() + "-shm", ec);
}

TEST(LdapConfigRest, ChannelAclRoundTrips) {
    LdapCfgRestFixture f(nextPort());
    auto cli = f.client();
    json req = {
        {"enabled", false},
        {"channel_acl", json::array({
            {{"channel_id", 7},
             {"groups", { {"CN=ops,DC=x", "operate"},
                          {"CN=ad,DC=x", "view"} }}},
            {{"channel_id", 12},
             {"groups", { {"CN=mgr,DC=x", "operate"} }}},
        })},
    };
    auto r = cli.Put("/api/auth/ldap/config", req.dump(), "application/json");
    ASSERT_TRUE(r);
    ASSERT_EQ(r->status, 200) << r->body;
    auto body = json::parse(r->body);
    ASSERT_TRUE(body["channel_acl"].is_array());
    ASSERT_EQ(body["channel_acl"].size(), 2u);
    bool found7 = false, found12 = false;
    for (const auto& acl : body["channel_acl"]) {
        if (acl["channel_id"] == 7) {
            found7 = true;
            EXPECT_EQ(acl["groups"]["CN=ops,DC=x"], "operate");
            EXPECT_EQ(acl["groups"]["CN=ad,DC=x"],  "view");
        }
        if (acl["channel_id"] == 12) found12 = true;
    }
    EXPECT_TRUE(found7);
    EXPECT_TRUE(found12);
}
