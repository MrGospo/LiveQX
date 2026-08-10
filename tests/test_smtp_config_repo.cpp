// fix22 commit 23/24 — SmtpConfigRepo unit tests.
//
// Покрытие:
//   1. Empty load — нет smtp_config row → load() == nullopt.
//   2. Save+Load roundtrip без password → password остаётся пустым.
//   3. Save+Load roundtrip с password → расшифровка возвращает оригинал.
//   4. Save без master key с непустым password → отказ (refuse plaintext).
//   5. securityFromString / securityToString корректно округляют все 3
//      варианта; неизвестная строка → nullopt.

#include <filesystem>

#include <gtest/gtest.h>

#include "auth/AuthDb.h"
#include "auth/MasterKey.h"
#include "auth/SmtpConfig.h"
#include "auth/SmtpConfigRepo.h"

namespace sa = liveqx::auth;

namespace {

struct Fx {
    std::filesystem::path                 dbpath;
    std::filesystem::path                 keypath;
    sa::AuthDb                            db;
    sa::MasterKey                         mk;
    sa::SmtpConfigRepo                    repo;

    Fx() : dbpath(std::filesystem::temp_directory_path() /
                  ("smtp_repo_test_" + std::to_string(::getpid()) + "_" +
                   std::to_string(reinterpret_cast<std::uintptr_t>(this)) + ".db")),
           keypath(dbpath.string() + ".key"),
           db(dbpath),
           mk(keypath.string()),
           repo(db, mk) {
        std::filesystem::remove(dbpath);
        std::filesystem::remove(keypath);
        EXPECT_TRUE(db.open());
    }
    ~Fx() {
        std::error_code ec;
        std::filesystem::remove(dbpath, ec);
        std::filesystem::remove(dbpath.string() + "-wal", ec);
        std::filesystem::remove(dbpath.string() + "-shm", ec);
        std::filesystem::remove(keypath, ec);
    }

    void loadKey() {
        // MasterKey::load() сам auto-generate'ит файл ключа, если ENV не
        // задано и файла нет. Этого достаточно для теста roundtrip'а:
        // ключ сохраняется в keypath, инстанс владеет им.
        ASSERT_TRUE(mk.load());
    }
};

}  // namespace

TEST(SmtpConfigRepo, EmptyLoadReturnsNullopt) {
    Fx f;
    auto cur = f.repo.load();
    EXPECT_FALSE(cur.has_value());
}

TEST(SmtpConfigRepo, SecurityToStringRoundtrip) {
    EXPECT_EQ(sa::SmtpConfigRepo::securityToString(sa::SmtpSecurity::None),     "none");
    EXPECT_EQ(sa::SmtpConfigRepo::securityToString(sa::SmtpSecurity::StartTls), "starttls");
    EXPECT_EQ(sa::SmtpConfigRepo::securityToString(sa::SmtpSecurity::Tls),      "tls");

    EXPECT_EQ(sa::SmtpConfigRepo::securityFromString("none"),     sa::SmtpSecurity::None);
    EXPECT_EQ(sa::SmtpConfigRepo::securityFromString("starttls"), sa::SmtpSecurity::StartTls);
    EXPECT_EQ(sa::SmtpConfigRepo::securityFromString("tls"),      sa::SmtpSecurity::Tls);
    EXPECT_FALSE(sa::SmtpConfigRepo::securityFromString("bogus").has_value());
}

TEST(SmtpConfigRepo, SaveLoadWithoutPassword) {
    Fx f;
    sa::SmtpConfig cfg;
    cfg.enabled     = true;
    cfg.server      = "smtp.corp.local";
    cfg.port        = 25;
    cfg.security    = sa::SmtpSecurity::None;
    cfg.from_email  = "noreply@corp.local";
    cfg.from_name   = "SC";
    cfg.timeout_sec = 30;
    EXPECT_TRUE(f.repo.save(cfg, std::nullopt));

    auto cur = f.repo.load();
    ASSERT_TRUE(cur.has_value());
    EXPECT_TRUE  (cur->enabled);
    EXPECT_EQ    (cur->server, "smtp.corp.local");
    EXPECT_EQ    (cur->port, 25);
    EXPECT_EQ    (cur->security, sa::SmtpSecurity::None);
    EXPECT_EQ    (cur->from_email, "noreply@corp.local");
    EXPECT_EQ    (cur->from_name,  "SC");
    EXPECT_EQ    (cur->timeout_sec, 30);
    EXPECT_TRUE  (cur->password.empty());
}

TEST(SmtpConfigRepo, SaveLoadWithPasswordRoundtrip) {
    Fx f;
    f.loadKey();

    sa::SmtpConfig cfg;
    cfg.server     = "smtp.example.com";
    cfg.security   = sa::SmtpSecurity::StartTls;
    cfg.from_email = "noreply@example.com";
    cfg.username   = "noreply";
    cfg.password   = "s3cret-pw!";
    EXPECT_TRUE(f.repo.save(cfg, std::nullopt));

    auto cur = f.repo.load();
    ASSERT_TRUE(cur.has_value());
    EXPECT_EQ(cur->username, "noreply");
    EXPECT_EQ(cur->password, "s3cret-pw!");
}

TEST(SmtpConfigRepo, SaveWithoutMasterKeyButWithPasswordRefuses) {
    Fx f;  // mk не загружен → loaded()==false.
    sa::SmtpConfig cfg;
    cfg.server     = "smtp.example.com";
    cfg.from_email = "x@y.z";
    cfg.password   = "must-encrypt";
    EXPECT_FALSE(f.repo.save(cfg, std::nullopt));
}
