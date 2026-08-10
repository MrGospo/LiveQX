// fix22 commit 10/24 — recovery-CLI помощники.
//
// Тестируем поведение printBootstrap / resetAdmin прямо как библиотечные
// функции (return code + stdout capture). Полноценный e2e через argv
// в main.cpp здесь не нужен — argv-роутинг тривиальный.

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include "auth/AuthCli.h"
#include "auth/AuthDb.h"
#include "auth/AuthService.h"
#include "auth/JwtIssuer.h"
#include "auth/MasterKey.h"
#include "auth/PasswordHasher.h"

namespace sa = liveqx::auth;

namespace {

class AuthCliTest : public ::testing::Test {
protected:
    std::filesystem::path tmp_;

    void SetUp() override {
        tmp_ = std::filesystem::temp_directory_path() /
            ("auth_cli_test_" +
             std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()) +
             ".db");
        std::filesystem::remove(tmp_);
    }
    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove(tmp_, ec);
        std::filesystem::remove(tmp_.string() + "-wal", ec);
        std::filesystem::remove(tmp_.string() + "-shm", ec);
    }

    // Перехват stdout/stderr вокруг f().
    struct Captured {
        std::string out;
        std::string err;
        int rc{0};
    };
    template <class F>
    Captured run(F&& f) {
        std::stringstream out, err;
        auto* old_out = std::cout.rdbuf(out.rdbuf());
        auto* old_err = std::cerr.rdbuf(err.rdbuf());
        Captured c;
        c.rc = f();
        std::cout.rdbuf(old_out);
        std::cerr.rdbuf(old_err);
        c.out = out.str();
        c.err = err.str();
        return c;
    }
};

}  // namespace

TEST_F(AuthCliTest, PrintBootstrapCreatesAdminAndPrintsPassword) {
    auto cap = run([&]{ return sa::printBootstrap(tmp_); });
    EXPECT_EQ(cap.rc, 0);
    // password line must be present
    EXPECT_NE(cap.out.find("username: admin"), std::string::npos);
    EXPECT_NE(cap.out.find("password:"),       std::string::npos);
    EXPECT_NE(cap.out.find("must_change_password: true"), std::string::npos);

    // верифицируем, что в БД действительно появился admin
    sa::AuthDb db(tmp_);
    ASSERT_TRUE(db.open());
    auto u = db.findUserByUsername("admin");
    ASSERT_TRUE(u.has_value());
    EXPECT_EQ(u->role, sa::Role::Admin);
    EXPECT_TRUE(u->must_change_password);
}

TEST_F(AuthCliTest, PrintBootstrapNoOpWhenAdminExists) {
    {
        sa::AuthDb db(tmp_);
        ASSERT_TRUE(db.open());
        auto h = sa::PasswordHasher::hash("p");
        ASSERT_TRUE(h.has_value());
        sa::User u;
        u.username = "rooty"; u.password_hash = *h;
        u.role = sa::Role::Admin; u.source = sa::Source::Local;
        ASSERT_TRUE(db.insertUser(u).has_value());
    }
    auto cap = run([&]{ return sa::printBootstrap(tmp_); });
    EXPECT_EQ(cap.rc, 0);
    // ничего не печатает в stdout, только в stderr
    EXPECT_EQ(cap.out.find("password:"), std::string::npos);
    EXPECT_NE(cap.err.find("already exists"), std::string::npos);

    // user "admin" не должен был появиться
    sa::AuthDb db(tmp_);
    ASSERT_TRUE(db.open());
    EXPECT_FALSE(db.findUserByUsername("admin").has_value());
}

TEST_F(AuthCliTest, PrintBootstrapFailsOnUnopenableDb) {
    // Передаём путь к директории — sqlite3_open не сможет открыть.
    auto bad = std::filesystem::temp_directory_path();  // existing dir, не файл
    auto cap = run([&]{ return sa::printBootstrap(bad); });
    EXPECT_EQ(cap.rc, 1);
    EXPECT_NE(cap.err.find("ERROR: cannot open auth.db"), std::string::npos);
}

TEST_F(AuthCliTest, ResetAdminGeneratesNewPasswordAndPrints) {
    {
        sa::AuthDb db(tmp_);
        ASSERT_TRUE(db.open());
        auto h = sa::PasswordHasher::hash("old");
        sa::User u;
        u.username = "admin"; u.password_hash = *h;
        u.role = sa::Role::Admin; u.source = sa::Source::Local;
        ASSERT_TRUE(db.insertUser(u).has_value());
    }
    auto cap = run([&]{ return sa::resetAdmin(tmp_, "admin"); });
    EXPECT_EQ(cap.rc, 0);
    EXPECT_NE(cap.out.find("username: admin"), std::string::npos);
    EXPECT_NE(cap.out.find("password:"),       std::string::npos);
    EXPECT_NE(cap.out.find("must_change_password: true"), std::string::npos);

    // old password больше не работает.
    sa::AuthDb db(tmp_);
    ASSERT_TRUE(db.open());
    auto u = db.findUserByUsername("admin");
    ASSERT_TRUE(u.has_value());
    EXPECT_FALSE(sa::PasswordHasher::verify("old", u->password_hash));
    EXPECT_TRUE(u->must_change_password);
}

TEST_F(AuthCliTest, ResetAdminUserNotFound) {
    sa::AuthDb db(tmp_);
    ASSERT_TRUE(db.open());

    auto cap = run([&]{ return sa::resetAdmin(tmp_, "ghost"); });
    EXPECT_EQ(cap.rc, 1);
    EXPECT_NE(cap.err.find("not found"), std::string::npos);
}

TEST_F(AuthCliTest, ResetAdminRefusesNonAdmin) {
    {
        sa::AuthDb db(tmp_);
        ASSERT_TRUE(db.open());
        auto h = sa::PasswordHasher::hash("p");
        sa::User u;
        u.username = "viewer1"; u.password_hash = *h;
        u.role = sa::Role::Viewer; u.source = sa::Source::Local;
        ASSERT_TRUE(db.insertUser(u).has_value());
    }
    auto cap = run([&]{ return sa::resetAdmin(tmp_, "viewer1"); });
    EXPECT_EQ(cap.rc, 1);
    EXPECT_NE(cap.err.find("not an admin"), std::string::npos);
}

TEST_F(AuthCliTest, ResetAdminRefusesLdapSource) {
    {
        sa::AuthDb db(tmp_);
        ASSERT_TRUE(db.open());
        // LDAP юзер: hash может быть пустой, source=ldap.
        sa::User u;
        u.username = "ldap-admin"; u.password_hash = "";
        u.role = sa::Role::Admin; u.source = sa::Source::Ldap;
        ASSERT_TRUE(db.insertUser(u).has_value());
    }
    auto cap = run([&]{ return sa::resetAdmin(tmp_, "ldap-admin"); });
    EXPECT_EQ(cap.rc, 1);
    EXPECT_NE(cap.err.find("ldap"), std::string::npos);
}

// ── fix22 c15/24 — rotateMasterKey ──────────────────────────────────────

namespace {

class RotateMasterKeyTest : public ::testing::Test {
protected:
    std::filesystem::path dir_;
    std::filesystem::path db_path()  const { return dir_ / "auth.db"; }
    std::filesystem::path key_path() const { return dir_ / "master.key"; }

    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() /
            ("rotate_mk_test_" +
             std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
        std::filesystem::create_directories(dir_);
        ::unsetenv("LIVEQX_MASTER_KEY");
    }
    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
        ::unsetenv("LIVEQX_MASTER_KEY");
    }
};

}  // namespace

TEST_F(RotateMasterKeyTest, FailsIfKeyFileMissing) {
    // DB ok, key file отсутствует — должна быть явная ошибка.
    sa::AuthDb db(db_path());
    ASSERT_TRUE(db.open());

    std::stringstream out, err;
    auto* old_out = std::cout.rdbuf(out.rdbuf());
    auto* old_err = std::cerr.rdbuf(err.rdbuf());
    int rc = sa::rotateMasterKey(db_path(), key_path());
    std::cout.rdbuf(old_out);
    std::cerr.rdbuf(old_err);

    EXPECT_EQ(rc, 1);
    EXPECT_NE(err.str().find("master key file not found"), std::string::npos);
}

TEST_F(RotateMasterKeyTest, RotatesEvenWithoutEncryptedBlobs) {
    // Создаём ключ через MasterKey::load() (auto-gen).
    {
        sa::MasterKey mk(key_path().string());
        ASSERT_TRUE(mk.load());
    }
    auto old_size = std::filesystem::file_size(key_path());
    EXPECT_EQ(old_size, sa::MasterKey::kKeyBytes);

    // Снимем raw bytes старого ключа для сравнения.
    std::vector<std::uint8_t> old_bytes(sa::MasterKey::kKeyBytes);
    {
        std::ifstream f(key_path(), std::ios::binary);
        f.read(reinterpret_cast<char*>(old_bytes.data()), old_bytes.size());
    }

    std::stringstream out, err;
    auto* old_out = std::cout.rdbuf(out.rdbuf());
    auto* old_err = std::cerr.rdbuf(err.rdbuf());
    int rc = sa::rotateMasterKey(db_path(), key_path());
    std::cout.rdbuf(old_out);
    std::cerr.rdbuf(old_err);

    EXPECT_EQ(rc, 0) << "stderr: " << err.str();
    EXPECT_NE(out.str().find("master_key_rotated: true"), std::string::npos);
    EXPECT_NE(out.str().find("blobs_re_encrypted: (none)"), std::string::npos);

    // Файл всё ещё на месте, размер 32 байта.
    ASSERT_TRUE(std::filesystem::exists(key_path()));
    EXPECT_EQ(std::filesystem::file_size(key_path()),
              static_cast<std::uintmax_t>(sa::MasterKey::kKeyBytes));

    // Содержимое отличается от старого.
    std::vector<std::uint8_t> new_bytes(sa::MasterKey::kKeyBytes);
    {
        std::ifstream f(key_path(), std::ios::binary);
        f.read(reinterpret_cast<char*>(new_bytes.data()), new_bytes.size());
    }
    EXPECT_NE(old_bytes, new_bytes);

    // Backup создан и содержит старые байты.
    bool found_bak = false;
    std::vector<std::uint8_t> bak_bytes;
    for (auto& e : std::filesystem::directory_iterator(dir_)) {
        const auto name = e.path().filename().string();
        if (name.rfind("master.key.bak.", 0) == 0) {
            found_bak = true;
            bak_bytes.resize(sa::MasterKey::kKeyBytes);
            std::ifstream f(e.path(), std::ios::binary);
            f.read(reinterpret_cast<char*>(bak_bytes.data()), bak_bytes.size());
            break;
        }
    }
    ASSERT_TRUE(found_bak);
    EXPECT_EQ(bak_bytes, old_bytes);
}

TEST_F(RotateMasterKeyTest, ReEncryptsJwtSecretSoNewKeyCanDecrypt) {
    // (1) auto-gen старого ключа и кладём в DB зашифрованный jwt_secret.
    const std::string jwt_plain = "super-secret-jwt-signing-key-32+bytes!!";
    {
        sa::MasterKey mk(key_path().string());
        ASSERT_TRUE(mk.load());
        auto ct = mk.encrypt(jwt_plain);

        sa::AuthDb db(db_path());
        ASSERT_TRUE(db.open());
        ASSERT_TRUE(db.storeJwtSecret(ct, /*rotated_at=*/1234567890));
        EXPECT_TRUE(db.hasJwtSecret());
    }

    // (2) Ротируем.
    std::stringstream out, err;
    auto* old_out = std::cout.rdbuf(out.rdbuf());
    auto* old_err = std::cerr.rdbuf(err.rdbuf());
    int rc = sa::rotateMasterKey(db_path(), key_path());
    std::cout.rdbuf(old_out);
    std::cerr.rdbuf(old_err);

    ASSERT_EQ(rc, 0) << "stderr: " << err.str();
    EXPECT_NE(out.str().find("blobs_re_encrypted: jwt_secret"),
              std::string::npos);

    // (3) Открываем БД с новым ключом и проверяем, что jwt_secret
    // расшифровывается обратно в plaintext.
    sa::MasterKey new_mk(key_path().string());
    ASSERT_TRUE(new_mk.load());

    sa::AuthDb db(db_path());
    ASSERT_TRUE(db.open());
    auto blob = db.readJwtSecret();
    ASSERT_TRUE(blob.has_value());
    auto pt = new_mk.decrypt(*blob);
    ASSERT_TRUE(pt.has_value());
    EXPECT_EQ(*pt, jwt_plain);
}

TEST_F(RotateMasterKeyTest, NewKeyFileIsMode0600) {
    {
        sa::MasterKey mk(key_path().string());
        ASSERT_TRUE(mk.load());
    }
    std::stringstream out, err;
    auto* old_out = std::cout.rdbuf(out.rdbuf());
    auto* old_err = std::cerr.rdbuf(err.rdbuf());
    int rc = sa::rotateMasterKey(db_path(), key_path());
    std::cout.rdbuf(old_out);
    std::cerr.rdbuf(old_err);
    ASSERT_EQ(rc, 0) << err.str();

    struct stat st{};
    ASSERT_EQ(::stat(key_path().string().c_str(), &st), 0);
    EXPECT_EQ(st.st_mode & 0777, 0600);
}

TEST_F(RotateMasterKeyTest, RefusesIfExistingBlobUndecryptable) {
    // (1) auto-gen ключа.
    {
        sa::MasterKey mk(key_path().string());
        ASSERT_TRUE(mk.load());
    }
    // (2) Кладём в jwt_secret мусор: ciphertext, который наш ключ
    // расшифровать не сможет.
    {
        sa::AuthDb db(db_path());
        ASSERT_TRUE(db.open());
        std::vector<std::uint8_t> garbage(
            sa::MasterKey::kNonceBytes + sa::MasterKey::kMacBytes + 8, 0xCC);
        ASSERT_TRUE(db.storeJwtSecret(garbage, 1));
    }
    // (3) Снимаем bytes до ротации — после неудачной попытки они должны
    // остаться неизменными.
    std::vector<std::uint8_t> before(sa::MasterKey::kKeyBytes);
    {
        std::ifstream f(key_path(), std::ios::binary);
        f.read(reinterpret_cast<char*>(before.data()), before.size());
    }

    std::stringstream out, err;
    auto* old_out = std::cout.rdbuf(out.rdbuf());
    auto* old_err = std::cerr.rdbuf(err.rdbuf());
    int rc = sa::rotateMasterKey(db_path(), key_path());
    std::cout.rdbuf(old_out);
    std::cerr.rdbuf(old_err);

    EXPECT_EQ(rc, 1);
    EXPECT_NE(err.str().find("cannot be decrypted"), std::string::npos);

    // Файл ключа не тронут.
    std::vector<std::uint8_t> after(sa::MasterKey::kKeyBytes);
    {
        std::ifstream f(key_path(), std::ios::binary);
        f.read(reinterpret_cast<char*>(after.data()), after.size());
    }
    EXPECT_EQ(before, after);
}
