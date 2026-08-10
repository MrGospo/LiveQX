// fix22 commit 7/24 — REST интеграционный тест admin user CRUD.
//
// /api/auth/users {GET, POST}
// /api/auth/users/{id} {GET, PUT, DELETE}
// /api/auth/users/{id}/enable
// /api/auth/users/{id}/reset-password

#include <chrono>
#include <filesystem>
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
#include "auth/AuthDb.h"
#include "auth/AuthService.h"
#include "auth/JwtIssuer.h"
#include "auth/PasswordHasher.h"

namespace {

using nlohmann::json;
using namespace std::chrono_literals;

constexpr const char* kSecret = "0123456789abcdef0123456789abcdef";

// Под `ctest -j` все тесты — отдельные процессы. Раньше port выбирался
// как `18120 + (pid % 800) * 5`, и при ~16 параллельных воркерах PID-
// хэш регулярно коллидил → два ControlApi пытались встать на тот же
// порт и waitListening() падал с "never started".
//
// Корректное решение — попросить ядро у себя порт через bind(0) и
// сразу его освободить: до повторного использования промежуток крайне
// мал, а пул вариантов — весь эфемерный диапазон.
int nextPort() {
    int s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = 0;
    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(s);
        return -1;
    }
    sockaddr_in actual{};
    socklen_t   len = sizeof(actual);
    ::getsockname(s, reinterpret_cast<sockaddr*>(&actual), &len);
    int port = ntohs(actual.sin_port);
    ::close(s);
    return port;
}

struct AdminRestFixture {
    std::filesystem::path           dbpath;
    std::unique_ptr<liveqx::auth::AuthDb>      db;
    std::unique_ptr<liveqx::auth::JwtIssuer>   jwt;
    std::unique_ptr<liveqx::auth::AuthService> svc;
    ChannelManager                                     manager;
    std::unique_ptr<ControlApi>                        api;
    int                                                port;

    explicit AdminRestFixture(int p) : manager(nullptr), port(p) {
        dbpath = std::filesystem::temp_directory_path() /
            ("admin_rest_test_" + std::to_string(p) + ".db");
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

    ~AdminRestFixture() {
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

    httplib::Client client() {
        httplib::Client c("127.0.0.1", port);
        c.set_connection_timeout(2, 0);
        c.set_read_timeout(5, 0);
        return c;
    }
};

}  // namespace

TEST(AdminUsersRest, ListUsersInitiallyEmpty) {
    AdminRestFixture f(nextPort());
    auto cli = f.client();
    auto r = cli.Get("/api/auth/users");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 200);
    auto body = json::parse(r->body);
    EXPECT_TRUE(body.contains("users"));
    EXPECT_TRUE(body["users"].is_array());
    EXPECT_EQ(body["users"].size(), 0u);
}

TEST(AdminUsersRest, CreateUserWithExplicitPasswordOmitsPlaintextInResponse) {
    AdminRestFixture f(nextPort());
    auto cli = f.client();
    json req = {
        {"username", "alice"},
        {"email",    "alice@example.com"},
        {"password", "topsecret"},
        {"role",     "operator"},
    };
    auto r = cli.Post("/api/auth/users", req.dump(), "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 201);
    auto body = json::parse(r->body);
    EXPECT_EQ(body["username"], "alice");
    EXPECT_EQ(body["email"],    "alice@example.com");
    EXPECT_EQ(body["role"],     "operator");
    EXPECT_FALSE(body.contains("initial_password"));
}

TEST(AdminUsersRest, CreateUserWithoutPasswordReturnsAutogenInitialPassword) {
    AdminRestFixture f(nextPort());
    auto cli = f.client();
    json req = {{"username", "bob"}, {"role", "viewer"}};
    auto r = cli.Post("/api/auth/users", req.dump(), "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 201);
    auto body = json::parse(r->body);
    ASSERT_TRUE(body.contains("initial_password"));
    EXPECT_FALSE(body["initial_password"].get<std::string>().empty());
}

TEST(AdminUsersRest, CreateUserMissingUsernameReturns400) {
    AdminRestFixture f(nextPort());
    auto cli = f.client();
    auto r = cli.Post("/api/auth/users",
                      json({{"role", "viewer"}}).dump(), "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400);
}

TEST(AdminUsersRest, CreateUserInvalidRoleReturns400) {
    AdminRestFixture f(nextPort());
    auto cli = f.client();
    json req = {{"username", "x"}, {"role", "wizard"}};
    auto r = cli.Post("/api/auth/users", req.dump(), "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400);
    auto body = json::parse(r->body);
    EXPECT_EQ(body["error"], "invalid_role");
}

TEST(AdminUsersRest, CreateUserDuplicateReturns409) {
    AdminRestFixture f(nextPort());
    auto cli = f.client();
    json req = {{"username", "dup"}, {"password", "p"}};
    EXPECT_EQ(cli.Post("/api/auth/users", req.dump(), "application/json")->status, 201);
    auto r = cli.Post("/api/auth/users", req.dump(), "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 409);
    auto body = json::parse(r->body);
    EXPECT_EQ(body["error"], "username_taken");
}

TEST(AdminUsersRest, CreateUserInvalidUsernameReturns400) {
    AdminRestFixture f(nextPort());
    auto cli = f.client();
    json req = {{"username", "has space"}, {"password", "p"}};
    auto r = cli.Post("/api/auth/users", req.dump(), "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400);
    auto body = json::parse(r->body);
    EXPECT_EQ(body["error"], "username_invalid");
}

TEST(AdminUsersRest, GetUserById) {
    AdminRestFixture f(nextPort());
    auto cli = f.client();
    json req = {{"username", "carol"}, {"password", "p"}};
    auto created = json::parse(
        cli.Post("/api/auth/users", req.dump(), "application/json")->body);
    int id = created["id"];
    auto r = cli.Get(("/api/auth/users/" + std::to_string(id)).c_str());
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 200);
    auto body = json::parse(r->body);
    EXPECT_EQ(body["username"], "carol");
}

TEST(AdminUsersRest, GetUserNotFound) {
    AdminRestFixture f(nextPort());
    auto cli = f.client();
    auto r = cli.Get("/api/auth/users/99999");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 404);
}

TEST(AdminUsersRest, UpdateUserChangesRoleAndEmail) {
    AdminRestFixture f(nextPort());
    auto cli = f.client();
    json create = {{"username", "dora"}, {"password", "p"}, {"role", "viewer"}};
    auto created = json::parse(
        cli.Post("/api/auth/users", create.dump(), "application/json")->body);
    int id = created["id"];

    json patch = {{"role", "operator"}, {"email", "dora@example.com"}};
    auto r = cli.Put(("/api/auth/users/" + std::to_string(id)).c_str(),
                     patch.dump(), "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 200);
    auto body = json::parse(r->body);
    EXPECT_EQ(body["role"],  "operator");
    EXPECT_EQ(body["email"], "dora@example.com");
}

TEST(AdminUsersRest, UpdateUserNotFound) {
    AdminRestFixture f(nextPort());
    auto cli = f.client();
    auto r = cli.Put("/api/auth/users/12345",
                     json({{"role", "viewer"}}).dump(),
                     "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 404);
}

TEST(AdminUsersRest, DeleteUserSoftDisables) {
    AdminRestFixture f(nextPort());
    auto cli = f.client();
    auto created = json::parse(
        cli.Post("/api/auth/users",
                 json({{"username", "ed"}, {"password", "p"}}).dump(),
                 "application/json")->body);
    int id = created["id"];

    auto r = cli.Delete(("/api/auth/users/" + std::to_string(id)).c_str());
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 200);

    auto get = cli.Get(("/api/auth/users/" + std::to_string(id)).c_str());
    ASSERT_TRUE(get);
    EXPECT_EQ(get->status, 200);
    auto body = json::parse(get->body);
    EXPECT_TRUE(body["disabled"]);
}

TEST(AdminUsersRest, EnableUserClearsDisabled) {
    AdminRestFixture f(nextPort());
    auto cli = f.client();
    auto created = json::parse(
        cli.Post("/api/auth/users",
                 json({{"username", "fay"}, {"password", "p"}}).dump(),
                 "application/json")->body);
    int id = created["id"];

    EXPECT_EQ(cli.Delete(("/api/auth/users/" + std::to_string(id)).c_str())->status, 200);
    auto r = cli.Post(("/api/auth/users/" + std::to_string(id) + "/enable").c_str(),
                      "", "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 200);

    auto get = cli.Get(("/api/auth/users/" + std::to_string(id)).c_str());
    auto body = json::parse(get->body);
    EXPECT_FALSE(body["disabled"]);
}

TEST(AdminUsersRest, ResetPasswordReturnsPlaintextOnce) {
    AdminRestFixture f(nextPort());
    auto cli = f.client();
    auto created = json::parse(
        cli.Post("/api/auth/users",
                 json({{"username", "greg"}, {"password", "old"}}).dump(),
                 "application/json")->body);
    int id = created["id"];

    auto r = cli.Post(("/api/auth/users/" + std::to_string(id) + "/reset-password").c_str(),
                      "", "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 200);
    auto body = json::parse(r->body);
    ASSERT_TRUE(body.contains("initial_password"));
    EXPECT_FALSE(body["initial_password"].get<std::string>().empty());

    // After reset — must_change_password=true (виден через GET).
    auto get = cli.Get(("/api/auth/users/" + std::to_string(id)).c_str());
    auto u = json::parse(get->body);
    EXPECT_TRUE(u["must_change_password"]);
}

TEST(AdminUsersRest, ResetPasswordUserNotFound) {
    AdminRestFixture f(nextPort());
    auto cli = f.client();
    auto r = cli.Post("/api/auth/users/55555/reset-password", "", "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 404);
}

TEST(AdminUsersRest, EndpointsReturn503WhenAuthNotConfigured) {
    int port = nextPort();
    ChannelManager mgr(nullptr);
    auto api = std::make_unique<ControlApi>(
        port, mgr,
        /*metrics=*/nullptr, LivezOptions{},
        /*gateways=*/nullptr,
        /*auth=*/nullptr);
    api->start();

    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(2, 0);
    for (int i = 0; i < 100; ++i) {
        auto r = cli.Get("/healthz");
        if (r && r->status == 200) break;
        std::this_thread::sleep_for(20ms);
    }

    auto r = cli.Get("/api/auth/users");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 503);

    api->stop();
}

// ── Brute-force lockout admin endpoint (commit 13/24) ────────────────────

TEST(AdminUsersRest, UserListShowsZeroFailedLoginCountForFreshUser) {
    AdminRestFixture f(nextPort());
    auto cli = f.client();
    json create = {{"username", "alice"}, {"password", "p-12345678"}, {"role", "operator"}};
    cli.Post("/api/auth/users", create.dump(), "application/json");

    auto r = cli.Get("/api/auth/users");
    auto body = json::parse(r->body);
    ASSERT_EQ(body["users"].size(), 1u);
    EXPECT_EQ(body["users"][0]["failed_login_count"], 0);
    EXPECT_FALSE(body["users"][0].contains("locked_until"));
}

TEST(AdminUsersRest, FailedLoginsExposeFailedCountInUserListing) {
    AdminRestFixture f(nextPort());
    // Используем небольшой threshold через политику сервиса напрямую —
    // REST не управляет политикой (это конфигурация процесса).
    liveqx::auth::AuthService::LockoutPolicy p;
    p.threshold = 2; p.base_delay_sec = 60;
    f.svc->setLockoutPolicy(p);

    auto cli = f.client();
    json create = {{"username", "alice"}, {"password", "p-correct"}};
    auto cr = cli.Post("/api/auth/users", create.dump(), "application/json");
    ASSERT_TRUE(cr);
    const auto uid = json::parse(cr->body)["id"].get<int>();

    for (int i = 0; i < 3; ++i) {
        json b = {{"username", "alice"}, {"password", "WRONG"}};
        cli.Post("/api/auth/login", b.dump(), "application/json");
    }

    auto r = cli.Get(("/api/auth/users/" + std::to_string(uid)).c_str());
    ASSERT_TRUE(r);
    auto body = json::parse(r->body);
    EXPECT_GE(body["failed_login_count"].get<int>(), 2);
    EXPECT_TRUE(body.contains("locked_until"));
}

TEST(AdminUsersRest, AccountLockedLoginReturns423) {
    AdminRestFixture f(nextPort());
    liveqx::auth::AuthService::LockoutPolicy p;
    p.threshold = 1; p.base_delay_sec = 600;  // долгий lock — не истечёт за тест
    f.svc->setLockoutPolicy(p);

    auto cli = f.client();
    cli.Post("/api/auth/users",
             json({{"username", "alice"}, {"password", "p-correct"}}).dump(),
             "application/json");
    // Первая wrong-попытка → InvalidPassword (401), но также сразу locks
    // (threshold=1).
    auto bad = cli.Post("/api/auth/login",
        json({{"username", "alice"}, {"password", "WRONG"}}).dump(),
        "application/json");
    ASSERT_TRUE(bad);
    EXPECT_EQ(bad->status, 401);

    // Вторая попытка — даже с верным паролем — должна получить 423.
    auto locked = cli.Post("/api/auth/login",
        json({{"username", "alice"}, {"password", "p-correct"}}).dump(),
        "application/json");
    ASSERT_TRUE(locked);
    EXPECT_EQ(locked->status, 423);
    auto body = json::parse(locked->body);
    EXPECT_EQ(body["error"], "account_locked");
}

TEST(AdminUsersRest, UnlockEndpointRestoresLogin) {
    AdminRestFixture f(nextPort());
    liveqx::auth::AuthService::LockoutPolicy p;
    p.threshold = 1; p.base_delay_sec = 3600;
    f.svc->setLockoutPolicy(p);

    auto cli = f.client();
    auto cr = cli.Post("/api/auth/users",
        json({{"username", "alice"}, {"password", "p-correct"}}).dump(),
        "application/json");
    const auto uid = json::parse(cr->body)["id"].get<int>();

    cli.Post("/api/auth/login",
        json({{"username", "alice"}, {"password", "WRONG"}}).dump(),
        "application/json");

    // Проверяем, что login сейчас locked.
    auto locked = cli.Post("/api/auth/login",
        json({{"username", "alice"}, {"password", "p-correct"}}).dump(),
        "application/json");
    EXPECT_EQ(locked->status, 423);

    // Unlock.
    auto unlock = cli.Post(("/api/auth/users/" + std::to_string(uid) + "/unlock").c_str(),
                           "", "application/json");
    ASSERT_TRUE(unlock);
    EXPECT_EQ(unlock->status, 200);
    auto u_body = json::parse(unlock->body);
    EXPECT_EQ(u_body["status"], "unlocked");

    // После unlock — логин снова работает.
    auto ok = cli.Post("/api/auth/login",
        json({{"username", "alice"}, {"password", "p-correct"}}).dump(),
        "application/json");
    ASSERT_TRUE(ok);
    EXPECT_EQ(ok->status, 200);
}

TEST(AdminUsersRest, UnlockUnknownUserReturns404) {
    AdminRestFixture f(nextPort());
    auto cli = f.client();
    auto r = cli.Post("/api/auth/users/9999/unlock", "", "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 404);
}
