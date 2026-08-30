// fix22 commit 14/24 — libsodium master key для encryption-at-rest.
//
// Что мы шифруем:
//   - jwt_secret.secret_encrypted   (commit 14)
//   - ldap_config.bind_password_encrypted (commit 17)
//   - smtp config password           (commit 23)
//
// Чем шифруем: crypto_secretbox_easy (XSalsa20-Poly1305).
//   key = 32 bytes, nonce = 24 bytes (rand), mac = 16 bytes.
// Storage layout: nonce(24) || ciphertext+mac(N+16). Никакой base64 —
// это BLOB-колонка sqlite, raw bytes.
//
// Источник ключа (приоритет):
//   1. ENV LIVEQX_MASTER_KEY — base64-кодированный 32-byte ключ.
//      Production way: ключ инжектится оператором/secret-store'ом. Простой,
//      но env-vars видны через /proc/<pid>/environ — на multi-tenant хосте
//      это leak.
//   2. ENV LIVEQX_MASTER_KEY_FILE — путь к файлу с ключом. Файл
//      может содержать либо raw 32 bytes, либо base64-encoded 32-byte ключ
//      (определяется по размеру). Этот путь рассчитан на systemd
//      LoadCredentialEncrypted= — systemd кладёт расшифрованный credential
//      в %d/<name> (tmpfs, недоступный другим процессам), а unit
//      экспортирует LIVEQX_MASTER_KEY_FILE=%d/<name>. Тогда мастер-
//      ключ зашифрован на диске TPM-bound key'ём от systemd-creds.
//   3. Файл `key_file_path_` mode 0600 — single-host fallback.
//      Файл хранит raw 32 bytes.
//   4. Auto-generate файла с mode 0600. Только для dev/первого запуска;
//      в проде это плохая идея, потому что ключ окажется на том же диске
//      что и DB. Но без auto-gen первый запуск процесса невозможен —
//      этот компромисс обсуждался в design-doc fix22.
//
// load() возвращает false на любой жёсткой ошибке (файл недоступен на
// запись, env value поломан) — caller обязан остановить старт сервера.

#include "auth/MasterKey.h"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <sodium.h>

#include "utils/Log.h"

namespace liveqx::auth {
namespace {

// Минимальный base64 decode без зависимостей. Принимает «std», «url» —
// допускаем и '+/' и '-_'. Padding опционален. Возвращает nullopt на мусор.
std::optional<std::vector<std::uint8_t>> base64Decode(std::string_view in) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+' || c == '-') return 62;
        if (c == '/' || c == '_') return 63;
        return -1;
    };
    std::vector<std::uint8_t> out;
    out.reserve((in.size() * 3) / 4 + 3);
    int bits = 0, accum = 0;
    for (char c : in) {
        if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
        int v = val(c);
        if (v < 0) return std::nullopt;
        accum = (accum << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>((accum >> bits) & 0xFF));
        }
    }
    return out;
}

// Гарантированное обнуление stack-buffer'а после использования (Argon2-style
// hygiene). sodium_memzero не ставит барьер компилятора, но libsodium
// гарантирует, что обнуление не выкинется оптимизатором.
void wipe(std::array<std::uint8_t, MasterKey::kKeyBytes>& buf) {
    sodium_memzero(buf.data(), buf.size());
}

bool ensureSodiumInit() {
    static const bool ok = (sodium_init() >= 0);
    return ok;
}

// Возвращает true и mode-биты, если файл существует. False если файла нет.
bool statFileMode(const std::string& path, mode_t& out_mode) {
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) return false;
    out_mode = st.st_mode;
    return true;
}

}  // namespace

MasterKey::MasterKey(std::string key_file_path)
    : key_file_path_(std::move(key_file_path)) {}

bool MasterKey::load() {
    if (!ensureSodiumInit()) {
        LOG_ERROR("MasterKey: sodium_init failed");
        return false;
    }

    // (1) ENV.
    if (const char* env = std::getenv("LIVEQX_MASTER_KEY")) {
        auto decoded = base64Decode(env);
        if (!decoded || decoded->size() != kKeyBytes) {
            LOG_ERROR("MasterKey: LIVEQX_MASTER_KEY malformed "
                      "(base64-encoded {}-byte key required)", kKeyBytes);
            return false;
        }
        std::memcpy(key_.data(), decoded->data(), kKeyBytes);
        sodium_memzero(decoded->data(), decoded->size());
        loaded_ = true;
        source_ = Source::Env;
        LOG_INFO("MasterKey: loaded from LIVEQX_MASTER_KEY env");
        return true;
    }

    // (2) ENV path → файл-credential. Чтобы дружить с systemd
    // LoadCredentialEncrypted= (см. header). Файл может быть:
    //   - ровно kKeyBytes байт → raw key
    //   - иначе → пробуем base64-decode (с trailing whitespace / newline)
    // Жёсткая ошибка, если ENV задана, но файл нечитаем / содержит мусор:
    // оператор явно попросил этот источник, тихий fallback на (3) скрыл бы
    // misconfiguration.
    if (const char* env_file = std::getenv("LIVEQX_MASTER_KEY_FILE")) {
        std::string path(env_file);
        if (path.empty()) {
            LOG_ERROR("MasterKey: LIVEQX_MASTER_KEY_FILE is empty");
            return false;
        }
        std::ifstream f(path, std::ios::binary);
        if (!f) {
            LOG_ERROR("MasterKey: cannot open LIVEQX_MASTER_KEY_FILE "
                      "{} for reading: {}", path, std::strerror(errno));
            return false;
        }
        // Читаем целиком, но ставим разумный потолок чтобы не сожрать
        // случайно гигабайтный файл.
        constexpr std::size_t kMaxBytes = 4096;
        std::vector<char> buf(kMaxBytes);
        f.read(buf.data(), buf.size());
        const auto n = static_cast<std::size_t>(f.gcount());
        if (n == 0) {
            LOG_ERROR("MasterKey: LIVEQX_MASTER_KEY_FILE {} is empty",
                      path);
            return false;
        }
        if (n == kMaxBytes && !f.eof()) {
            LOG_ERROR("MasterKey: LIVEQX_MASTER_KEY_FILE {} larger "
                      "than {} bytes — refusing", path, kMaxBytes);
            sodium_memzero(buf.data(), buf.size());
            return false;
        }

        if (n == kKeyBytes) {
            // Raw 32 bytes.
            std::memcpy(key_.data(), buf.data(), kKeyBytes);
            sodium_memzero(buf.data(), buf.size());
            loaded_ = true;
            source_ = Source::EnvFile;
            LOG_INFO("MasterKey: loaded raw key from "
                     "LIVEQX_MASTER_KEY_FILE={}", path);
            return true;
        }
        // Иначе — пробуем base64. base64Decode сам игнорит \n/\r/space/=.
        std::string_view view(buf.data(), n);
        auto decoded = base64Decode(view);
        sodium_memzero(buf.data(), buf.size());
        if (!decoded || decoded->size() != kKeyBytes) {
            LOG_ERROR("MasterKey: LIVEQX_MASTER_KEY_FILE {} content "
                      "is neither raw {}-byte key nor base64-encoded {}-byte "
                      "key (got {} bytes)", path, kKeyBytes, kKeyBytes, n);
            return false;
        }
        std::memcpy(key_.data(), decoded->data(), kKeyBytes);
        sodium_memzero(decoded->data(), decoded->size());
        loaded_ = true;
        source_ = Source::EnvFile;
        LOG_INFO("MasterKey: loaded base64 key from "
                 "LIVEQX_MASTER_KEY_FILE={}", path);
        return true;
    }

    // (3) Файл.
    {
        mode_t mode = 0;
        if (statFileMode(key_file_path_, mode)) {
            // Жалуемся при слишком открытых правах, но не блокируем
            // запуск — на dev-машинах оператор иногда правит вручную.
            const auto perm_bits = mode & 0777;
            if ((perm_bits & 0077) != 0) {
                LOG_WARN("MasterKey: {} has loose permissions {:#o} "
                         "(recommend 0600)",
                         key_file_path_, perm_bits);
            }

            std::ifstream f(key_file_path_, std::ios::binary);
            if (!f) {
                LOG_ERROR("MasterKey: cannot open {} for reading",
                          key_file_path_);
                return false;
            }
            f.read(reinterpret_cast<char*>(key_.data()), kKeyBytes);
            if (f.gcount() != static_cast<std::streamsize>(kKeyBytes)) {
                LOG_ERROR("MasterKey: file {} is {}-bytes, expected {}",
                          key_file_path_, f.gcount(), kKeyBytes);
                wipe(key_);
                return false;
            }
            loaded_ = true;
            source_ = Source::File;
            LOG_INFO("MasterKey: loaded from file {}", key_file_path_);
            return true;
        }
    }

    // (4) Auto-generate. Создаём родительскую директорию, кидаем ключ
    // через libsodium, пишем mode 0600.
    std::error_code ec;
    auto parent = std::filesystem::path(key_file_path_).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, ec);

    std::array<std::uint8_t, kKeyBytes> fresh{};
    randombytes_buf(fresh.data(), fresh.size());

    // O_CREAT|O_WRONLY|O_EXCL чтобы случайно не перезаписать чужой
    // файл (race с параллельным процессом). Mode 0600 ставится atomic'но.
    int fd = ::open(key_file_path_.c_str(),
                    O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        LOG_ERROR("MasterKey: cannot create {}: {}",
                  key_file_path_, std::strerror(errno));
        wipe(fresh);
        return false;
    }
    const auto written = ::write(fd, fresh.data(), fresh.size());
    if (written != static_cast<ssize_t>(kKeyBytes)) {
        LOG_ERROR("MasterKey: short write {}/{} to {}",
                  written, kKeyBytes, key_file_path_);
        ::close(fd);
        wipe(fresh);
        std::filesystem::remove(key_file_path_, ec);
        return false;
    }
    if (::fsync(fd) != 0) {
        LOG_WARN("MasterKey: fsync failed on {}: {}",
                 key_file_path_, std::strerror(errno));
    }
    ::close(fd);
    // Принудительно ставим 0600 — на случай, если umask повлиял на
    // O_CREAT (некоторые ядра учитывают umask поверх explicit mode).
    ::chmod(key_file_path_.c_str(), 0600);

    std::memcpy(key_.data(), fresh.data(), kKeyBytes);
    sodium_memzero(fresh.data(), fresh.size());
    loaded_ = true;
    source_ = Source::File;
    LOG_WARN("MasterKey: auto-generated new master key at {} — "
             "ROTATE in production via CLI tool (commit 15/24)",
             key_file_path_);
    return true;
}

std::vector<std::uint8_t> MasterKey::encrypt(std::string_view plaintext) const {
    if (!loaded_) return {};

    std::vector<std::uint8_t> out;
    out.resize(kNonceBytes + plaintext.size() + kMacBytes);

    randombytes_buf(out.data(), kNonceBytes);
    if (crypto_secretbox_easy(
            out.data() + kNonceBytes,
            reinterpret_cast<const unsigned char*>(plaintext.data()),
            plaintext.size(),
            out.data(),
            key_.data()) != 0) {
        // crypto_secretbox_easy сейчас не возвращает ошибку при
        // правильных аргументах — но обрабатываем для будущих версий.
        return {};
    }
    return out;
}

namespace {

// 16 hex chars (8 bytes) of sha256(key). Short enough to compare visually
// in the UI; one-way preimage stays infeasible (full hash is 256 bits,
// the truncation just shrinks the human-facing label).
std::string keyFingerprint(const std::array<std::uint8_t, MasterKey::kKeyBytes>& key) {
    std::array<std::uint8_t, crypto_hash_sha256_BYTES> digest{};
    crypto_hash_sha256(digest.data(), key.data(), key.size());
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(16);
    for (std::size_t i = 0; i < 8; ++i) {
        out.push_back(kHex[(digest[i] >> 4) & 0xF]);
        out.push_back(kHex[digest[i] & 0xF]);
    }
    return out;
}

// Best-effort scan of the key-file directory for `<basename>.<digits>.bak`
// entries left behind by MasterKeyReset. Returns the newest mtime in
// unix-seconds, or nullopt if no backups exist (or the directory is gone).
std::optional<std::int64_t> latestBackupMtime(const std::string& key_file_path) {
    namespace fs = std::filesystem;
    fs::path key_path(key_file_path);
    fs::path dir = key_path.parent_path();
    if (dir.empty()) dir = fs::path(".");
    std::error_code ec;
    if (!fs::exists(dir, ec)) return std::nullopt;

    const std::string base = key_path.filename().string();
    const std::string prefix = base + ".";  // e.g. "master.key."
    std::optional<std::int64_t> newest;

    for (auto it = fs::directory_iterator(dir, ec);
         !ec && it != fs::directory_iterator(); it.increment(ec)) {
        const auto name = it->path().filename().string();
        if (name.size() <= prefix.size() + 4) continue;     // need ".bak"
        if (name.compare(0, prefix.size(), prefix) != 0) continue;
        if (name.compare(name.size() - 4, 4, ".bak") != 0) continue;
        // Middle segment must be an unsigned decimal — guards against
        // unrelated files like "master.key.swp.bak" leaking in.
        const auto mid_begin = prefix.size();
        const auto mid_end   = name.size() - 4;
        bool digits_only = mid_begin < mid_end;
        for (auto i = mid_begin; i < mid_end && digits_only; ++i) {
            if (name[i] < '0' || name[i] > '9') digits_only = false;
        }
        if (!digits_only) continue;

        struct stat st{};
        if (::stat(it->path().string().c_str(), &st) != 0) continue;
        const auto mt = static_cast<std::int64_t>(st.st_mtime);
        if (!newest || mt > *newest) newest = mt;
    }
    return newest;
}

}  // namespace

MasterKey::Info MasterKey::getInfo() const {
    Info info;
    info.algorithm = "xsalsa20poly1305";
    switch (source_) {
        case Source::Env:     info.source = "env";      break;
        case Source::EnvFile: info.source = "env_file"; break;
        case Source::File:    info.source = "file";     break;
        case Source::Unknown: info.source = "";         break;
    }

    if (loaded_) {
        info.fingerprint = keyFingerprint(key_);
    }

    // created_at: only meaningful for file-loaded keys. For ENV / EnvFile
    // sources we leave it null — for ENV the operator's secret-store
    // decides how to age the secret; for EnvFile the path is typically a
    // systemd-tmpfs credential whose mtime equals "last service start"
    // and would mislead the admin UI.
    if (source_ == Source::File) {
        struct stat st{};
        if (::stat(key_file_path_.c_str(), &st) == 0) {
            info.created_at = static_cast<std::int64_t>(st.st_mtime);
        }
    }

    info.last_rotated_at = latestBackupMtime(key_file_path_);
    return info;
}

std::array<std::uint8_t, 32> MasterKey::hmacSha256(std::string_view msg) const {
    std::array<std::uint8_t, 32> out{};
    if (!loaded_) return out;
    static_assert(crypto_auth_hmacsha256_BYTES == 32,
                  "hmacsha256 output must be 32 bytes");
    // crypto_auth_hmacsha256_KEYBYTES == 32 by design — matches kKeyBytes.
    crypto_auth_hmacsha256(
        out.data(),
        reinterpret_cast<const unsigned char*>(msg.data()),
        msg.size(),
        key_.data());
    return out;
}

std::string MasterKey::fingerprint() const {
    if (!loaded_) return {};
    return keyFingerprint(key_);
}

std::optional<std::string>
MasterKey::decrypt(const std::vector<std::uint8_t>& ciphertext) const {
    if (!loaded_) return std::nullopt;
    if (ciphertext.size() < kNonceBytes + kMacBytes) return std::nullopt;

    const auto body_len = ciphertext.size() - kNonceBytes;
    std::vector<std::uint8_t> plain(body_len - kMacBytes);
    if (crypto_secretbox_open_easy(
            plain.data(),
            ciphertext.data() + kNonceBytes,
            body_len,
            ciphertext.data(),
            key_.data()) != 0) {
        // wrong key или tamper.
        return std::nullopt;
    }
    return std::string(reinterpret_cast<const char*>(plain.data()),
                       plain.size());
}

}  // namespace liveqx::auth
