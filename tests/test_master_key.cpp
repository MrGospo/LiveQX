// fix22 commit 14/24 — MasterKey load + encrypt/decrypt тесты.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "auth/MasterKey.h"

namespace fs = std::filesystem;
namespace sa = liveqx::auth;

namespace {

class MasterKeyTest : public ::testing::Test {
protected:
    fs::path tmp_;
    fs::path keyPath() const { return tmp_ / "master.key"; }

    void SetUp() override {
        tmp_ = fs::temp_directory_path() /
            ("master_key_test_" + std::to_string(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count())
             + "_" + ::testing::UnitTest::GetInstance()->current_test_info()->name());
        fs::create_directories(tmp_);
        ::unsetenv("LIVEQX_MASTER_KEY");
        ::unsetenv("LIVEQX_MASTER_KEY_FILE");
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(tmp_, ec);
        ::unsetenv("LIVEQX_MASTER_KEY");
        ::unsetenv("LIVEQX_MASTER_KEY_FILE");
    }
};

// Helper: encode raw bytes to standard base64 (no url-safe quirks).
std::string toBase64(std::string_view raw) {
    static constexpr char kAlpha[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string b64;
    int bits = 0, accum = 0;
    for (char c : raw) {
        accum = (accum << 8) | static_cast<unsigned char>(c);
        bits += 8;
        while (bits >= 6) { bits -= 6; b64.push_back(kAlpha[(accum >> bits) & 0x3F]); }
    }
    if (bits > 0) b64.push_back(kAlpha[(accum << (6 - bits)) & 0x3F]);
    while (b64.size() % 4) b64.push_back('=');
    return b64;
}

}  // namespace

TEST_F(MasterKeyTest, AutoGeneratesFileWithMode0600) {
    sa::MasterKey mk(keyPath().string());
    ASSERT_TRUE(mk.load());
    EXPECT_TRUE(mk.loaded());
    ASSERT_TRUE(fs::exists(keyPath()));

    struct stat st{};
    ASSERT_EQ(::stat(keyPath().string().c_str(), &st), 0);
    EXPECT_EQ(st.st_mode & 0777, 0600);
    EXPECT_EQ(st.st_size, static_cast<off_t>(sa::MasterKey::kKeyBytes));
}

TEST_F(MasterKeyTest, EncryptDecryptRoundTrip) {
    sa::MasterKey mk(keyPath().string());
    ASSERT_TRUE(mk.load());

    const std::string plaintext = "hunter2-very-secret-password-😀-юникод";
    auto ct = mk.encrypt(plaintext);
    ASSERT_GE(ct.size(),
              sa::MasterKey::kNonceBytes + sa::MasterKey::kMacBytes);

    auto pt = mk.decrypt(ct);
    ASSERT_TRUE(pt.has_value());
    EXPECT_EQ(*pt, plaintext);
}

TEST_F(MasterKeyTest, DecryptFailsOnTamper) {
    sa::MasterKey mk(keyPath().string());
    ASSERT_TRUE(mk.load());
    auto ct = mk.encrypt("payload");
    // Меняем байт ciphertext'а — MAC должен поймать.
    ct[ct.size() - 1] ^= 0xAA;
    EXPECT_FALSE(mk.decrypt(ct).has_value());
}

TEST_F(MasterKeyTest, DecryptFailsOnTooShortCiphertext) {
    sa::MasterKey mk(keyPath().string());
    ASSERT_TRUE(mk.load());
    std::vector<std::uint8_t> garbage(sa::MasterKey::kNonceBytes, 0);
    EXPECT_FALSE(mk.decrypt(garbage).has_value());
}

TEST_F(MasterKeyTest, ReloadingFromExistingFileGivesSameKey) {
    sa::MasterKey mk1(keyPath().string());
    ASSERT_TRUE(mk1.load());
    auto ct = mk1.encrypt("constant-plaintext");

    sa::MasterKey mk2(keyPath().string());
    ASSERT_TRUE(mk2.load());
    auto pt = mk2.decrypt(ct);
    ASSERT_TRUE(pt.has_value());
    EXPECT_EQ(*pt, "constant-plaintext");
}

TEST_F(MasterKeyTest, EnvOverridesFile) {
    // Положим в файл случайный мусор-ключ (не используется при ENV).
    {
        std::ofstream f(keyPath(), std::ios::binary);
        std::string junk(sa::MasterKey::kKeyBytes, 'A');
        f.write(junk.data(), junk.size());
    }
    // ENV-ключ — base64'нём 32 'B'.
    std::string raw_key(sa::MasterKey::kKeyBytes, 'B');
    // Простой base64 для теста.
    static constexpr char kAlpha[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string b64;
    int bits = 0, accum = 0;
    for (char c : raw_key) {
        accum = (accum << 8) | static_cast<unsigned char>(c);
        bits += 8;
        while (bits >= 6) { bits -= 6; b64.push_back(kAlpha[(accum >> bits) & 0x3F]); }
    }
    if (bits > 0) b64.push_back(kAlpha[(accum << (6 - bits)) & 0x3F]);
    while (b64.size() % 4) b64.push_back('=');
    ::setenv("LIVEQX_MASTER_KEY", b64.c_str(), /*overwrite=*/1);

    sa::MasterKey mk(keyPath().string());
    ASSERT_TRUE(mk.load());

    auto ct = mk.encrypt("env-payload");
    auto pt = mk.decrypt(ct);
    ASSERT_TRUE(pt.has_value());
    EXPECT_EQ(*pt, "env-payload");

    // Тот же ключ должен дать deterministic decrypt в другом instance.
    sa::MasterKey mk2(keyPath().string() + ".unused");
    ASSERT_TRUE(mk2.load());
    auto pt2 = mk2.decrypt(ct);
    ASSERT_TRUE(pt2.has_value());
    EXPECT_EQ(*pt2, "env-payload");
}

// ─── ENV_FILE source (systemd LoadCredentialEncrypted=) ───────────────────

TEST_F(MasterKeyTest, EnvFileRawBytesLoads) {
    const auto credPath = tmp_ / "cred.raw";
    std::string raw_key(sa::MasterKey::kKeyBytes, 'D');
    {
        std::ofstream f(credPath, std::ios::binary);
        f.write(raw_key.data(), raw_key.size());
    }
    ::setenv("LIVEQX_MASTER_KEY_FILE", credPath.string().c_str(), 1);

    sa::MasterKey mk(keyPath().string());
    ASSERT_TRUE(mk.load());
    EXPECT_EQ(mk.source(), sa::MasterKey::Source::EnvFile);

    auto ct = mk.encrypt("env-file-raw");
    auto pt = mk.decrypt(ct);
    ASSERT_TRUE(pt.has_value());
    EXPECT_EQ(*pt, "env-file-raw");
    // Auto-generate path должен НЕ сработать — файл не создан.
    EXPECT_FALSE(fs::exists(keyPath()));
}

TEST_F(MasterKeyTest, EnvFileBase64Loads) {
    const auto credPath = tmp_ / "cred.b64";
    std::string raw_key(sa::MasterKey::kKeyBytes, 'E');
    {
        std::ofstream f(credPath);
        f << toBase64(raw_key) << '\n';   // newline в конце — valid
    }
    ::setenv("LIVEQX_MASTER_KEY_FILE", credPath.string().c_str(), 1);

    sa::MasterKey mk(keyPath().string());
    ASSERT_TRUE(mk.load());
    EXPECT_EQ(mk.source(), sa::MasterKey::Source::EnvFile);

    auto ct = mk.encrypt("env-file-b64");
    auto pt = mk.decrypt(ct);
    ASSERT_TRUE(pt.has_value());
    EXPECT_EQ(*pt, "env-file-b64");
}

TEST_F(MasterKeyTest, EnvFileTakesPriorityOverLocalFile) {
    // На локальном пути — мусорный ключ, который НЕ должен использоваться.
    {
        std::ofstream f(keyPath(), std::ios::binary);
        std::string junk(sa::MasterKey::kKeyBytes, 'X');
        f.write(junk.data(), junk.size());
    }
    // ENV_FILE — настоящий ключ.
    const auto credPath = tmp_ / "cred.raw";
    std::string raw_key(sa::MasterKey::kKeyBytes, 'F');
    {
        std::ofstream f(credPath, std::ios::binary);
        f.write(raw_key.data(), raw_key.size());
    }
    ::setenv("LIVEQX_MASTER_KEY_FILE", credPath.string().c_str(), 1);

    sa::MasterKey mk(keyPath().string());
    ASSERT_TRUE(mk.load());
    EXPECT_EQ(mk.source(), sa::MasterKey::Source::EnvFile);

    // Ключ из credPath — независимый instance с тем же credPath даст
    // identical decrypt; instance, читающий keyPath напрямую (без ENV),
    // даст другой ключ.
    auto ct = mk.encrypt("priority-test");

    sa::MasterKey mk_via_envfile(tmp_ / "unused.key");
    ASSERT_TRUE(mk_via_envfile.load());
    auto pt_match = mk_via_envfile.decrypt(ct);
    ASSERT_TRUE(pt_match.has_value());
    EXPECT_EQ(*pt_match, "priority-test");

    ::unsetenv("LIVEQX_MASTER_KEY_FILE");
    sa::MasterKey mk_via_localfile(keyPath().string());
    ASSERT_TRUE(mk_via_localfile.load());  // загрузит junk-ключ
    auto pt_mismatch = mk_via_localfile.decrypt(ct);
    EXPECT_FALSE(pt_mismatch.has_value());
}

TEST_F(MasterKeyTest, EnvFileEnvOverEnvFile) {
    // ENV (raw) имеет приоритет над ENV_FILE.
    std::string env_key(sa::MasterKey::kKeyBytes, 'G');
    ::setenv("LIVEQX_MASTER_KEY", toBase64(env_key).c_str(), 1);

    const auto credPath = tmp_ / "cred.raw";
    {
        std::ofstream f(credPath, std::ios::binary);
        std::string other(sa::MasterKey::kKeyBytes, 'H');
        f.write(other.data(), other.size());
    }
    ::setenv("LIVEQX_MASTER_KEY_FILE", credPath.string().c_str(), 1);

    sa::MasterKey mk(keyPath().string());
    ASSERT_TRUE(mk.load());
    EXPECT_EQ(mk.source(), sa::MasterKey::Source::Env);
}

TEST_F(MasterKeyTest, EnvFileMissingPathIsHardError) {
    ::setenv("LIVEQX_MASTER_KEY_FILE",
             (tmp_ / "does-not-exist").string().c_str(), 1);

    sa::MasterKey mk(keyPath().string());
    EXPECT_FALSE(mk.load());
    EXPECT_FALSE(mk.loaded());
    // Авто-генерация по local path не должна была сработать —
    // ENV_FILE задан явно, тихий fallback скрыл бы misconfiguration.
    EXPECT_FALSE(fs::exists(keyPath()));
}

TEST_F(MasterKeyTest, EnvFileEmptyContentIsHardError) {
    const auto credPath = tmp_ / "cred.empty";
    { std::ofstream f(credPath); }  // 0 bytes
    ::setenv("LIVEQX_MASTER_KEY_FILE", credPath.string().c_str(), 1);

    sa::MasterKey mk(keyPath().string());
    EXPECT_FALSE(mk.load());
    EXPECT_FALSE(mk.loaded());
}

TEST_F(MasterKeyTest, EnvFileGarbageContentIsHardError) {
    const auto credPath = tmp_ / "cred.garbage";
    {
        std::ofstream f(credPath);
        // 40 байт — не raw key (не 32) и не valid base64 для 32-byte key.
        f << "this-is-clearly-not-a-key-or-base64!!!@@";
    }
    ::setenv("LIVEQX_MASTER_KEY_FILE", credPath.string().c_str(), 1);

    sa::MasterKey mk(keyPath().string());
    EXPECT_FALSE(mk.load());
}

TEST_F(MasterKeyTest, GetInfoEnvFileSourceHasNullCreatedAt) {
    const auto credPath = tmp_ / "cred.raw";
    std::string raw_key(sa::MasterKey::kKeyBytes, 'I');
    {
        std::ofstream f(credPath, std::ios::binary);
        f.write(raw_key.data(), raw_key.size());
    }
    ::setenv("LIVEQX_MASTER_KEY_FILE", credPath.string().c_str(), 1);

    sa::MasterKey mk(keyPath().string());
    ASSERT_TRUE(mk.load());
    const auto info = mk.getInfo();
    EXPECT_EQ(info.source, "env_file");
    EXPECT_FALSE(info.created_at.has_value());
    EXPECT_EQ(info.fingerprint.size(), 16u);
}

TEST_F(MasterKeyTest, MalformedEnvIsRejected) {
    ::setenv("LIVEQX_MASTER_KEY", "not-base64-and-too-short!", 1);
    sa::MasterKey mk(keyPath().string());
    EXPECT_FALSE(mk.load());
    EXPECT_FALSE(mk.loaded());
}

// ─── fix32 B3 — getInfo() ──────────────────────────────────────────────────

TEST_F(MasterKeyTest, GetInfoReportsAlgorithmSourceAndStableFingerprint) {
    sa::MasterKey mk(keyPath().string());
    ASSERT_TRUE(mk.load());

    const auto info = mk.getInfo();
    EXPECT_EQ(info.algorithm, "xsalsa20poly1305");
    EXPECT_EQ(info.source, "file");
    // 16 hex chars = first 8 bytes of sha256(key).
    ASSERT_EQ(info.fingerprint.size(), 16u);
    for (char c : info.fingerprint) {
        EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))
            << "fingerprint must be lowercase hex: '" << c << "'";
    }
    // created_at должен совпадать с mtime файла.
    ASSERT_TRUE(info.created_at.has_value());
    struct stat st{};
    ASSERT_EQ(::stat(keyPath().string().c_str(), &st), 0);
    EXPECT_EQ(*info.created_at, static_cast<std::int64_t>(st.st_mtime));
    // Без .bak — last_rotated_at пустой.
    EXPECT_FALSE(info.last_rotated_at.has_value());
}

TEST_F(MasterKeyTest, GetInfoFingerprintIsStableAcrossInstances) {
    sa::MasterKey mk1(keyPath().string());
    ASSERT_TRUE(mk1.load());
    const auto fp1 = mk1.getInfo().fingerprint;

    sa::MasterKey mk2(keyPath().string());
    ASSERT_TRUE(mk2.load());
    const auto fp2 = mk2.getInfo().fingerprint;

    EXPECT_EQ(fp1, fp2);
}

TEST_F(MasterKeyTest, GetInfoFingerprintDiffersAcrossKeys) {
    // Один ключ — auto-generate.
    sa::MasterKey mk1(keyPath().string());
    ASSERT_TRUE(mk1.load());
    const auto fp1 = mk1.getInfo().fingerprint;

    // Удаляем файл и грузим заново — будет другой ключ.
    fs::remove(keyPath());
    sa::MasterKey mk2(keyPath().string());
    ASSERT_TRUE(mk2.load());
    const auto fp2 = mk2.getInfo().fingerprint;

    EXPECT_NE(fp1, fp2);
}

TEST_F(MasterKeyTest, GetInfoEnvSourceHasNullCreatedAt) {
    // ENV-loaded key: created_at не имеет смысла (файла нет).
    std::string raw_key(sa::MasterKey::kKeyBytes, 'C');
    static constexpr char kAlpha[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string b64;
    int bits = 0, accum = 0;
    for (char c : raw_key) {
        accum = (accum << 8) | static_cast<unsigned char>(c);
        bits += 8;
        while (bits >= 6) { bits -= 6; b64.push_back(kAlpha[(accum >> bits) & 0x3F]); }
    }
    if (bits > 0) b64.push_back(kAlpha[(accum << (6 - bits)) & 0x3F]);
    while (b64.size() % 4) b64.push_back('=');
    ::setenv("LIVEQX_MASTER_KEY", b64.c_str(), 1);

    sa::MasterKey mk(keyPath().string());
    ASSERT_TRUE(mk.load());
    const auto info = mk.getInfo();
    EXPECT_EQ(info.source, "env");
    EXPECT_FALSE(info.created_at.has_value());
    EXPECT_FALSE(info.last_rotated_at.has_value());
    EXPECT_EQ(info.fingerprint.size(), 16u);
}

TEST_F(MasterKeyTest, GetInfoLastRotatedAtPicksNewestBackup) {
    sa::MasterKey mk(keyPath().string());
    ASSERT_TRUE(mk.load());

    // Создаём два .bak с разными mtime — getInfo должен взять самый свежий.
    const auto older_path = keyPath().string() + ".1700000000.bak";
    const auto newer_path = keyPath().string() + ".1800000000.bak";
    {
        std::ofstream f1(older_path); f1 << "old";
        std::ofstream f2(newer_path); f2 << "new";
    }
    // Принудительно ставим mtime — иначе оба получат now().
    struct timespec older_ts[2] = { {1700000000, 0}, {1700000000, 0} };
    struct timespec newer_ts[2] = { {1800000000, 0}, {1800000000, 0} };
    ASSERT_EQ(::utimensat(AT_FDCWD, older_path.c_str(), older_ts, 0), 0);
    ASSERT_EQ(::utimensat(AT_FDCWD, newer_path.c_str(), newer_ts, 0), 0);

    const auto info = mk.getInfo();
    ASSERT_TRUE(info.last_rotated_at.has_value());
    EXPECT_EQ(*info.last_rotated_at, 1800000000);

    // Контроль: левый файл с похожим именем не учитывается.
    std::ofstream junk(keyPath().string() + ".swp.bak"); junk << "junk";
    const auto info2 = mk.getInfo();
    ASSERT_TRUE(info2.last_rotated_at.has_value());
    EXPECT_EQ(*info2.last_rotated_at, 1800000000);
}