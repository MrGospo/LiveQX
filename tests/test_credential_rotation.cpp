// Rotation of a master key that lives in a systemd credential.
//
// The real thing runs `systemd-creds` (needs a host key / TPM); here a fake
// CredentialStore keeps the key in a plain file so the rotation logic — what
// is decrypted, what is staged, what is committed, and what is left untouched
// on every failure — can be tested on any machine.

#include "auth/AuthCli.h"

#include "auth/AuthDb.h"
#include "auth/MasterKey.h"
#include "utils/CredentialStore.h"
#include "utils/DeviceCa.h"
#include "utils/RuntimeLock.h"

#include <gtest/gtest.h>

#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

namespace fs = std::filesystem;
namespace sa = liveqx::auth;
using liveqx::util::CredentialStore;

namespace {

using Key = std::array<std::uint8_t, 32>;

std::string hex(const Key& k) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (auto b : k) { s.push_back(d[b >> 4]); s.push_back(d[b & 15]); }
    return s;
}

// Stores "<name>:<hex key>" in the file; checks the name on decrypt, as systemd does.
class FakeCredentialStore : public CredentialStore {
public:
    bool fail_encrypt{false};
    bool fail_decrypt{false};
    int  encrypts{0};

    bool decrypt(const fs::path& file, const std::string& name, Key& out,
                 std::string& err) override {
        if (fail_decrypt) { err = "fake decrypt failure"; return false; }
        std::ifstream f(file, std::ios::binary);
        if (!f) { err = "cannot read " + file.string(); return false; }
        std::string line;
        std::getline(f, line);
        const auto colon = line.find(':');
        if (colon == std::string::npos || line.substr(0, colon) != name ||
            line.size() != colon + 1 + 64) {
            err = "credential has a different name or is damaged";
            return false;
        }
        for (std::size_t i = 0; i < 32; ++i)
            out[i] = static_cast<std::uint8_t>(
                std::stoi(line.substr(colon + 1 + i * 2, 2), nullptr, 16));
        return true;
    }

    bool encrypt(const Key& key, const fs::path& file, const std::string& name,
                 std::string& err) override {
        ++encrypts;
        if (fail_encrypt) { err = "fake encrypt failure"; return false; }
        std::ofstream f(file, std::ios::binary | std::ios::trunc);
        f << name << ":" << hex(key) << "\n";
        return static_cast<bool>(f);
    }
};

constexpr const char* kName = "liveqx_master_key";

class CredRotationTest : public ::testing::Test {
protected:
    fs::path dir_, cred_, db_, state_, caDir_;
    FakeCredentialStore store_;
    Key oldKey_{};

    void SetUp() override {
        static std::atomic<int> seq{0};
        dir_ = fs::temp_directory_path() /
               ("lqx-credrot-" + std::to_string(::getpid()) + "-" + std::to_string(seq.fetch_add(1)));
        fs::create_directories(dir_ / "state");
        state_ = dir_ / "state";
        cred_  = dir_ / "liveqx_master_key";
        db_    = state_ / "auth.db";
        caDir_ = state_ / "device_ca";
        oldKey_.fill(0x11);
        std::string err;
        ASSERT_TRUE(store_.encrypt(oldKey_, cred_, kName, err)) << err;
        store_.encrypts = 0;
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    std::unique_ptr<sa::MasterKey> keyOf(const Key& k) {
        auto mk = std::make_unique<sa::MasterKey>("unused");
        mk->loadRaw(k);
        return mk;
    }

    std::vector<std::uint8_t> storeJwtSecret(const std::string& plain) {
        auto mk = keyOf(oldKey_);
        auto blob = mk->encrypt(plain);
        sa::AuthDb db(db_);
        EXPECT_TRUE(db.open());
        EXPECT_TRUE(db.storeJwtSecret(blob, 1));
        return blob;
    }

    Key credentialKey() {
        Key k{};
        std::string err;
        EXPECT_TRUE(store_.decrypt(cred_, kName, k, err)) << err;
        return k;
    }

    struct Out { int rc; std::string out, err; };
    Out rotate() {
        std::stringstream o, e;
        auto* oo = std::cout.rdbuf(o.rdbuf());
        auto* oe = std::cerr.rdbuf(e.rdbuf());
        Out r;
        r.rc = sa::rotateMasterKeyCredential(db_, cred_, kName, store_, {}, caDir_, state_);
        std::cout.rdbuf(oo);
        std::cerr.rdbuf(oe);
        r.out = o.str();
        r.err = e.str();
        return r;
    }

    std::size_t filesNamed(const std::string& prefix) {
        std::size_t n = 0;
        for (const auto& e : fs::directory_iterator(dir_))
            if (e.path().filename().string().rfind(prefix, 0) == 0) ++n;
        return n;
    }
};

}  // namespace

TEST_F(CredRotationTest, RotatesTheCredentialAndReEncryptsTheSecrets) {
    storeJwtSecret("jwt-secret-value-32-bytes-long!!!!");
    const auto r = rotate();
    ASSERT_EQ(r.rc, 0) << r.err;
    EXPECT_NE(r.out.find("master_key_rotated: true"), std::string::npos);

    const Key fresh = credentialKey();
    EXPECT_NE(fresh, oldKey_) << "the credential must now hold a new key";

    sa::AuthDb db(db_);
    ASSERT_TRUE(db.open());
    auto blob = db.readJwtSecret();
    ASSERT_TRUE(blob.has_value());
    auto mk = keyOf(fresh);
    EXPECT_EQ(mk->decrypt(*blob).value_or(""), "jwt-secret-value-32-bytes-long!!!!");
}

TEST_F(CredRotationTest, KeepsTheOldCredentialAsABackup) {
    storeJwtSecret("jwt-secret-value-32-bytes-long!!!!");
    ASSERT_EQ(rotate().rc, 0);
    ASSERT_EQ(filesNamed("liveqx_master_key.bak."), 1u);
    for (const auto& e : fs::directory_iterator(dir_)) {
        if (e.path().filename().string().rfind("liveqx_master_key.bak.", 0) != 0) continue;
        Key old{};
        std::string err;
        ASSERT_TRUE(store_.decrypt(e.path(), kName, old, err)) << err;
        EXPECT_EQ(old, oldKey_);
    }
    EXPECT_EQ(filesNamed("liveqx_master_key.new"), 0u) << "no staging file left behind";
}

TEST_F(CredRotationTest, KeepsTheModeOfTheCredentialFile) {
    storeJwtSecret("jwt-secret-value-32-bytes-long!!!!");
    ASSERT_EQ(::chmod(cred_.c_str(), 0640), 0);
    ASSERT_EQ(rotate().rc, 0);
    struct stat st{};
    ASSERT_EQ(::stat(cred_.c_str(), &st), 0);
    EXPECT_EQ(st.st_mode & 0777, 0640u);
}

TEST_F(CredRotationTest, ReSealsTheDeviceCaUnderTheNewKey) {
    storeJwtSecret("jwt-secret-value-32-bytes-long!!!!");
    std::string cert_pem;
    {
        auto mk = keyOf(oldKey_);
        std::string err;
        auto ca = liveqx::tls::DeviceCa::open(caDir_, *mk, err);
        ASSERT_TRUE(ca) << err;
        cert_pem = ca->certPem();
    }
    ASSERT_EQ(rotate().rc, 0);

    auto mk = keyOf(credentialKey());
    std::string err;
    auto ca = liveqx::tls::DeviceCa::open(caDir_, *mk, err, /*allow_create=*/false);
    ASSERT_TRUE(ca) << err;
    EXPECT_EQ(ca->certPem(), cert_pem);
}

TEST_F(CredRotationTest, RefusesWhileTheServiceIsRunning) {
    storeJwtSecret("jwt-secret-value-32-bytes-long!!!!");
    auto lock = liveqx::util::RuntimeLock::tryAcquire(state_ / "liveqx.lock");
    ASSERT_TRUE(lock.held());
    const auto r = rotate();
    EXPECT_EQ(r.rc, 1);
    EXPECT_NE(r.err.find("running"), std::string::npos) << r.err;
    EXPECT_EQ(credentialKey(), oldKey_);
}

TEST_F(CredRotationTest, RefusesAndChangesNothingWhenASecretCannotBeDecrypted) {
    // A jwt secret encrypted with some OTHER key: the credential cannot read it.
    Key other{};
    other.fill(0x77);
    auto mk = keyOf(other);
    {
        sa::AuthDb db(db_);
        ASSERT_TRUE(db.open());
        ASSERT_TRUE(db.storeJwtSecret(mk->encrypt("secret-of-another-key"), 1));
    }
    const auto r = rotate();
    EXPECT_EQ(r.rc, 1);
    EXPECT_NE(r.err.find("cannot be decrypted"), std::string::npos) << r.err;
    EXPECT_EQ(credentialKey(), oldKey_);
    EXPECT_EQ(filesNamed("liveqx_master_key.new"), 0u);
    EXPECT_EQ(store_.encrypts, 0) << "nothing may be staged before every secret is checked";
}

TEST_F(CredRotationTest, ReportsAnUnreadableCredentialAndChangesNothing) {
    storeJwtSecret("jwt-secret-value-32-bytes-long!!!!");
    store_.fail_decrypt = true;
    const auto r = rotate();
    EXPECT_EQ(r.rc, 1);
    EXPECT_NE(r.err.find("decrypt"), std::string::npos) << r.err;
    EXPECT_EQ(store_.encrypts, 0);
}

TEST_F(CredRotationTest, MissingCredentialFileIsRefused) {
    fs::remove(cred_);
    const auto r = rotate();
    EXPECT_EQ(r.rc, 1);
    EXPECT_NE(r.err.find("not found"), std::string::npos) << r.err;
}

TEST_F(CredRotationTest, FailureToEncryptTheNewKeyLeavesTheDatabaseUntouched) {
    const auto blob_before = storeJwtSecret("jwt-secret-value-32-bytes-long!!!!");
    store_.fail_encrypt = true;
    const auto r = rotate();
    EXPECT_EQ(r.rc, 1);
    sa::AuthDb db(db_);
    ASSERT_TRUE(db.open());
    EXPECT_EQ(db.readJwtSecret().value(), blob_before)
        << "the database must not move to a key that was never saved";
    EXPECT_EQ(credentialKey(), oldKey_);
}
