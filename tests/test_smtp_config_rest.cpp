// fix22 commit 23/24 — REST integration tests для SMTP config + test endpoint.
//
// GET  /api/auth/smtp/config
// PUT  /api/auth/smtp/config
// POST /api/auth/smtp/test       {to, ...override-fields}
//
// SMTP send против реального сервера в этих тестах не делается.
// Test endpoint бьёт в 127.0.0.1:39999 (заведомо никем не слушается),
// получая быстрый CONNECTION REFUSED — так проверяется ошибочный путь
// без сетевой ненадёжности.

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
#include "auth/SmtpConfigRepo.h"

namespace {

using nlohmann::json;
using namespace std::chrono_literals;

constexpr int kSmtpCfgPortBase = 19500;
constexpr const char* kSecret = "0123456789abcdef0123456789abcdef";

int nextPort() {
    static std::atomic<int> n{0};
    static const int base = kSmtpCfgPortBase + (static_cast<int>(getpid()) % 600) * 5;
    return base + n.fetch_add(1);
}

struct SmtpCfgRestFixture {
    std::filesystem::path                                  dbpath;
    std::filesystem::path                                  keypath;
    std::filesystem::path                                  audit_dbpath;
    std::filesystem::path                                  audit_emerg_path;
    std::unique_ptr<liveqx::auth::AuthDb>          db;
    std::unique_ptr<liveqx::auth::JwtIssuer>       jwt;
    std::unique_ptr<liveqx::auth::AuthService>     svc;
    std::unique_ptr<liveqx::auth::MasterKey>       mk;
    std::unique_ptr<liveqx::auth::SmtpConfigRepo>  repo;
    std::unique_ptr<liveqx::audit::AuditDb>        audit_db;
    std::unique_ptr<liveqx::audit::AuditLogger>    audit_logger;
    ChannelManager                                         manager;
    std::unique_ptr<ControlApi>                            api;
    int                                                    port;

    explicit SmtpCfgRestFixture(int p) : manager(nullptr), port(p) {
        const auto base = std::filesystem::temp_directory_path() /
            ("smtp_cfg_rest_" + std::to_string(p));
        dbpath          = base.string() + ".db";
        keypath         = base.string() + ".key";
        audit_dbpath    = base.string() + ".audit.db";
        audit_emerg_path= base.string() + ".audit-emergency.jsonl";
        std::filesystem::remove(dbpath);
        std::filesystem::remove(keypath);
        std::filesystem::remove(audit_dbpath);
        std::filesystem::remove(audit_emerg_path);

        db  = std::make_unique<liveqx::auth::AuthDb>(dbpath);
        EXPECT_TRUE(db->open());
        jwt = std::make_unique<liveqx::auth::JwtIssuer>(std::string(kSecret));
        svc = std::make_unique<liveqx::auth::AuthService>(
            *db, *jwt,
            [](std::int64_t) { return std::vector<liveqx::auth::ChannelGrant>{}; });
        mk  = std::make_unique<liveqx::auth::MasterKey>(keypath.string());
        EXPECT_TRUE(mk->load());
        repo = std::make_unique<liveqx::auth::SmtpConfigRepo>(*db, *mk);

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
            repo.get(),
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

    ~SmtpCfgRestFixture() {
        api->stop();
        api.reset();
        if (audit_logger) audit_logger->stop();
        audit_logger.reset();
        audit_db.reset();
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
        c.set_read_timeout(15, 0);
        return c;
    }
};

}  // namespace

TEST(SmtpConfigRest, GetEmptyReturnsDefaults) {
    SmtpCfgRestFixture f(nextPort());
    auto cli = f.client();
    auto r = cli.Get("/api/auth/smtp/config");
    ASSERT_TRUE(r);
    ASSERT_EQ(r->status, 200) << r->body;
    auto body = json::parse(r->body);
    EXPECT_EQ(body["enabled"], false);
    EXPECT_EQ(body["server"],  "");
    EXPECT_EQ(body["password_set"], false);
    EXPECT_EQ(body["security"], "starttls");
}

TEST(SmtpConfigRest, PutWithMinimalConfigPersists) {
    SmtpCfgRestFixture f(nextPort());
    auto cli = f.client();
    auto r = cli.Put("/api/auth/smtp/config",
        json({{"server",     "smtp.corp.local"},
              {"port",       587},
              {"security",   "starttls"},
              {"from_email", "noreply@corp.local"},
              {"from_name",  "SC Bot"}}).dump(),
        "application/json");
    ASSERT_TRUE(r);
    ASSERT_EQ(r->status, 200) << r->body;
    auto body = json::parse(r->body);
    EXPECT_EQ(body["server"],     "smtp.corp.local");
    EXPECT_EQ(body["from_email"], "noreply@corp.local");
    EXPECT_EQ(body["from_name"],  "SC Bot");
    EXPECT_EQ(body["password_set"], false);

    // Round-trip GET.
    auto g = cli.Get("/api/auth/smtp/config");
    auto gb = json::parse(g->body);
    EXPECT_EQ(gb["server"], "smtp.corp.local");
}

TEST(SmtpConfigRest, PutPasswordIsWriteOnlyButPersists) {
    SmtpCfgRestFixture f(nextPort());
    auto cli = f.client();
    auto r = cli.Put("/api/auth/smtp/config",
        json({{"server",     "smtp.corp.local"},
              {"username",   "noreply"},
              {"password",   "very-secret"},
              {"from_email", "noreply@corp.local"}}).dump(),
        "application/json");
    ASSERT_TRUE(r);
    ASSERT_EQ(r->status, 200) << r->body;
    auto body = json::parse(r->body);
    EXPECT_EQ(body["password_set"], true);
    // GET тоже не возвращает пароль (write-only) — флаг password_set
    // подтверждает, что он сохранён.
    EXPECT_FALSE(body.contains("password"));
}

TEST(SmtpConfigRest, PutPreservesPasswordWhenFieldOmitted) {
    SmtpCfgRestFixture f(nextPort());
    auto cli = f.client();
    cli.Put("/api/auth/smtp/config",
        json({{"server","smtp.corp.local"},{"username","u"},
              {"password","keep-me"},
              {"from_email","x@y.z"}}).dump(),
        "application/json");
    auto r = cli.Put("/api/auth/smtp/config",
        json({{"server","smtp.corp.local"},
              {"from_email","x@y.z"},
              {"from_name","New Name"}}).dump(),
        "application/json");
    ASSERT_TRUE(r);
    ASSERT_EQ(r->status, 200);
    auto body = json::parse(r->body);
    EXPECT_EQ(body["from_name"], "New Name");
    EXPECT_EQ(body["password_set"], true);
}

TEST(SmtpConfigRest, PutEmptyPasswordClearsIt) {
    SmtpCfgRestFixture f(nextPort());
    auto cli = f.client();
    cli.Put("/api/auth/smtp/config",
        json({{"server","smtp.corp.local"},{"username","u"},
              {"password","secret"},{"from_email","x@y.z"}}).dump(),
        "application/json");
    auto r = cli.Put("/api/auth/smtp/config",
        json({{"username",""},{"password",""}}).dump(),
        "application/json");
    ASSERT_TRUE(r);
    auto body = json::parse(r->body);
    EXPECT_EQ(body["password_set"], false);
}

TEST(SmtpConfigRest, PutEnabledRequiresValidConfig) {
    SmtpCfgRestFixture f(nextPort());
    auto cli = f.client();
    auto r = cli.Put("/api/auth/smtp/config",
        json({{"enabled", true},
              {"server",  "smtp.corp.local"}
              // from_email отсутствует → validate должен отказать.
              }).dump(),
        "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400);
    EXPECT_EQ(json::parse(r->body)["error"], "invalid_config");
}

TEST(SmtpConfigRest, PutInvalidSecurityReturns400) {
    SmtpCfgRestFixture f(nextPort());
    auto cli = f.client();
    auto r = cli.Put("/api/auth/smtp/config",
        json({{"security", "rotorua"}}).dump(),
        "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400);
    EXPECT_EQ(json::parse(r->body)["error"], "invalid_security");
}

TEST(SmtpConfigRest, PutPortOutOfRangeReturns400) {
    SmtpCfgRestFixture f(nextPort());
    auto cli = f.client();
    auto r = cli.Put("/api/auth/smtp/config",
        json({{"port", 999999}}).dump(),
        "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400);
    EXPECT_EQ(json::parse(r->body)["error"], "port_out_of_range");
}

TEST(SmtpConfigRest, TestRejectsMissingTo) {
    SmtpCfgRestFixture f(nextPort());
    auto cli = f.client();
    auto r = cli.Post("/api/auth/smtp/test", "{}", "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400);
    EXPECT_EQ(json::parse(r->body)["error"], "missing_to");
}

TEST(SmtpConfigRest, TestRejectsBadEmailRecipient) {
    SmtpCfgRestFixture f(nextPort());
    auto cli = f.client();
    auto r = cli.Post("/api/auth/smtp/test",
        json({{"to","not-an-email"},
              {"server","127.0.0.1"},
              {"port",39999},
              {"security","none"},
              {"from_email","x@y.z"}}).dump(),
        "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400);
    EXPECT_EQ(json::parse(r->body)["error"], "invalid_to");
}

TEST(SmtpConfigRest, TestRejectsInvalidConfig) {
    SmtpCfgRestFixture f(nextPort());
    auto cli = f.client();
    // server omitted → invalid_config.
    auto r = cli.Post("/api/auth/smtp/test",
        json({{"to","admin@x.test"},
              {"security","none"},
              {"from_email","x@y.z"}}).dump(),
        "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400);
    EXPECT_EQ(json::parse(r->body)["error"], "invalid_config");
}

TEST(SmtpConfigRest, TestReportsConnectionRefused) {
    SmtpCfgRestFixture f(nextPort());
    auto cli = f.client();
    // 127.0.0.1:39999 свободен → ECONNREFUSED.
    auto r = cli.Post("/api/auth/smtp/test",
        json({{"to","admin@x.test"},
              {"server","127.0.0.1"},
              {"port",39999},
              {"security","none"},
              {"from_email","x@y.z"},
              {"timeout_sec",2},
              {"use_saved",false}}).dump(),
        "application/json");
    ASSERT_TRUE(r);
    ASSERT_EQ(r->status, 200) << r->body;
    auto body = json::parse(r->body);
    EXPECT_EQ(body["ok"], false);
    EXPECT_TRUE(body.contains("error"));
    EXPECT_TRUE(body.contains("latency_ms"));
}

TEST(SmtpConfigRest, AuditEventsForConfigUpdateAndTest) {
    SmtpCfgRestFixture f(nextPort());
    auto cli = f.client();
    cli.Put("/api/auth/smtp/config",
        json({{"server","smtp.corp.local"},
              {"from_email","x@y.z"}}).dump(),
        "application/json");
    cli.Post("/api/auth/smtp/test",
        json({{"to","admin@x.test"},
              {"server","127.0.0.1"},
              {"port",39999},
              {"security","none"},
              {"from_email","x@y.z"},
              {"timeout_sec",2},
              {"use_saved",false}}).dump(),
        "application/json");

    f.audit_logger->flushForTesting();
    liveqx::audit::AuditFilter af;
    af.action = "admin.smtp.config_updated";
    af.limit  = 16;
    auto upd = f.audit_db->list(af);
    EXPECT_FALSE(upd.empty());

    af.action = "admin.smtp.test";
    auto tst = f.audit_db->list(af);
    EXPECT_FALSE(tst.empty());
}
