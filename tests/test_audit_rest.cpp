// fix22 commit 12/24 — REST интеграционный тест для audit-эндпоинтов.
//
// GET  /api/auth/audit?event=login.ok&limit=...
// POST /api/auth/audit/purge?older_than_days=N

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
#include "auth/PasswordHasher.h"

namespace {

using nlohmann::json;
using namespace std::chrono_literals;

// Сдвигаем порты по PID — параллельный ctest запускает каждый тест в
// отдельном процессе, иначе соседние jobs дерутся за один порт.
constexpr int kAuditPortBase = 18760;
constexpr const char* kSecret = "0123456789abcdef0123456789abcdef";

int nextPort() {
    static std::atomic<int> n{0};
    static const int base = kAuditPortBase + (static_cast<int>(getpid()) % 800) * 5;
    return base + n.fetch_add(1);
}

struct AuditRestFixture {
    std::filesystem::path                              dbpath;
    std::unique_ptr<liveqx::auth::AuthDb>      db;
    std::unique_ptr<liveqx::auth::JwtIssuer>   jwt;
    std::unique_ptr<liveqx::auth::AuthService> svc;
    ChannelManager                                     manager;
    std::unique_ptr<ControlApi>                        api;
    int                                                port;

    explicit AuditRestFixture(int p) : manager(nullptr), port(p) {
        dbpath = std::filesystem::temp_directory_path() /
            ("audit_rest_test_" + std::to_string(p) + ".db");
        std::filesystem::remove(dbpath);

        db  = std::make_unique<liveqx::auth::AuthDb>(dbpath);
        EXPECT_TRUE(db->open());
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

    ~AuditRestFixture() {
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

    // Создаёт юзера в БД, возвращает id.
    std::int64_t insertUser(const std::string& name,
                            const std::string& password,
                            liveqx::auth::Role role
                                = liveqx::auth::Role::Operator) {
        auto h = liveqx::auth::PasswordHasher::hash(password);
        EXPECT_TRUE(h.has_value());
        liveqx::auth::User u;
        u.username      = name;
        u.password_hash = *h;
        u.role          = role;
        u.source        = liveqx::auth::Source::Local;
        auto id = db->insertUser(u);
        EXPECT_TRUE(id.has_value());
        return *id;
    }
};

}  // namespace

TEST(AuditRest, ListEmptyByDefault) {
    AuditRestFixture f(nextPort());
    auto cli = f.client();
    auto r = cli.Get("/api/auth/audit");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 200);
    auto body = json::parse(r->body);
    EXPECT_TRUE(body["events"].is_array());
    EXPECT_EQ(body["events"].size(), 0u);
}

TEST(AuditRest, LoginEmitsAuditEventVisibleViaGet) {
    AuditRestFixture f(nextPort());
    f.insertUser("alice", "p-12345678");

    auto cli = f.client();
    json login = {{"username", "alice"}, {"password", "p-12345678"}};
    auto rl = cli.Post("/api/auth/login", login.dump(), "application/json");
    ASSERT_TRUE(rl);
    EXPECT_EQ(rl->status, 200);

    auto r = cli.Get("/api/auth/audit?event=login.ok");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 200);
    auto body = json::parse(r->body);
    ASSERT_EQ(body["events"].size(), 1u);
    const auto& e = body["events"][0];
    EXPECT_EQ(e["event"], "login.ok");
    EXPECT_EQ(e["username"], "alice");
    // details должны быть JSON-объектом, не raw-строкой.
    ASSERT_TRUE(e.contains("details"));
    EXPECT_TRUE(e["details"].contains("jti"));
}

TEST(AuditRest, FailedLoginEmitsLoginFailEvent) {
    AuditRestFixture f(nextPort());
    f.insertUser("alice", "p-correct");

    auto cli = f.client();
    json bad = {{"username", "alice"}, {"password", "WRONG"}};
    auto rl = cli.Post("/api/auth/login", bad.dump(), "application/json");
    ASSERT_TRUE(rl);
    EXPECT_EQ(rl->status, 401);

    auto r = cli.Get("/api/auth/audit?event=login.fail");
    ASSERT_TRUE(r);
    auto body = json::parse(r->body);
    ASSERT_EQ(body["events"].size(), 1u);
    EXPECT_EQ(body["events"][0]["details"]["reason"], "invalid_password");
}

TEST(AuditRest, FilterByUserIdNarrowsResults) {
    AuditRestFixture f(nextPort());
    auto uid_a = f.insertUser("alice", "p-12345678");
    f.insertUser("bob",   "p-12345678");

    auto cli = f.client();
    auto loginAs = [&](const std::string& u, const std::string& p) {
        json b = {{"username", u}, {"password", p}};
        cli.Post("/api/auth/login", b.dump(), "application/json");
    };
    loginAs("alice", "p-12345678");
    loginAs("bob",   "p-12345678");
    loginAs("alice", "WRONG");  // login.fail для alice

    auto r = cli.Get(("/api/auth/audit?user_id=" + std::to_string(uid_a)).c_str());
    ASSERT_TRUE(r);
    auto body = json::parse(r->body);
    // Должны быть только события alice (login.ok + login.fail = 2).
    EXPECT_EQ(body["events"].size(), 2u);
    for (const auto& e : body["events"]) EXPECT_EQ(e["user_id"], uid_a);
}

TEST(AuditRest, LimitAndOffsetPaginate) {
    AuditRestFixture f(nextPort());
    f.insertUser("alice", "p-12345678");
    auto cli = f.client();
    for (int i = 0; i < 5; ++i) {
        json b = {{"username", "alice"}, {"password", "WRONG"}};
        cli.Post("/api/auth/login", b.dump(), "application/json");
    }

    auto r1 = cli.Get("/api/auth/audit?limit=2&offset=0");
    auto r2 = cli.Get("/api/auth/audit?limit=2&offset=2");
    ASSERT_TRUE(r1); ASSERT_TRUE(r2);
    auto b1 = json::parse(r1->body);
    auto b2 = json::parse(r2->body);
    EXPECT_EQ(b1["events"].size(), 2u);
    EXPECT_EQ(b2["events"].size(), 2u);
    EXPECT_NE(b1["events"][0]["id"], b2["events"][0]["id"]);
}

TEST(AuditRest, BadLimitReturns400) {
    AuditRestFixture f(nextPort());
    auto cli = f.client();
    auto r = cli.Get("/api/auth/audit?limit=NaN");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400);
}

TEST(AuditRest, PurgeRemovesOlderEntries) {
    AuditRestFixture f(nextPort());
    // Вставим напрямую через AuthDb — события датированы прошлым.
    liveqx::auth::AuditEvent old1; old1.ts = 100; old1.event = "x";
    liveqx::auth::AuditEvent old2; old2.ts = 200; old2.event = "x";
    ASSERT_TRUE(f.db->insertAuditEvent(old1));
    ASSERT_TRUE(f.db->insertAuditEvent(old2));

    // Свежее событие — через login (now ts).
    f.insertUser("alice", "p-12345678");
    auto cli = f.client();
    json b = {{"username", "alice"}, {"password", "p-12345678"}};
    cli.Post("/api/auth/login", b.dump(), "application/json");

    auto r = cli.Post("/api/auth/audit/purge?older_than_days=1", "", "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 200);
    auto body = json::parse(r->body);
    EXPECT_GE(body["removed"].get<int>(), 2);

    auto r2 = cli.Get("/api/auth/audit?event=x");
    auto b2 = json::parse(r2->body);
    EXPECT_EQ(b2["events"].size(), 0u);
}

TEST(AuditRest, PurgeRequiresOlderThanDays) {
    AuditRestFixture f(nextPort());
    auto cli = f.client();
    auto r = cli.Post("/api/auth/audit/purge", "", "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400);
}

TEST(AuditRest, UnconfiguredAuthReturns503) {
    // AuditRestFixture всегда конфигурирует auth — вместо этого создадим
    // ControlApi напрямую без AuthService.
    const int port = nextPort();
    ChannelManager mgr(nullptr);
    auto api = std::make_unique<ControlApi>(
        port, mgr, /*metrics=*/nullptr, LivezOptions{},
        /*gateways=*/nullptr, /*auth=*/nullptr);
    api->start();
    AuditRestFixture::waitListening(port);

    httplib::Client c("127.0.0.1", port);
    c.set_read_timeout(5, 0);
    auto r = c.Get("/api/auth/audit");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 503);
    api->stop();
}
