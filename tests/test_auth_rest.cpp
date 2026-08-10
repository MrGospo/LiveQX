// fix22 commit 5/24 — REST интеграционный тест /api/auth/{login,refresh,logout}.
//
// Spins up ControlApi с настоящими AuthDb/JwtIssuer/AuthService и делает
// HTTP-вызовы через httplib::Client.

#include <chrono>
#include <filesystem>
#include <thread>

#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include "api/ChannelManager.h"
#include "api/ControlApi.h"
#include "auth/AuthDb.h"
#include "auth/AuthService.h"
#include "auth/JwtIssuer.h"
#include "auth/PasswordHasher.h"

namespace {

using nlohmann::json;
using namespace std::chrono_literals;

constexpr int kAuthRestPort         = 18101;
constexpr int kAuthRestUnconfigured = 18102;
constexpr const char* kSecret = "0123456789abcdef0123456789abcdef";

struct AuthRestFixture {
    std::filesystem::path           dbpath;
    std::unique_ptr<liveqx::auth::AuthDb>      db;
    std::unique_ptr<liveqx::auth::JwtIssuer>   jwt;
    std::unique_ptr<liveqx::auth::AuthService> svc;
    ChannelManager                                     manager;
    std::unique_ptr<ControlApi>                        api;
    int                                                port;

    explicit AuthRestFixture(int p) : manager(nullptr), port(p) {
        dbpath = std::filesystem::temp_directory_path() /
            ("auth_rest_test_" + std::to_string(p) + ".db");
        std::filesystem::remove(dbpath);

        db  = std::make_unique<liveqx::auth::AuthDb>(dbpath);
        const bool open_ok = db->open();
        EXPECT_TRUE(open_ok);

        jwt = std::make_unique<liveqx::auth::JwtIssuer>(std::string(kSecret));
        svc = std::make_unique<liveqx::auth::AuthService>(
            *db, *jwt,
            [](std::int64_t) { return std::vector<liveqx::auth::ChannelGrant>{}; });

        api = std::make_unique<ControlApi>(
            port, manager,
            /*metrics=*/nullptr, LivezOptions{},
            /*gateways=*/nullptr,
            svc.get());
        api->start();
        waitListening(port);
    }

    ~AuthRestFixture() {
        api->stop();
        api.reset();
        svc.reset();
        jwt.reset();
        db.reset();
        std::error_code ec;
        std::filesystem::remove(dbpath, ec);
        std::filesystem::remove(dbpath.string() + "-wal", ec);
        std::filesystem::remove(dbpath.string() + "-shm", ec);
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

    std::int64_t insertUser(const std::string& name,
                            const std::string& password,
                            bool disabled = false) {
        auto h = liveqx::auth::PasswordHasher::hash(password);
        EXPECT_TRUE(h.has_value());
        liveqx::auth::User u;
        u.username      = name;
        u.password_hash = *h;
        u.role          = liveqx::auth::Role::Operator;
        u.source        = liveqx::auth::Source::Local;
        u.disabled      = disabled;
        auto id = db->insertUser(u);
        EXPECT_TRUE(id.has_value());
        return *id;
    }
};

}  // namespace

TEST(AuthRest, LoginRefreshLogoutFlow) {
    AuthRestFixture f(kAuthRestPort);
    f.insertUser("alice", "hunter2");

    httplib::Client cli("127.0.0.1", kAuthRestPort);
    cli.set_connection_timeout(0, 200'000);
    cli.set_read_timeout(2, 0);

    // ─── login ───
    auto r1 = cli.Post("/api/auth/login", R"({"username":"alice","password":"hunter2"})",
                       "application/json");
    ASSERT_TRUE(r1);
    ASSERT_EQ(r1->status, 200);
    auto body1 = json::parse(r1->body);
    EXPECT_EQ(body1["token_type"], "Bearer");
    ASSERT_TRUE(body1["access_token"].is_string());
    ASSERT_TRUE(body1["refresh_token"].is_string());
    EXPECT_EQ(body1["user"]["username"], "alice");
    EXPECT_EQ(body1["user"]["role"], "operator");
    const auto access1  = body1["access_token"].get<std::string>();
    const auto refresh1 = body1["refresh_token"].get<std::string>();

    // ─── refresh ───
    json refresh_body = {{"refresh_token", refresh1}};
    auto r2 = cli.Post("/api/auth/refresh", refresh_body.dump(), "application/json");
    ASSERT_TRUE(r2);
    ASSERT_EQ(r2->status, 200);
    auto body2 = json::parse(r2->body);
    const auto access2  = body2["access_token"].get<std::string>();
    const auto refresh2 = body2["refresh_token"].get<std::string>();
    EXPECT_NE(access2,  access1);
    EXPECT_NE(refresh2, refresh1);

    // ─── reuse old refresh — replay, отлуп ───
    auto r3 = cli.Post("/api/auth/refresh", refresh_body.dump(), "application/json");
    ASSERT_TRUE(r3);
    EXPECT_EQ(r3->status, 401);
    auto body3 = json::parse(r3->body);
    EXPECT_EQ(body3["error"], "token_revoked");

    // ─── logout c новым access ───
    httplib::Headers hdr{{"Authorization", "Bearer " + access2}};
    auto r4 = cli.Post("/api/auth/logout", hdr, "", "application/json");
    ASSERT_TRUE(r4);
    EXPECT_EQ(r4->status, 200);

    // ─── повторный logout с тем же access — теперь токен невалиден (сессия revoked) ───
    auto r5 = cli.Post("/api/auth/logout", hdr, "", "application/json");
    ASSERT_TRUE(r5);
    EXPECT_EQ(r5->status, 401);
}

TEST(AuthRest, LoginInvalidPasswordHidesUserExistence) {
    AuthRestFixture f(kAuthRestPort + 10);
    f.insertUser("alice", "hunter2");

    httplib::Client cli("127.0.0.1", kAuthRestPort + 10);
    cli.set_connection_timeout(0, 200'000);
    cli.set_read_timeout(2, 0);

    // Wrong password.
    auto r1 = cli.Post("/api/auth/login", R"({"username":"alice","password":"WRONG"})",
                       "application/json");
    ASSERT_TRUE(r1);
    EXPECT_EQ(r1->status, 401);
    auto b1 = json::parse(r1->body);
    EXPECT_EQ(b1["error"], "invalid_credentials");

    // Unknown user — same response.
    auto r2 = cli.Post("/api/auth/login", R"({"username":"nobody","password":"x"})",
                       "application/json");
    ASSERT_TRUE(r2);
    EXPECT_EQ(r2->status, 401);
    auto b2 = json::parse(r2->body);
    EXPECT_EQ(b2["error"], "invalid_credentials");
}

TEST(AuthRest, LoginDisabledUserReturns403) {
    AuthRestFixture f(kAuthRestPort + 20);
    f.insertUser("alice", "hunter2", /*disabled=*/true);

    httplib::Client cli("127.0.0.1", kAuthRestPort + 20);
    cli.set_connection_timeout(0, 200'000);
    cli.set_read_timeout(2, 0);

    auto r = cli.Post("/api/auth/login", R"({"username":"alice","password":"hunter2"})",
                      "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 403);
    auto b = json::parse(r->body);
    EXPECT_EQ(b["error"], "user_disabled");
}

TEST(AuthRest, LoginMissingFieldsReturns400) {
    AuthRestFixture f(kAuthRestPort + 30);
    httplib::Client cli("127.0.0.1", kAuthRestPort + 30);
    cli.set_connection_timeout(0, 200'000);
    cli.set_read_timeout(2, 0);

    auto r = cli.Post("/api/auth/login", R"({"username":"alice"})", "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400);
    auto b = json::parse(r->body);
    EXPECT_EQ(b["error"], "missing_credentials");
}

TEST(AuthRest, RefreshUnknownTokenReturns401) {
    AuthRestFixture f(kAuthRestPort + 40);
    httplib::Client cli("127.0.0.1", kAuthRestPort + 40);
    cli.set_connection_timeout(0, 200'000);
    cli.set_read_timeout(2, 0);

    auto r = cli.Post("/api/auth/refresh",
                      R"({"refresh_token":"not-a-real-token"})", "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 401);
    auto b = json::parse(r->body);
    EXPECT_EQ(b["error"], "token_not_found");
}

TEST(AuthRest, LogoutWithoutBearerReturns401) {
    AuthRestFixture f(kAuthRestPort + 50);
    httplib::Client cli("127.0.0.1", kAuthRestPort + 50);
    cli.set_connection_timeout(0, 200'000);
    cli.set_read_timeout(2, 0);

    auto r = cli.Post("/api/auth/logout", "", "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 401);
    auto b = json::parse(r->body);
    EXPECT_EQ(b["error"], "missing_bearer");
}

TEST(AuthRest, LogoutWithGarbageBearerReturns401) {
    AuthRestFixture f(kAuthRestPort + 60);
    httplib::Client cli("127.0.0.1", kAuthRestPort + 60);
    cli.set_connection_timeout(0, 200'000);
    cli.set_read_timeout(2, 0);

    httplib::Headers hdr{{"Authorization", "Bearer aaa.bbb.ccc"}};
    auto r = cli.Post("/api/auth/logout", hdr, "", "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 401);
    auto b = json::parse(r->body);
    EXPECT_EQ(b["error"], "invalid_token");
}

TEST(AuthRest, AuthEndpointsReturn503WhenServiceNotConfigured) {
    // ControlApi без AuthService — все три auth-эндпоинта 503.
    ChannelManager manager(nullptr);
    ControlApi api(kAuthRestUnconfigured, manager,
                   /*metrics=*/nullptr, LivezOptions{},
                   /*gateways=*/nullptr,
                   /*auth=*/nullptr);
    api.start();

    httplib::Client cli("127.0.0.1", kAuthRestUnconfigured);
    cli.set_connection_timeout(0, 200'000);
    cli.set_read_timeout(2, 0);
    for (int i = 0; i < 100; ++i) {
        auto r = cli.Get("/healthz");
        if (r && r->status == 200) break;
        std::this_thread::sleep_for(20ms);
    }

    auto r1 = cli.Post("/api/auth/login", R"({"username":"a","password":"b"})",
                       "application/json");
    ASSERT_TRUE(r1);
    EXPECT_EQ(r1->status, 503);

    auto r2 = cli.Post("/api/auth/refresh", R"({"refresh_token":"x"})",
                       "application/json");
    ASSERT_TRUE(r2);
    EXPECT_EQ(r2->status, 503);

    auto r3 = cli.Post("/api/auth/logout", "", "application/json");
    ASSERT_TRUE(r3);
    EXPECT_EQ(r3->status, 503);

    auto r4 = cli.Post("/api/auth/me/password",
                       R"({"current_password":"a","new_password":"bbcccccc"})",
                       "application/json");
    ASSERT_TRUE(r4);
    EXPECT_EQ(r4->status, 503);

    api.stop();
}

// ─── Self-change password (commit 8/24) ────────────────────────────────

TEST(AuthRest, MePasswordHappyPath) {
    AuthRestFixture f(kAuthRestPort + 70);
    f.insertUser("ivy", "init-pw-12345");

    httplib::Client cli("127.0.0.1", kAuthRestPort + 70);
    cli.set_connection_timeout(0, 200'000);
    cli.set_read_timeout(2, 0);

    auto r1 = cli.Post("/api/auth/login",
                       R"({"username":"ivy","password":"init-pw-12345"})",
                       "application/json");
    ASSERT_TRUE(r1);
    ASSERT_EQ(r1->status, 200);
    auto access = json::parse(r1->body)["access_token"].get<std::string>();

    httplib::Headers hdr{{"Authorization", "Bearer " + access}};
    auto r2 = cli.Post("/api/auth/me/password", hdr,
                       R"({"current_password":"init-pw-12345","new_password":"new-strong-pw"})",
                       "application/json");
    ASSERT_TRUE(r2);
    EXPECT_EQ(r2->status, 200);

    // login old → 401
    auto r3 = cli.Post("/api/auth/login",
                       R"({"username":"ivy","password":"init-pw-12345"})",
                       "application/json");
    ASSERT_TRUE(r3);
    EXPECT_EQ(r3->status, 401);

    // login new → 200
    auto r4 = cli.Post("/api/auth/login",
                       R"({"username":"ivy","password":"new-strong-pw"})",
                       "application/json");
    ASSERT_TRUE(r4);
    EXPECT_EQ(r4->status, 200);
}

TEST(AuthRest, MePasswordWithoutBearerReturns401) {
    AuthRestFixture f(kAuthRestPort + 80);
    httplib::Client cli("127.0.0.1", kAuthRestPort + 80);
    cli.set_connection_timeout(0, 200'000);
    cli.set_read_timeout(2, 0);

    auto r = cli.Post("/api/auth/me/password",
                      R"({"current_password":"a","new_password":"bbbbbbbb"})",
                      "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 401);
    EXPECT_EQ(json::parse(r->body)["error"], "missing_bearer");
}

TEST(AuthRest, MePasswordWrongCurrentReturns403) {
    AuthRestFixture f(kAuthRestPort + 90);
    f.insertUser("kim", "real-pw-12345");

    httplib::Client cli("127.0.0.1", kAuthRestPort + 90);
    cli.set_connection_timeout(0, 200'000);
    cli.set_read_timeout(2, 0);

    auto r1 = cli.Post("/api/auth/login",
                       R"({"username":"kim","password":"real-pw-12345"})",
                       "application/json");
    auto access = json::parse(r1->body)["access_token"].get<std::string>();

    httplib::Headers hdr{{"Authorization", "Bearer " + access}};
    auto r = cli.Post("/api/auth/me/password", hdr,
                      R"({"current_password":"wrong","new_password":"new-strong-pw"})",
                      "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 403);
    EXPECT_EQ(json::parse(r->body)["error"], "current_password_wrong");
}

TEST(AuthRest, MePasswordWeakReturns400) {
    AuthRestFixture f(kAuthRestPort + 100);
    f.insertUser("liz", "good-pw-12345");

    httplib::Client cli("127.0.0.1", kAuthRestPort + 100);
    cli.set_connection_timeout(0, 200'000);
    cli.set_read_timeout(2, 0);

    auto r1 = cli.Post("/api/auth/login",
                       R"({"username":"liz","password":"good-pw-12345"})",
                       "application/json");
    auto access = json::parse(r1->body)["access_token"].get<std::string>();

    httplib::Headers hdr{{"Authorization", "Bearer " + access}};
    auto r = cli.Post("/api/auth/me/password", hdr,
                      R"({"current_password":"good-pw-12345","new_password":"short"})",
                      "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400);
    EXPECT_EQ(json::parse(r->body)["error"], "new_password_weak");
}

TEST(AuthRest, MePasswordMissingFieldsReturns400) {
    AuthRestFixture f(kAuthRestPort + 110);
    f.insertUser("max", "good-pw-12345");

    httplib::Client cli("127.0.0.1", kAuthRestPort + 110);
    cli.set_connection_timeout(0, 200'000);
    cli.set_read_timeout(2, 0);

    auto r1 = cli.Post("/api/auth/login",
                       R"({"username":"max","password":"good-pw-12345"})",
                       "application/json");
    auto access = json::parse(r1->body)["access_token"].get<std::string>();

    httplib::Headers hdr{{"Authorization", "Bearer " + access}};
    auto r = cli.Post("/api/auth/me/password", hdr,
                      R"({"current_password":"good-pw-12345"})",
                      "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400);
    EXPECT_EQ(json::parse(r->body)["error"], "missing_fields");
}

// ─── /api/auth/me ──────────────────────────────────────────────────────────
TEST(AuthRest, MeReturnsCurrentUser) {
    AuthRestFixture f(kAuthRestPort + 120);
    f.insertUser("dora", "hunter2-strong");

    httplib::Client cli("127.0.0.1", kAuthRestPort + 120);
    cli.set_connection_timeout(0, 200'000);
    cli.set_read_timeout(2, 0);

    // login c явным IP — backend сохраняет last_login_ip.
    httplib::Headers login_hdr{{"X-Forwarded-For", "10.0.0.5"}};
    (void)login_hdr;  // httplib::Client не подкладывает X-Forwarded-For; реальный
                      // remote_addr — 127.0.0.1, его и проверим.
    auto r1 = cli.Post("/api/auth/login",
                       R"({"username":"dora","password":"hunter2-strong"})",
                       "application/json");
    ASSERT_TRUE(r1);
    ASSERT_EQ(r1->status, 200);
    const auto access = json::parse(r1->body)["access_token"].get<std::string>();

    httplib::Headers hdr{{"Authorization", "Bearer " + access}};
    auto r = cli.Get("/api/auth/me", hdr);
    ASSERT_TRUE(r);
    ASSERT_EQ(r->status, 200);
    auto body = json::parse(r->body);
    EXPECT_EQ(body["username"], "dora");
    EXPECT_EQ(body["role"],     "operator");
    EXPECT_EQ(body["source"],   "local");
    // last_login_at эмитится только если AuthService записал в БД при login —
    // он записывает (см. recordLoginSuccess), так что поле должно быть.
    EXPECT_TRUE(body.contains("last_login_at"));
    EXPECT_TRUE(body.contains("last_login_ip"));
}

TEST(AuthRest, MeRequiresBearer) {
    AuthRestFixture f(kAuthRestPort + 130);

    httplib::Client cli("127.0.0.1", kAuthRestPort + 130);
    cli.set_connection_timeout(0, 200'000);
    cli.set_read_timeout(2, 0);

    auto r = cli.Get("/api/auth/me");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 401);
}
