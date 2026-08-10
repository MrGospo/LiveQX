// fix22 commit 3/24 — argon2id wrapper через libsodium.
//
// libsodium реализует argon2id под `crypto_pwhash` API:
//   crypto_pwhash_str()       — hash + encode в "$argon2id$..." строку.
//   crypto_pwhash_str_verify()— constant-time verify против encoded string.
//   randombytes_buf()         — RNG из /dev/urandom (через kernel CSPRNG).
//
// Параметры:
//   OPSLIMIT_INTERACTIVE = 2 passes
//   MEMLIMIT_INTERACTIVE = 64 MiB
//   ALG = ARGON2ID13         (через crypto_pwhash_str_alg)
//
// Эти значения соответствуют рекомендации libsodium для interactive
// логин-цикла: ~50ms на современном CPU, на 256-ядерном EPYC падает до
// ~10ms. Хост держит десятки login'ов/сек без проблем — это не bottleneck.
//
// `crypto_pwhash_str_alg` явно фиксирует ALGORITHM на ARGON2ID. На
// libsodium ≥ 1.0.15 default уже ARGON2ID, но фиксируем для
// предсказуемого поведения при апгрейде версии.
//
// generateRandom: 24 байта → URL-safe base64 без padding ≈ 32 ascii chars.
// Используется для (a) bootstrap admin password (commit 9) и (b)
// admin-triggered reset (commit 7).

#include "auth/PasswordHasher.h"

#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

#include <sodium.h>

#include "utils/Log.h"

namespace liveqx::auth {
namespace {

// libsodium требует sodium_init() перед первым использованием. init
// идемпотентен и thread-safe внутри, но мы оборачиваем в std::once_flag
// чтобы не зависеть от внутренних деталей и логировать одно сообщение.
std::once_flag g_sodium_init_flag;
bool           g_sodium_ok = false;

void ensureSodiumInit() {
    std::call_once(g_sodium_init_flag, []() {
        const int rc = sodium_init();
        // sodium_init возвращает 0 при первом успехе, 1 если уже init,
        // -1 при ошибке (отсутствие /dev/urandom и т.п.).
        if (rc < 0) {
            LOG_ERROR("PasswordHasher: sodium_init failed");
            g_sodium_ok = false;
        } else {
            g_sodium_ok = true;
        }
    });
}

}  // namespace

std::optional<std::string> PasswordHasher::hash(std::string_view plaintext) {
    ensureSodiumInit();
    if (!g_sodium_ok) return std::nullopt;
    if (plaintext.empty()) return std::nullopt;

    // crypto_pwhash_STRBYTES — максимальная длина encoded строки
    // (включая trailing '\0'). Для argon2id это 128 байт.
    char encoded[crypto_pwhash_STRBYTES] = {0};

    const int rc = crypto_pwhash_str_alg(
        encoded,
        plaintext.data(),
        plaintext.size(),
        crypto_pwhash_OPSLIMIT_INTERACTIVE,
        crypto_pwhash_MEMLIMIT_INTERACTIVE,
        crypto_pwhash_ALG_ARGON2ID13);

    if (rc != 0) {
        // Единственная задокументированная причина — нехватка памяти под
        // argon2 buffer (64 MiB). На 256-ядерном EPYC такого не бывает,
        // но логируем на всякий случай.
        LOG_ERROR("PasswordHasher::hash crypto_pwhash_str_alg failed");
        return std::nullopt;
    }
    return std::string(encoded);
}

bool PasswordHasher::verify(std::string_view plaintext,
                            std::string_view encoded_hash) {
    ensureSodiumInit();
    if (!g_sodium_ok) return false;
    if (plaintext.empty() || encoded_hash.empty()) return false;

    // crypto_pwhash_str_verify требует null-terminated encoded string.
    // Делаем копию — не используем data(), потому что string_view может
    // указывать на не-null-terminated буфер.
    std::string encoded_copy(encoded_hash);
    if (encoded_copy.size() >= crypto_pwhash_STRBYTES) {
        // Слишком длинная encoded-строка — хеш точно не наш формат.
        return false;
    }

    const int rc = crypto_pwhash_str_verify(
        encoded_copy.c_str(),
        plaintext.data(),
        plaintext.size());
    return rc == 0;
}

std::string PasswordHasher::generateRandom() {
    ensureSodiumInit();
    if (!g_sodium_ok) return {};

    // 24 random bytes → URL-safe base64 ≈ 32 ascii chars.
    constexpr std::size_t kRandomBytes = 24;
    unsigned char buf[kRandomBytes];
    randombytes_buf(buf, sizeof(buf));

    // sodium_base64_VARIANT_URLSAFE_NO_PADDING — gives strings безопасные
    // для копирования в URL/JSON без quoting'а. ENCODED_LEN включает
    // trailing '\0'.
    const std::size_t encoded_max =
        sodium_base64_ENCODED_LEN(kRandomBytes, sodium_base64_VARIANT_URLSAFE_NO_PADDING);
    std::vector<char> out(encoded_max);

    const char* enc = sodium_bin2base64(
        out.data(), out.size(),
        buf, sizeof(buf),
        sodium_base64_VARIANT_URLSAFE_NO_PADDING);

    // sodium_memzero чтобы random bytes не лежали в стеке после возврата.
    sodium_memzero(buf, sizeof(buf));

    if (!enc) {
        LOG_ERROR("PasswordHasher::generateRandom sodium_bin2base64 failed");
        return {};
    }
    return std::string(out.data());
}

}  // namespace liveqx::auth
