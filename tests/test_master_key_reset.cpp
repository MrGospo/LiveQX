// fix35 (ROADMAP_1 A3.14) — unit tests for MasterKeyReset.
//
// Four cases per the roadmap:
//   1. ResetWithoutConfirmReturnsNoConfirmation
//   2. ResetSucceedsWhenServiceStopped — backup created, new key differs
//   3. ResetRefusesWhenAuthDbLocked    — concurrent BEGIN EXCLUSIVE blocks us
//   4. ResetWipesEncryptedSecrets      — pre-populated ciphertext gets NULL'd
//
// Each test runs in an isolated tmp directory; we drive Paths via CLI
// `Args` so resolution is deterministic (no FHS probing, no ENV).

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include "auth/AuthDb.h"
#include "auth/MasterKey.h"
#include "auth/MasterKeyReset.h"
#include "utils/Paths.h"

namespace fs = std::filesystem;
namespace sa = liveqx::auth;
namespace scp = liveqx::paths;

namespace {

class MasterKeyResetTest : public ::testing::Test {
protected:
    fs::path root_;

    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        root_ = fs::temp_directory_path() /
                ("master_key_reset_" + std::string(info->name()) + "_" +
                 std::to_string(::getpid()));
        std::error_code ec;
        fs::remove_all(root_, ec);
        fs::create_directories(root_ / "state");
        fs::create_directories(root_ / "logs");
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(root_, ec);
    }

    scp::Paths makePaths() {
        scp::Args a;
        a.state_dir = (root_ / "state").string();
        a.log_dir   = (root_ / "logs").string();
        a.plugin_dir = (root_ / "plugins").string();
        // FHS probe must NOT win even if /var/lib/liveqx exists
        // on the build host. Use a stub Env that always says "not found".
        scp::Paths::Env env;
        env.get_env = [](const char*) { return std::optional<std::string>{}; };
        env.exists  = [](const fs::path&) { return false; };
        return scp::Paths::resolve(a, nlohmann::json::object(), env);
    }

    std::vector<std::uint8_t> readFileBytes(const fs::path& p) {
        std::ifstream f(p, std::ios::binary);
        return {std::istreambuf_iterator<char>(f),
                std::istreambuf_iterator<char>()};
    }

    // Create a master.key file with a deterministic 32-byte payload so
    // tests can prove "the new key is different from the old one".
    void writeKey(const fs::path& p, std::uint8_t fill) {
        std::array<std::uint8_t, sa::MasterKey::kKeyBytes> bytes{};
        bytes.fill(fill);
        std::ofstream f(p, std::ios::binary);
        f.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }
};

}  // namespace

TEST_F(MasterKeyResetTest, ResetWithoutConfirmReturnsNoConfirmation) {
    const auto paths = makePaths();
    writeKey(paths.masterKey(), 0xAA);

    const auto report = sa::resetMasterKey(paths, /*confirmed=*/false);
    EXPECT_EQ(report.result, sa::ResetResult::NoConfirmation);

    // Key file must NOT have been touched.
    const auto bytes = readFileBytes(paths.masterKey());
    ASSERT_EQ(bytes.size(), sa::MasterKey::kKeyBytes);
    EXPECT_EQ(bytes[0], 0xAA);

    // No .bak should have been written.
    bool any_bak = false;
    for (const auto& e : fs::directory_iterator(paths.stateDir())) {
        if (e.path().filename().string().find(".bak") != std::string::npos)
            any_bak = true;
    }
    EXPECT_FALSE(any_bak);
}

TEST_F(MasterKeyResetTest, ResetSucceedsWhenServiceStopped) {
    const auto paths = makePaths();

    // Pre-existing master.key with deterministic content.
    writeKey(paths.masterKey(), 0xAA);
    const auto old = readFileBytes(paths.masterKey());

    // Materialise auth.db so probeServiceState has something to lock on.
    {
        sa::AuthDb db(paths.authDb());
        ASSERT_TRUE(db.open());
    }

    const auto report = sa::resetMasterKey(paths, /*confirmed=*/true);
    EXPECT_EQ(report.result, sa::ResetResult::Success) << report.error;

    // .bak file is recorded and exists on disk with the OLD bytes.
    ASSERT_FALSE(report.backup_path.empty());
    EXPECT_TRUE(fs::exists(report.backup_path));
    EXPECT_EQ(readFileBytes(report.backup_path), old);

    // master.key still exists, has the right size, but differs from old.
    const auto fresh = readFileBytes(paths.masterKey());
    ASSERT_EQ(fresh.size(), sa::MasterKey::kKeyBytes);
    EXPECT_NE(fresh, old);

    // mode 0600.
    struct stat st{};
    ASSERT_EQ(::stat(paths.masterKey().c_str(), &st), 0);
    EXPECT_EQ(st.st_mode & 0777, 0600);

    // audit log line written.
    const auto audit = paths.logDir() / "audit.log";
    ASSERT_TRUE(fs::exists(audit));
    std::ifstream af(audit);
    std::string line;
    std::getline(af, line);
    EXPECT_NE(line.find("event=master_key_reset"), std::string::npos);
}

TEST_F(MasterKeyResetTest, ResetRefusesWhenServiceRunning) {
    const auto paths = makePaths();
    writeKey(paths.masterKey(), 0xAA);

    // Simulate a running daemon by holding an exclusive flock on the
    // single-instance lockfile that main.cpp acquires at startup. The
    // probe inside resetMasterKey() must see EWOULDBLOCK and refuse.
    const auto lockfile = paths.stateDir() / "liveqx.lock";
    const int fd = ::open(lockfile.c_str(),
                          O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    ASSERT_GE(fd, 0) << "open lockfile: " << lockfile.string();
    ASSERT_EQ(::flock(fd, LOCK_EX | LOCK_NB), 0)
        << "could not pre-lock " << lockfile.string();

    const auto report = sa::resetMasterKey(paths, /*confirmed=*/true);
    EXPECT_EQ(report.result, sa::ResetResult::ServiceRunning);
    EXPECT_NE(report.error.find("running"), std::string::npos);

    // Master key must NOT have been replaced while the service held the lock.
    const auto bytes = readFileBytes(paths.masterKey());
    ASSERT_EQ(bytes.size(), sa::MasterKey::kKeyBytes);
    EXPECT_EQ(bytes[0], 0xAA);

    // Release.
    ::flock(fd, LOCK_UN);
    ::close(fd);
}

TEST_F(MasterKeyResetTest, ResetWipesEncryptedSecrets) {
    const auto paths = makePaths();
    writeKey(paths.masterKey(), 0x55);

    // Pre-populate auth.db with rows that have BLOB-encrypted columns.
    // The reset call should NULL them out (or DELETE for jwt_secret).
    {
        sa::AuthDb db(paths.authDb());
        ASSERT_TRUE(db.open());
    }
    {
        sqlite3* db = nullptr;
        ASSERT_EQ(sqlite3_open_v2(paths.authDb().c_str(), &db,
                                  SQLITE_OPEN_READWRITE, nullptr), SQLITE_OK);
        const std::vector<std::uint8_t> blob = {0x01, 0x02, 0x03, 0x04};

        auto bindBlob = [&](const char* sql) {
            sqlite3_stmt* st = nullptr;
            ASSERT_EQ(sqlite3_prepare_v2(db, sql, -1, &st, nullptr), SQLITE_OK)
                << sqlite3_errmsg(db);
            ASSERT_EQ(sqlite3_bind_blob(st, 1, blob.data(),
                                        static_cast<int>(blob.size()),
                                        SQLITE_STATIC), SQLITE_OK);
            const int rc = sqlite3_step(st);
            sqlite3_finalize(st);
            ASSERT_EQ(rc, SQLITE_DONE) << sqlite3_errmsg(db);
        };

        // Minimal INSERTs — schema has DEFAULTs for everything else and
        // we only care that the encrypted column starts non-NULL so we
        // can prove the reset NULL'd it. Keep this list in lock-step
        // with kSchemaSqlV4 in AuthDb.cpp.
        bindBlob("INSERT INTO ldap_config(id, bind_password_encrypted) "
                 "VALUES(1, ?)");
        bindBlob("INSERT INTO smtp_config(id, password_encrypted) "
                 "VALUES(1, ?)");
        bindBlob("INSERT INTO jwt_secret(id, secret_encrypted, rotated_at) "
                 "VALUES(1, ?, 0)");
        sqlite3_close(db);
    }

    const auto report = sa::resetMasterKey(paths, /*confirmed=*/true);
    ASSERT_EQ(report.result, sa::ResetResult::Success) << report.error;

    // All three column-sets must show up in the wiped[] report.
    auto contains = [&](std::string_view needle) {
        for (const auto& s : report.wiped)
            if (s.find(needle) != std::string::npos) return true;
        return false;
    };
    EXPECT_TRUE(contains("ldap_config.bind_password_encrypted"));
    EXPECT_TRUE(contains("smtp_config.password_encrypted"));
    EXPECT_TRUE(contains("jwt_secret"));

    // Verify on disk: ldap/smtp blobs are NULL, jwt_secret row gone.
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open_v2(paths.authDb().c_str(), &db,
                              SQLITE_OPEN_READONLY, nullptr), SQLITE_OK);
    auto isNullBlob = [&](const char* sql) {
        sqlite3_stmt* st = nullptr;
        EXPECT_EQ(sqlite3_prepare_v2(db, sql, -1, &st, nullptr), SQLITE_OK);
        const int rc = sqlite3_step(st);
        bool result = false;
        if (rc == SQLITE_ROW) {
            result = (sqlite3_column_type(st, 0) == SQLITE_NULL);
        }
        sqlite3_finalize(st);
        return result;
    };
    auto rowCount = [&](const char* sql) {
        sqlite3_stmt* st = nullptr;
        EXPECT_EQ(sqlite3_prepare_v2(db, sql, -1, &st, nullptr), SQLITE_OK);
        int n = 0;
        if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
        sqlite3_finalize(st);
        return n;
    };
    EXPECT_TRUE(isNullBlob("SELECT bind_password_encrypted FROM ldap_config WHERE id=1"));
    EXPECT_TRUE(isNullBlob("SELECT password_encrypted     FROM smtp_config WHERE id=1"));
    EXPECT_EQ(rowCount("SELECT COUNT(*) FROM jwt_secret"), 0);
    sqlite3_close(db);
}
