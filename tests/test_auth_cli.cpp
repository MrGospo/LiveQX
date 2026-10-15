// fix22 commit 10/24 — recovery-CLI помощники.
//
// Тестируем поведение printBootstrap / resetAdmin прямо как библиотечные
// функции (return code + stdout capture). Полноценный e2e через argv
// в main.cpp здесь не нужен — argv-роутинг тривиальный.

#include <gtest/gtest.h>

#include <cstdlib>
#include <functional>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
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
#include "mounts/MountsDb.h"
#include "utils/DeviceCa.h"
#include "utils/RuntimeLock.h"

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

// ── rotation must cover EVERY secret encrypted with the master key ───────
//
// Before this, rotateMasterKey() re-encrypted only jwt_secret: the LDAP
// bind password, the SMTP password and the CIFS passwords of mounts were
// left under the old key and became undecryptable after a rotation.

namespace {

struct RotationRun {
    int         rc{0};
    std::string out;
    std::string err;
};

RotationRun runRotation(const std::filesystem::path& db,
                        const std::filesystem::path& key,
                        const std::filesystem::path& mounts = {},
                        const std::filesystem::path& device_ca = {},
                        const std::filesystem::path& state_dir = {},
                        const std::string& key_store = "file") {
    std::stringstream out, err;
    auto* old_out = std::cout.rdbuf(out.rdbuf());
    auto* old_err = std::cerr.rdbuf(err.rdbuf());
    RotationRun r;
    r.rc = sa::rotateMasterKey(db, key, mounts, device_ca, state_dir, key_store);
    std::cout.rdbuf(old_out);
    std::cerr.rdbuf(old_err);
    r.out = out.str();
    r.err = err.str();
    return r;
}

std::vector<std::uint8_t> readFileBytes(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

std::vector<std::uint8_t> garbageBlob() {
    return std::vector<std::uint8_t>(
        sa::MasterKey::kNonceBytes + sa::MasterKey::kMacBytes + 8, 0xCC);
}

}  // namespace

TEST_F(RotateMasterKeyTest, ReEncryptsLdapBindPassword) {
    sa::MasterKey mk(key_path().string());
    ASSERT_TRUE(mk.load());
    {
        sa::AuthDb db(db_path());
        ASSERT_TRUE(db.open());
        sa::AuthDb::LdapConfigRow row;
        row.server = "ldaps://dir.example.com";
        row.bind_dn = "cn=svc,dc=example,dc=com";
        row.bind_password_encrypted = mk.encrypt("ldap-secret");
        ASSERT_TRUE(db.writeLdapConfigRow(row));
    }

    const auto run = runRotation(db_path(), key_path());
    ASSERT_EQ(run.rc, 0) << run.err;
    EXPECT_NE(run.out.find("ldap_config.bind_password"), std::string::npos);

    sa::MasterKey fresh(key_path().string());
    ASSERT_TRUE(fresh.load());
    sa::AuthDb db(db_path());
    ASSERT_TRUE(db.open());
    const auto row = db.readLdapConfigRow();
    ASSERT_TRUE(row.has_value());
    const auto plain = fresh.decrypt(row->bind_password_encrypted);
    ASSERT_TRUE(plain.has_value());
    EXPECT_EQ(*plain, "ldap-secret");
    // Nothing else in the row is touched.
    EXPECT_EQ(row->server, "ldaps://dir.example.com");
    EXPECT_EQ(row->bind_dn, "cn=svc,dc=example,dc=com");
}

TEST_F(RotateMasterKeyTest, ReEncryptsSmtpPassword) {
    sa::MasterKey mk(key_path().string());
    ASSERT_TRUE(mk.load());
    {
        sa::AuthDb db(db_path());
        ASSERT_TRUE(db.open());
        sa::AuthDb::SmtpConfigRow row;
        row.server = "smtp.example.com";
        row.port = 2525;
        row.password_encrypted = mk.encrypt("smtp-secret");
        ASSERT_TRUE(db.writeSmtpConfigRow(row));
    }

    const auto run = runRotation(db_path(), key_path());
    ASSERT_EQ(run.rc, 0) << run.err;
    EXPECT_NE(run.out.find("smtp_config.password"), std::string::npos);

    sa::MasterKey fresh(key_path().string());
    ASSERT_TRUE(fresh.load());
    sa::AuthDb db(db_path());
    ASSERT_TRUE(db.open());
    const auto row = db.readSmtpConfigRow();
    ASSERT_TRUE(row.has_value());
    const auto plain = fresh.decrypt(row->password_encrypted);
    ASSERT_TRUE(plain.has_value());
    EXPECT_EQ(*plain, "smtp-secret");
    EXPECT_EQ(row->server, "smtp.example.com");
    EXPECT_EQ(row->port, 2525);
}

TEST_F(RotateMasterKeyTest, ReEncryptsCifsPasswordsOfMounts) {
    const auto mounts_path = dir_ / "mounts.db";
    sa::MasterKey mk(key_path().string());
    ASSERT_TRUE(mk.load());

    std::int64_t with_pw = 0, without_pw = 0;
    std::int64_t updated_with = 0;
    {
        liveqx::mounts::MountsDb mdb(mounts_path);
        ASSERT_TRUE(mdb.open());
        liveqx::mounts::MountRow a;
        a.fs_type = "cifs"; a.source = "//srv/a"; a.target = "/mnt/liveqx/a";
        a.cifs_username = "alice";
        a.cifs_password_blob = mk.encrypt("cifs-secret");
        const auto ia = mdb.insert(a);
        ASSERT_TRUE(ia.has_value());
        with_pw = *ia;

        liveqx::mounts::MountRow b;
        b.fs_type = "nfs"; b.source = "10.0.0.1:/x"; b.target = "/mnt/liveqx/b";
        const auto ib = mdb.insert(b);
        ASSERT_TRUE(ib.has_value());
        without_pw = *ib;
        updated_with = mdb.findById(with_pw)->updated_at;
    }

    const auto run = runRotation(db_path(), key_path(), mounts_path);
    ASSERT_EQ(run.rc, 0) << run.err;
    EXPECT_NE(run.out.find("mounts.cifs_password"), std::string::npos);

    sa::MasterKey fresh(key_path().string());
    ASSERT_TRUE(fresh.load());
    liveqx::mounts::MountsDb mdb(mounts_path);
    ASSERT_TRUE(mdb.open());
    const auto a = mdb.findById(with_pw);
    ASSERT_TRUE(a.has_value());
    const auto plain = fresh.decrypt(a->cifs_password_blob);
    ASSERT_TRUE(plain.has_value());
    EXPECT_EQ(*plain, "cifs-secret");
    EXPECT_EQ(a->updated_at, updated_with)
        << "a rotation must not look like an operator edit";
    EXPECT_EQ(a->cifs_username, "alice");
    const auto b = mdb.findById(without_pw);
    ASSERT_TRUE(b.has_value());
    EXPECT_FALSE(b->hasPassword());
}

TEST_F(RotateMasterKeyTest, MissingMountsDbIsSkippedAndNotCreated) {
    {
        sa::MasterKey mk(key_path().string());
        ASSERT_TRUE(mk.load());
    }
    const auto mounts_path = dir_ / "no-such-mounts.db";
    const auto run = runRotation(db_path(), key_path(), mounts_path);
    ASSERT_EQ(run.rc, 0) << run.err;
    EXPECT_FALSE(std::filesystem::exists(mounts_path));
}

// ── key source awareness: rotation must write where the service reads ────

namespace {

struct EnvVar {
    std::string name;
    EnvVar(const char* n, const std::string& v) : name(n) {
        ::setenv(n, v.c_str(), 1);
    }
    ~EnvVar() { ::unsetenv(name.c_str()); }
};

void writeRawKey(const std::filesystem::path& p, char fill) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << std::string(sa::MasterKey::kKeyBytes, fill);
}

}  // namespace

TEST_F(RotateMasterKeyTest, RefusesWhileServiceIsRunningAndChangesNothing) {
    {
        sa::MasterKey mk(key_path().string());
        ASSERT_TRUE(mk.load());
    }
    const auto key_before = readFileBytes(key_path());
    auto lock = liveqx::util::RuntimeLock::tryAcquire(dir_ / "liveqx.lock");
    ASSERT_TRUE(lock.held());

    const auto run = runRotation(db_path(), key_path(), {}, {}, dir_);
    EXPECT_EQ(run.rc, 1);
    EXPECT_NE(run.err.find("running"), std::string::npos) << run.err;
    EXPECT_EQ(readFileBytes(key_path()), key_before);
}

TEST_F(RotateMasterKeyTest, RotatesWhenServiceIsStopped) {
    {
        sa::MasterKey mk(key_path().string());
        ASSERT_TRUE(mk.load());
    }
    const auto run = runRotation(db_path(), key_path(), {}, {}, dir_);
    EXPECT_EQ(run.rc, 0) << run.err;
}

TEST_F(RotateMasterKeyTest, RefusesWhenKeyComesFromEnvironmentVariable) {
    {
        sa::MasterKey mk(key_path().string());
        ASSERT_TRUE(mk.load());
    }
    const auto key_before = readFileBytes(key_path());
    // 32 zero bytes, base64.
    EnvVar env("LIVEQX_MASTER_KEY",
               "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=");

    const auto run = runRotation(db_path(), key_path());
    EXPECT_EQ(run.rc, 1);
    EXPECT_NE(run.err.find("LIVEQX_MASTER_KEY"), std::string::npos) << run.err;
    EXPECT_EQ(readFileBytes(key_path()), key_before)
        << "the file the service does not read must stay untouched";
}

TEST_F(RotateMasterKeyTest, RotatesExternalKeyFileNamedByEnvironment) {
    const auto ext = dir_ / "external.key";
    writeRawKey(ext, 'K');
    const auto before = readFileBytes(ext);
    EnvVar env("LIVEQX_MASTER_KEY_FILE", ext.string());

    const auto run = runRotation(db_path(), ext);
    ASSERT_EQ(run.rc, 0) << run.err;
    EXPECT_NE(readFileBytes(ext), before);
    bool backup = false;
    for (const auto& e : std::filesystem::directory_iterator(dir_))
        if (e.path().filename().string().rfind("external.key.bak.", 0) == 0)
            backup = true;
    EXPECT_TRUE(backup);
}

TEST_F(RotateMasterKeyTest, RefusesSystemdCredential) {
    const auto creds = dir_ / "creds";
    std::filesystem::create_directories(creds);
    writeRawKey(creds / "liveqx_master_key", 'S');
    EnvVar dirvar("CREDENTIALS_DIRECTORY", creds.string());
    EnvVar filevar("LIVEQX_MASTER_KEY_FILE", (creds / "liveqx_master_key").string());

    const auto run = runRotation(db_path(), creds / "liveqx_master_key");
    EXPECT_EQ(run.rc, 1);
    EXPECT_NE(run.err.find("systemd"), std::string::npos) << run.err;
}

TEST_F(RotateMasterKeyTest, RefusesKeyStoreItCannotRewrite) {
    {
        sa::MasterKey mk(key_path().string());
        ASSERT_TRUE(mk.load());
    }
    const auto key_before = readFileBytes(key_path());
    const auto run = runRotation(db_path(), key_path(), {}, {}, {}, "creds");
    EXPECT_EQ(run.rc, 1);
    EXPECT_NE(run.err.find("master_key_store"), std::string::npos) << run.err;
    EXPECT_EQ(readFileBytes(key_path()), key_before);
}

TEST_F(RotateMasterKeyTest, ResealsDeviceCaKeyUnderTheNewMasterKey) {
    const auto ca_dir = dir_ / "device_ca";
    std::string cert_pem;
    {
        sa::MasterKey mk(key_path().string());
        ASSERT_TRUE(mk.load());
        std::string err;
        auto ca = liveqx::tls::DeviceCa::open(ca_dir, mk, err);
        ASSERT_TRUE(ca) << err;
        cert_pem = ca->certPem();
    }
    const auto run = runRotation(db_path(), key_path(), {}, ca_dir);
    ASSERT_EQ(run.rc, 0) << run.err;
    EXPECT_NE(run.out.find("device_ca"), std::string::npos);

    sa::MasterKey fresh(key_path().string());
    ASSERT_TRUE(fresh.load());
    std::string err;
    auto ca = liveqx::tls::DeviceCa::open(ca_dir, fresh, err);
    ASSERT_TRUE(ca) << "device CA must open under the rotated key: " << err;
    EXPECT_EQ(ca->certPem(), cert_pem) << "the CA itself must not change";
}

TEST_F(RotateMasterKeyTest, RefusesIfDeviceCaKeyUndecryptableAndChangesNothing) {
    const auto ca_dir = dir_ / "device_ca";
    {
        sa::MasterKey mk(key_path().string());
        ASSERT_TRUE(mk.load());
        std::string err;
        ASSERT_TRUE(liveqx::tls::DeviceCa::open(ca_dir, mk, err)) << err;
    }
    {
        std::ofstream f(ca_dir / "gen-1" / "ca.key.enc",
                        std::ios::binary | std::ios::trunc);
        f << std::string(80, 'x');
    }
    const auto key_before = readFileBytes(key_path());

    const auto run = runRotation(db_path(), key_path(), {}, ca_dir);
    EXPECT_EQ(run.rc, 1);
    EXPECT_NE(run.err.find("device CA"), std::string::npos) << run.err;
    EXPECT_EQ(readFileBytes(key_path()), key_before);
}

TEST_F(RotateMasterKeyTest, MissingDeviceCaDirIsSkippedAndNotCreated) {
    {
        sa::MasterKey mk(key_path().string());
        ASSERT_TRUE(mk.load());
    }
    const auto ca_dir = dir_ / "no-device-ca";
    const auto run = runRotation(db_path(), key_path(), {}, ca_dir);
    ASSERT_EQ(run.rc, 0) << run.err;
    EXPECT_FALSE(std::filesystem::exists(ca_dir));
}

TEST_F(RotateMasterKeyTest, RefusesIfLdapBlobUndecryptableAndChangesNothing) {
    sa::MasterKey mk(key_path().string());
    ASSERT_TRUE(mk.load());
    const auto jwt_blob = mk.encrypt("jwt-secret-value-32-bytes-long!!!!");
    {
        sa::AuthDb db(db_path());
        ASSERT_TRUE(db.open());
        ASSERT_TRUE(db.storeJwtSecret(jwt_blob, 1));
        sa::AuthDb::LdapConfigRow row;
        row.server = "ldaps://x";
        row.bind_password_encrypted = garbageBlob();
        ASSERT_TRUE(db.writeLdapConfigRow(row));
    }
    const auto key_before = readFileBytes(key_path());

    const auto run = runRotation(db_path(), key_path());
    EXPECT_EQ(run.rc, 1);
    EXPECT_NE(run.err.find("cannot be decrypted"), std::string::npos);
    EXPECT_EQ(readFileBytes(key_path()), key_before);

    // The jwt blob, which could have been rotated first, is untouched.
    sa::AuthDb db(db_path());
    ASSERT_TRUE(db.open());
    const auto jwt_after = db.readJwtSecret();
    ASSERT_TRUE(jwt_after.has_value());
    EXPECT_EQ(*jwt_after, jwt_blob);
}

TEST_F(RotateMasterKeyTest, RefusesIfMountBlobUndecryptableAndChangesNothing) {
    const auto mounts_path = dir_ / "mounts.db";
    sa::MasterKey mk(key_path().string());
    ASSERT_TRUE(mk.load());
    const auto good = mk.encrypt("fine");
    {
        sa::AuthDb db(db_path());
        ASSERT_TRUE(db.open());
        ASSERT_TRUE(db.storeJwtSecret(mk.encrypt("jwt"), 1));
        liveqx::mounts::MountsDb mdb(mounts_path);
        ASSERT_TRUE(mdb.open());
        liveqx::mounts::MountRow a;
        a.fs_type = "cifs"; a.source = "//srv/a"; a.target = "/mnt/liveqx/a";
        a.cifs_password_blob = good;
        ASSERT_TRUE(mdb.insert(a).has_value());
        liveqx::mounts::MountRow b;
        b.fs_type = "cifs"; b.source = "//srv/b"; b.target = "/mnt/liveqx/b";
        b.cifs_password_blob = garbageBlob();
        ASSERT_TRUE(mdb.insert(b).has_value());
    }
    const auto key_before = readFileBytes(key_path());

    const auto run = runRotation(db_path(), key_path(), mounts_path);
    EXPECT_EQ(run.rc, 1);
    EXPECT_EQ(readFileBytes(key_path()), key_before);

    liveqx::mounts::MountsDb mdb(mounts_path);
    ASSERT_TRUE(mdb.open());
    const auto rows = mdb.listAll();
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows[0].cifs_password_blob, good);   // not rotated
}

// ── the transactional core, tested without any real storage ──────────────

namespace {

class ReencryptAllTest : public ::testing::Test {
protected:
    std::filesystem::path dir_;
    std::unique_ptr<sa::MasterKey> key_;

    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() /
            ("reencrypt_all_" +
             std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
        std::filesystem::create_directories(dir_);
        ::unsetenv("LIVEQX_MASTER_KEY");
        key_ = std::make_unique<sa::MasterKey>((dir_ / "k.key").string());
        ASSERT_TRUE(key_->load());
    }
    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    // An entry whose "storage" is a vector the test can inspect.
    sa::detail::RotationEntry entry(std::string label, std::string plain,
                                    std::vector<std::vector<std::uint8_t>>& writes,
                                    bool fail_on_new = false) {
        sa::detail::RotationEntry e;
        e.label     = std::move(label);
        e.plaintext = std::move(plain);
        e.old_blob  = std::vector<std::uint8_t>{0x0A, 0x0B};   // marker
        e.store = [&writes, fail_on_new, old = e.old_blob]
                  (const std::vector<std::uint8_t>& blob) {
            // A failing store still fails when asked to put the OLD blob
            // back only if the test says so via writes.size() sentinel.
            if (fail_on_new && blob != old) return false;
            writes.push_back(blob);
            return true;
        };
        return e;
    }
};

}  // namespace

TEST_F(ReencryptAllTest, StoresEveryEntryUnderTheNewKey) {
    std::vector<std::vector<std::uint8_t>> w1, w2;
    std::vector<sa::detail::RotationEntry> entries;
    entries.push_back(entry("a", "alpha", w1));
    entries.push_back(entry("b", "beta",  w2));

    std::string err;
    std::vector<std::string> done;
    ASSERT_TRUE(sa::detail::reencryptAll(entries, *key_, err, &done)) << err;
    ASSERT_EQ(w1.size(), 1u);
    ASSERT_EQ(w2.size(), 1u);
    EXPECT_EQ(key_->decrypt(w1[0]).value_or(""), "alpha");
    EXPECT_EQ(key_->decrypt(w2[0]).value_or(""), "beta");
    EXPECT_EQ(done, (std::vector<std::string>{"a", "b"}));
}

TEST_F(ReencryptAllTest, PutsBackEntriesAlreadyStoredWhenALaterStoreFails) {
    std::vector<std::vector<std::uint8_t>> w1, w2, w3;
    std::vector<sa::detail::RotationEntry> entries;
    entries.push_back(entry("a", "alpha", w1));
    entries.push_back(entry("b", "beta",  w2));
    entries.push_back(entry("c", "gamma", w3, /*fail_on_new=*/true));

    std::string err;
    std::vector<std::string> done;
    EXPECT_FALSE(sa::detail::reencryptAll(entries, *key_, err, &done));
    EXPECT_NE(err.find("re-encrypted c"), std::string::npos) << "the failing entry is named";

    // a and b were stored under the new key, then put back to the old blob.
    ASSERT_EQ(w1.size(), 2u);
    ASSERT_EQ(w2.size(), 2u);
    EXPECT_EQ(w1[1], (std::vector<std::uint8_t>{0x0A, 0x0B}));
    EXPECT_EQ(w2[1], (std::vector<std::uint8_t>{0x0A, 0x0B}));
    EXPECT_TRUE(w3.empty());
    EXPECT_TRUE(done.empty()) << "nothing counts as rotated after a rollback";
}
