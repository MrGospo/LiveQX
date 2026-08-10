// fix22 commit 24/24 — REST integration test для RBAC pre-handler.
//
// Проверяем, что когда ControlApi получает RbacMiddleware*, pre-routing
// handler действительно срабатывает и:
//   - открытые endpoint'ы (probes/login) не требуют токена;
//   - закрытые без Bearer возвращают 401;
//   - role-rule отдаёт 403 при недостатке привилегий;
//   - admin role проходит admin-only endpoint;
//   - незарегистрированный путь даёт 500 rbac.misconfigured (default-deny);
//   - валидный токен с Viewer-role читает /api/channels.
//
// per-channel ACL уже проверен в test_rbac_middleware.cpp на уровне
// authorize() — здесь не дублируем, чтобы не плодить REST-фикстуры
// с реальными каналами.

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
#include "auth/RbacMiddleware.h"

namespace {

using nlohmann::json;
using namespace std::chrono_literals;

constexpr int kRbacRestPortBase = 21500;
constexpr const char* kSecret   = "0123456789abcdef0123456789abcdef";

int nextPort() {
    static std::atomic<int> n{0};
    static const int base =
        kRbacRestPortBase + (static_cast<int>(getpid()) % 600) * 5;
    return base + n.fetch_add(1);
}

struct RbacRestFixture {
    std::filesystem::path                                  dbpath;
    std::unique_ptr<liveqx::auth::AuthDb>          db;
    std::unique_ptr<liveqx::auth::JwtIssuer>       jwt;
    std::unique_ptr<liveqx::auth::AuthService>     svc;
    std::unique_ptr<liveqx::auth::RbacMiddleware>  rbac;
    ChannelManager                                         manager;
    std::unique_ptr<ControlApi>                            api;
    int                                                    port;

    explicit RbacRestFixture(int p) : manager(nullptr), port(p) {
        dbpath = std::filesystem::temp_directory_path() /
            ("rbac_rest_" + std::to_string(p) + ".db");
        std::filesystem::remove(dbpath);

        db  = std::make_unique<liveqx::auth::AuthDb>(dbpath);
        EXPECT_TRUE(db->open());
        jwt = std::make_unique<liveqx::auth::JwtIssuer>(std::string(kSecret));
        svc = std::make_unique<liveqx::auth::AuthService>(
            *db, *jwt,
            [](std::int64_t) { return std::vector<liveqx::auth::ChannelGrant>{}; });
        rbac = std::make_unique<liveqx::auth::RbacMiddleware>(*db, *jwt);

        api = std::make_unique<ControlApi>(
            port, manager,
            /*metrics=*/nullptr, LivezOptions{},
            /*gateways=*/nullptr,
            svc.get(),
            /*ldap_repo=*/nullptr,
            /*smtp_repo=*/nullptr,
            rbac.get());
        api->start();
        waitListening(port);
    }

    ~RbacRestFixture() {
        api->stop();
        api.reset();
        rbac.reset();
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

    httplib::Client client() {
        httplib::Client c("127.0.0.1", port);
        c.set_connection_timeout(2, 0);
        c.set_read_timeout(15, 0);
        return c;
    }

    // Создаёт юзера и возвращает свежий access-token.
    std::string loginAs(const std::string& username,
                        liveqx::auth::Role role) {
        liveqx::auth::AuthService::CreateUserRequest req;
        req.username             = username;
        req.password             = "Pass1234!";
        req.role                 = role;
        req.must_change_password = false;
        auto cr = svc->adminCreateUser(req);
        EXPECT_TRUE(std::holds_alternative<
                    liveqx::auth::AuthService::CreatedUser>(cr));

        auto r = svc->login(username, "Pass1234!", "127.0.0.1", "test");
        EXPECT_TRUE(std::holds_alternative<
                    liveqx::auth::JwtIssuer::TokenPair>(r.outcome));
        return std::get<liveqx::auth::JwtIssuer::TokenPair>(r.outcome)
            .access_token;
    }
};

httplib::Headers bearer(const std::string& token) {
    return {{"Authorization", "Bearer " + token}};
}

}  // namespace

TEST(RbacRest, OpenEndpointsBypassAuth) {
    RbacRestFixture f(nextPort());
    auto cli = f.client();
    {
        auto r = cli.Get("/healthz");
        ASSERT_TRUE(r);
        EXPECT_EQ(r->status, 200);
    }
    {
        auto r = cli.Get("/livez");
        ASSERT_TRUE(r);
        EXPECT_EQ(r->status, 200);
    }
    {
        auto r = cli.Post("/api/auth/login",
            json({{"username", "doesnotexist"}, {"password", "x"}}).dump(),
            "application/json");
        ASSERT_TRUE(r);
        // 401 — login сам отверг неверные creds, НЕ pre-handler.
        EXPECT_EQ(r->status, 401);
    }
}

TEST(RbacRest, MissingTokenReturns401OnProtected) {
    RbacRestFixture f(nextPort());
    auto cli = f.client();
    auto r = cli.Get("/api/channels");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 401);
    auto body = json::parse(r->body);
    EXPECT_EQ(body["error"], "unauthorized");
}

TEST(RbacRest, BadBearerReturns401) {
    RbacRestFixture f(nextPort());
    auto cli = f.client();
    auto r = cli.Get("/api/channels", bearer("not.a.real.jwt"));
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 401);
}

TEST(RbacRest, ViewerCanListChannels) {
    RbacRestFixture f(nextPort());
    auto cli   = f.client();
    auto token = f.loginAs("alice_viewer", liveqx::auth::Role::Viewer);

    auto r = cli.Get("/api/channels", bearer(token));
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 200);
}

TEST(RbacRest, ViewerCannotCreateChannel) {
    RbacRestFixture f(nextPort());
    auto cli   = f.client();
    auto token = f.loginAs("bob_viewer", liveqx::auth::Role::Viewer);

    auto r = cli.Post("/api/channels",
                      bearer(token),
                      json::object().dump(),
                      "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 403);
    auto body = json::parse(r->body);
    EXPECT_EQ(body["error"], "forbidden");
}

TEST(RbacRest, OperatorCannotCreateChannel) {
    // POST /api/channels требует Admin — operator тоже отлетает.
    RbacRestFixture f(nextPort());
    auto cli   = f.client();
    auto token = f.loginAs("eve_op", liveqx::auth::Role::Operator);

    auto r = cli.Post("/api/channels",
                      bearer(token),
                      json::object().dump(),
                      "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 403);
}

TEST(RbacRest, AdminPassesRolePreHandler) {
    // Admin проходит pre-handler: главное — НЕ 401/403. Сам ChannelManager
    // на пустом body может вернуть 201 (канал создан с дефолтами) либо 4xx
    // (валидация). Pre-handler-успех = passthrough; конкретный код выбирает
    // ChannelManager и здесь не предмет проверки.
    RbacRestFixture f(nextPort());
    auto cli   = f.client();
    auto token = f.loginAs("root_admin", liveqx::auth::Role::Admin);

    auto r = cli.Post("/api/channels",
                      bearer(token),
                      json::object().dump(),
                      "application/json");
    ASSERT_TRUE(r);
    EXPECT_NE(r->status, 401);
    EXPECT_NE(r->status, 403);
}

TEST(RbacRest, UnknownPathReturnsRbacMisconfigured) {
    // default-deny: для незарегистрированного маршрута авторизация
    // возвращает NotConfigured → 500 с error="rbac.misconfigured".
    RbacRestFixture f(nextPort());
    auto cli = f.client();
    auto r = cli.Get("/api/this-route-does-not-exist");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 500);
    auto body = json::parse(r->body);
    EXPECT_EQ(body["error"], "rbac.misconfigured");
}

TEST(RbacRest, AnyAuthenticatedCanLogout) {
    RbacRestFixture f(nextPort());
    auto cli   = f.client();
    auto token = f.loginAs("zoe_viewer", liveqx::auth::Role::Viewer);

    auto r = cli.Post("/api/auth/logout", bearer(token), "", "application/json");
    ASSERT_TRUE(r);
    // 200/204 — passthrough; главное не 401/403.
    EXPECT_NE(r->status, 401);
    EXPECT_NE(r->status, 403);
}
