// fix22 commit 22/24 — REST интеграционные тесты per-channel ACL
// для local-юзеров.
//
// Endpoints:
//   GET    /api/auth/users/{id}/channels
//   PUT    /api/auth/users/{id}/channels/{cid}      {permission}
//   DELETE /api/auth/users/{id}/channels/{cid}
//   GET    /api/channels/{id}/permissions

#include <chrono>
#include <filesystem>
#include <thread>

#include <unistd.h>

#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include "api/ChannelManager.h"
#include "api/ControlApi.h"
#include "audit/AuditDb.h"
#include "audit/AuditLogger.h"
#include "audit/AuditTypes.h"
#include "auth/AuthDb.h"
#include "auth/AuthService.h"
#include "auth/JwtIssuer.h"
#include "auth/MasterKey.h"
#include "auth/PasswordHasher.h"

namespace {

using nlohmann::json;
using namespace std::chrono_literals;

constexpr int kAclPortBase = 19200;
constexpr const char* kSecret = "0123456789abcdef0123456789abcdef";

int nextPort() {
    static std::atomic<int> n{0};
    static const int base = kAclPortBase + (static_cast<int>(getpid()) % 600) * 5;
    return base + n.fetch_add(1);
}

struct AclRestFixture {
    std::filesystem::path                              dbpath;
    std::filesystem::path                              keypath;
    std::filesystem::path                              audit_dbpath;
    std::filesystem::path                              audit_emerg_path;
    std::unique_ptr<liveqx::auth::AuthDb>      db;
    std::unique_ptr<liveqx::auth::JwtIssuer>   jwt;
    std::unique_ptr<liveqx::auth::AuthService> svc;
    std::unique_ptr<liveqx::auth::MasterKey>   mk;
    std::unique_ptr<liveqx::audit::AuditDb>    audit_db;
    std::unique_ptr<liveqx::audit::AuditLogger> audit_logger;
    ChannelManager                                     manager;
    std::unique_ptr<ControlApi>                        api;
    int                                                port;

    explicit AclRestFixture(int p) : manager(nullptr), port(p) {
        const auto base = std::filesystem::temp_directory_path() /
            ("acl_rest_test_" + std::to_string(p));
        dbpath           = base.string() + ".db";
        keypath          = base.string() + ".key";
        audit_dbpath     = base.string() + ".audit.db";
        audit_emerg_path = base.string() + ".audit-emergency.jsonl";
        std::filesystem::remove(dbpath);
        std::filesystem::remove(keypath);
        std::filesystem::remove(audit_dbpath);
        std::filesystem::remove(audit_emerg_path);

        db  = std::make_unique<liveqx::auth::AuthDb>(dbpath);
        EXPECT_TRUE(db->open());

        jwt = std::make_unique<liveqx::auth::JwtIssuer>(std::string(kSecret));
        svc = std::make_unique<liveqx::auth::AuthService>(
            *db, *jwt,
            [](std::int64_t) {
                return std::vector<liveqx::auth::ChannelGrant>{};
            });

        mk = std::make_unique<liveqx::auth::MasterKey>(keypath.string());
        EXPECT_TRUE(mk->load());
        audit_db = std::make_unique<liveqx::audit::AuditDb>(audit_dbpath, mk.get());
        EXPECT_TRUE(audit_db->open());
        audit_logger = std::make_unique<liveqx::audit::AuditLogger>(
            audit_db.get(), audit_emerg_path);
        audit_logger->start();

        api = std::make_unique<ControlApi>(
            port, manager,
            /*metrics=*/nullptr, LivezOptions{},
            /*gateways=*/nullptr,
            svc.get(),
            /*ldap_repo=*/nullptr,
            /*smtp_repo=*/nullptr,
            /*rbac=*/nullptr,
            /*events=*/nullptr,
            /*preview=*/nullptr,
            /*stress=*/nullptr,
            /*plugins=*/nullptr,
            mk.get(),
            /*mounts=*/nullptr,
            TlsBindings{},
            /*time_repo=*/nullptr,
            /*time_src=*/nullptr,
            /*sntp=*/nullptr,
            audit_logger.get());
        api->start();
        waitListening(port);
    }

    ~AclRestFixture() {
        api->stop();
        api.reset();
        if (audit_logger) audit_logger->stop();
        audit_logger.reset();
        audit_db.reset();
        mk.reset();
        svc.reset();
        jwt.reset();
        db.reset();
        std::error_code ec;
        std::filesystem::remove(dbpath, ec);
        std::filesystem::remove(dbpath.string() + "-wal", ec);
        std::filesystem::remove(dbpath.string() + "-shm", ec);
        std::filesystem::remove(keypath, ec);
        std::filesystem::remove(audit_dbpath, ec);
        std::filesystem::remove(audit_dbpath.string() + "-wal", ec);
        std::filesystem::remove(audit_dbpath.string() + "-shm", ec);
        std::filesystem::remove(audit_emerg_path, ec);
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

    std::int64_t insertLocal(const std::string& name,
                             liveqx::auth::Role role
                             = liveqx::auth::Role::Operator) {
        auto h = liveqx::auth::PasswordHasher::hash("pw-" + name);
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

    std::int64_t insertLdap(const std::string& name) {
        liveqx::auth::User u;
        u.username      = name;
        u.password_hash = "";
        u.role          = liveqx::auth::Role::Viewer;
        u.source        = liveqx::auth::Source::Ldap;
        auto id = db->insertUser(u);
        EXPECT_TRUE(id.has_value());
        return *id;
    }
};

}  // namespace

TEST(ChannelAclRest, GetEmptyForFreshUserReturnsEmpty) {
    AclRestFixture f(nextPort());
    auto cli = f.client();
    auto uid = f.insertLocal("alice");

    auto r = cli.Get(("/api/auth/users/" + std::to_string(uid) + "/channels").c_str());
    ASSERT_TRUE(r);
    ASSERT_EQ(r->status, 200) << r->body;
    auto body = json::parse(r->body);
    ASSERT_TRUE(body["items"].is_array());
    EXPECT_EQ(body["items"].size(), 0u);
}

TEST(ChannelAclRest, GetForUnknownUserReturns404) {
    AclRestFixture f(nextPort());
    auto cli = f.client();
    auto r = cli.Get("/api/auth/users/9999/channels");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 404);
    EXPECT_EQ(json::parse(r->body)["error"], "user_not_found");
}

TEST(ChannelAclRest, PutSetsAndGetReturnsRow) {
    AclRestFixture f(nextPort());
    auto cli = f.client();
    auto uid = f.insertLocal("alice");

    auto r = cli.Put(
        ("/api/auth/users/" + std::to_string(uid) + "/channels/7").c_str(),
        json({{"permission", "operate"}}).dump(),
        "application/json");
    ASSERT_TRUE(r);
    ASSERT_EQ(r->status, 200) << r->body;
    auto body = json::parse(r->body);
    EXPECT_EQ(body["user_id"],    uid);
    EXPECT_EQ(body["channel_id"], 7);
    EXPECT_EQ(body["permission"], "operate");

    auto g = cli.Get(("/api/auth/users/" + std::to_string(uid) + "/channels").c_str());
    ASSERT_TRUE(g);
    auto gb = json::parse(g->body);
    ASSERT_EQ(gb["items"].size(), 1u);
    EXPECT_EQ(gb["items"][0]["channel_id"], 7);
    EXPECT_EQ(gb["items"][0]["permission"], "operate");
    EXPECT_EQ(gb["items"][0]["username"],   "alice");
}

TEST(ChannelAclRest, PutOverwritesPermission) {
    AclRestFixture f(nextPort());
    auto cli = f.client();
    auto uid = f.insertLocal("bob");

    cli.Put(("/api/auth/users/" + std::to_string(uid) + "/channels/12").c_str(),
            json({{"permission","view"}}).dump(), "application/json");
    auto r = cli.Put(
        ("/api/auth/users/" + std::to_string(uid) + "/channels/12").c_str(),
        json({{"permission","operate"}}).dump(),
        "application/json");
    ASSERT_TRUE(r);
    ASSERT_EQ(r->status, 200);

    auto g = cli.Get(("/api/auth/users/" + std::to_string(uid) + "/channels").c_str());
    auto gb = json::parse(g->body);
    ASSERT_EQ(gb["items"].size(), 1u);
    EXPECT_EQ(gb["items"][0]["permission"], "operate");
}

TEST(ChannelAclRest, PutWithoutPermissionFieldReturns400) {
    AclRestFixture f(nextPort());
    auto cli = f.client();
    auto uid = f.insertLocal("carol");
    auto r = cli.Put(
        ("/api/auth/users/" + std::to_string(uid) + "/channels/3").c_str(),
        "{}", "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400);
    EXPECT_EQ(json::parse(r->body)["error"], "missing_permission");
}

TEST(ChannelAclRest, PutInvalidPermissionReturns400) {
    AclRestFixture f(nextPort());
    auto cli = f.client();
    auto uid = f.insertLocal("dave");
    auto r = cli.Put(
        ("/api/auth/users/" + std::to_string(uid) + "/channels/3").c_str(),
        json({{"permission","godmode"}}).dump(),
        "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400);
    EXPECT_EQ(json::parse(r->body)["error"], "invalid_permission");
}

TEST(ChannelAclRest, PutForUnknownUserReturns404) {
    AclRestFixture f(nextPort());
    auto cli = f.client();
    auto r = cli.Put("/api/auth/users/8888/channels/1",
                     json({{"permission","view"}}).dump(),
                     "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 404);
    EXPECT_EQ(json::parse(r->body)["error"], "user_not_found");
}

TEST(ChannelAclRest, PutForLdapUserReturnsSourceMismatch) {
    AclRestFixture f(nextPort());
    auto cli = f.client();
    auto uid = f.insertLdap("ldap-user");
    auto r = cli.Put(
        ("/api/auth/users/" + std::to_string(uid) + "/channels/1").c_str(),
        json({{"permission","view"}}).dump(),
        "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400);
    EXPECT_EQ(json::parse(r->body)["error"], "user_source_mismatch");
}

TEST(ChannelAclRest, DeleteRemovesGrant) {
    AclRestFixture f(nextPort());
    auto cli = f.client();
    auto uid = f.insertLocal("erin");
    cli.Put(("/api/auth/users/" + std::to_string(uid) + "/channels/4").c_str(),
            json({{"permission","operate"}}).dump(), "application/json");

    auto r = cli.Delete(
        ("/api/auth/users/" + std::to_string(uid) + "/channels/4").c_str());
    ASSERT_TRUE(r);
    ASSERT_EQ(r->status, 200) << r->body;
    EXPECT_EQ(json::parse(r->body)["removed"], true);

    auto g = cli.Get(("/api/auth/users/" + std::to_string(uid) + "/channels").c_str());
    EXPECT_EQ(json::parse(g->body)["items"].size(), 0u);
}

TEST(ChannelAclRest, DeleteOfMissingGrantReturns404) {
    AclRestFixture f(nextPort());
    auto cli = f.client();
    auto uid = f.insertLocal("frank");
    auto r = cli.Delete(
        ("/api/auth/users/" + std::to_string(uid) + "/channels/99").c_str());
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 404);
    EXPECT_EQ(json::parse(r->body)["error"], "grant_not_found");
}

TEST(ChannelAclRest, DeleteForLdapUserReturnsSourceMismatch) {
    AclRestFixture f(nextPort());
    auto cli = f.client();
    auto uid = f.insertLdap("ldap-bob");
    auto r = cli.Delete(
        ("/api/auth/users/" + std::to_string(uid) + "/channels/1").c_str());
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400);
    EXPECT_EQ(json::parse(r->body)["error"], "user_source_mismatch");
}

TEST(ChannelAclRest, GetByChannelGroupsAllUsers) {
    AclRestFixture f(nextPort());
    auto cli = f.client();
    auto a_id = f.insertLocal("aaa");
    auto b_id = f.insertLocal("bbb");
    auto c_id = f.insertLocal("ccc");
    cli.Put(("/api/auth/users/" + std::to_string(a_id) + "/channels/77").c_str(),
            json({{"permission","operate"}}).dump(), "application/json");
    cli.Put(("/api/auth/users/" + std::to_string(b_id) + "/channels/77").c_str(),
            json({{"permission","view"}}).dump(),    "application/json");
    cli.Put(("/api/auth/users/" + std::to_string(c_id) + "/channels/88").c_str(),
            json({{"permission","operate"}}).dump(), "application/json");

    auto r = cli.Get("/api/channels/77/permissions");
    ASSERT_TRUE(r);
    ASSERT_EQ(r->status, 200) << r->body;
    auto body = json::parse(r->body);
    EXPECT_EQ(body["channel_id"], 77);
    ASSERT_EQ(body["items"].size(), 2u);
    // ORDER BY username alphabetical → aaa, bbb.
    EXPECT_EQ(body["items"][0]["username"], "aaa");
    EXPECT_EQ(body["items"][0]["permission"], "operate");
    EXPECT_EQ(body["items"][1]["username"], "bbb");
    EXPECT_EQ(body["items"][1]["permission"], "view");
}

TEST(ChannelAclRest, GetByChannelEmptyChannelReturnsEmptyList) {
    AclRestFixture f(nextPort());
    auto cli = f.client();
    auto r = cli.Get("/api/channels/123/permissions");
    ASSERT_TRUE(r);
    ASSERT_EQ(r->status, 200);
    auto body = json::parse(r->body);
    EXPECT_EQ(body["channel_id"], 123);
    EXPECT_EQ(body["items"].size(), 0u);
}

TEST(ChannelAclRest, AuditEventsForGrantSetAndRemoved) {
    AclRestFixture f(nextPort());
    auto cli = f.client();
    auto uid = f.insertLocal("audited");
    cli.Put(("/api/auth/users/" + std::to_string(uid) + "/channels/55").c_str(),
            json({{"permission","operate"}}).dump(), "application/json");
    cli.Delete(("/api/auth/users/" + std::to_string(uid) + "/channels/55").c_str());

    f.audit_logger->flushForTesting();
    liveqx::audit::AuditFilter af;
    af.action = "admin.user.channel_grant_set";
    af.limit  = 16;
    auto set_events = f.audit_db->list(af);
    EXPECT_FALSE(set_events.empty());

    af.action = "admin.user.channel_grant_removed";
    auto rm_events = f.audit_db->list(af);
    EXPECT_FALSE(rm_events.empty());
}

// Bonus: integration — login через AuthService, у которого resolver ходит в
// AuthDb::listChannelGrantsForUser. Подтверждает, что channel_grants реально
// доезжают до JWT. Pre-commit-24 (где нет main wiring'а) — это единственное
// место в codebase'е, где видно конец полного цикла.
TEST(ChannelAclRest, GrantsFlowIntoJwtViaResolver) {
    namespace sa = liveqx::auth;
    const int port = nextPort();
    auto dbpath = std::filesystem::temp_directory_path() /
        ("acl_resolver_" + std::to_string(port) + ".db");
    std::filesystem::remove(dbpath);
    auto db = std::make_unique<sa::AuthDb>(dbpath);
    ASSERT_TRUE(db->open());
    sa::JwtIssuer jwt{std::string(kSecret)};
    auto& dbref = *db;
    sa::AuthService::GrantsResolver resolver = [&dbref](std::int64_t id) {
        return dbref.listChannelGrantsForUser(id);
    };
    sa::AuthService svc(*db, jwt, resolver);

    // Юзер + grant.
    sa::User u;
    u.username      = "jwt-user";
    auto h = sa::PasswordHasher::hash("pw");
    ASSERT_TRUE(h.has_value());
    u.password_hash = *h;
    u.role          = sa::Role::Operator;
    u.source        = sa::Source::Local;
    auto uid = db->insertUser(u);
    ASSERT_TRUE(uid.has_value());
    db->setChannelPermission(*uid, 17, sa::ChannelPermission::Operate, *uid, 1);
    db->setChannelPermission(*uid, 33, sa::ChannelPermission::View,    *uid, 1);

    auto login = svc.login("jwt-user", "pw", "127.0.0.1", "ut");
    auto* tp = std::get_if<sa::JwtIssuer::TokenPair>(&login.outcome);
    ASSERT_NE(tp, nullptr) << "login failed";

    auto claims = svc.verifyActiveAccess(tp->access_token);
    ASSERT_TRUE(claims.has_value());
    ASSERT_EQ(claims->channel_grants.size(), 2u);
    // Sorted by AuthDb (channel_id ASC).
    EXPECT_EQ(claims->channel_grants[0].channel_id, 17);
    EXPECT_EQ(claims->channel_grants[0].permission, sa::ChannelPermission::Operate);
    EXPECT_EQ(claims->channel_grants[1].channel_id, 33);
    EXPECT_EQ(claims->channel_grants[1].permission, sa::ChannelPermission::View);

    db.reset();
    std::error_code ec;
    std::filesystem::remove(dbpath, ec);
    std::filesystem::remove(dbpath.string() + "-wal", ec);
    std::filesystem::remove(dbpath.string() + "-shm", ec);
}
