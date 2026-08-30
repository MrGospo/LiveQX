#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace liveqx::auth {

// libsodium master key for crypto_secretbox (XSalsa20-Poly1305).
// Encrypts: ldap_config.bind_password, jwt_secret, smtp.password.
// Argon2id password hashes are NOT encrypted (already plaintext-safe).
//
// Source priority on load():
//   1. ENV LIVEQX_MASTER_KEY (base64, 32 bytes) — production.
//   2. file mode 0600 (default state/master.key) — single-host fallback.
//   3. auto-generate the file if neither (1) nor (2) is present (dev only).
//
// Implementation lands in commit 14/24 of fix22.
class MasterKey {
public:
    static constexpr std::size_t kKeyBytes   = 32;  // crypto_secretbox_KEYBYTES
    static constexpr std::size_t kNonceBytes = 24;  // crypto_secretbox_NONCEBYTES
    static constexpr std::size_t kMacBytes   = 16;  // crypto_secretbox_MACBYTES

    enum class Source { Unknown, File, Env, EnvFile };

    // Snapshot of master-key metadata for the admin UI / REST endpoint.
    // Never exposes raw key material — only a short fingerprint over a
    // SHA-256 of the key, plus on-disk timestamps that prove "the key on
    // disk is still the same one we deployed".
    struct Info {
        std::string fingerprint;                       // 16 hex chars = first 8 bytes of sha256(key)
        std::string algorithm;                         // "xsalsa20poly1305"
        std::string source;                            // "file" | "env" | "env_file"
        std::optional<std::int64_t> created_at;        // unix-sec mtime of key file, null when source=env
        std::optional<std::int64_t> last_rotated_at;   // unix-sec mtime of newest .bak in key dir, null if none
    };

    explicit MasterKey(std::string key_file_path);

    // Load from env / file / auto-generate. Returns false on any error
    // that should keep the server from starting (file write fails, env
    // value malformed).
    bool load();

    // Read-only metadata for /api/auth/master-key/info. load() must have
    // succeeded — otherwise the fingerprint would lie about an empty key.
    // No throw: when the underlying file is gone (e.g. ENV-loaded key),
    // created_at is left empty; last_rotated_at always best-effort.
    Info getInfo() const;

    Source source() const noexcept { return source_; }

    // Encrypt produces nonce (24B) || ciphertext || mac, base64-safe via
    // raw bytes (callers store in BLOB columns).
    std::vector<std::uint8_t> encrypt(std::string_view plaintext) const;

    // Decrypt — opposite of encrypt(). std::nullopt on tamper / wrong key.
    std::optional<std::string> decrypt(const std::vector<std::uint8_t>& ciphertext) const;

    // HMAC-SHA256 keyed on the master key. Used by the audit-log hash
    // chain: mac[N] = HMAC(master_key, prev_mac || canonical(record[N])).
    // Returns 32 raw bytes. Empty array iff !loaded().
    std::array<std::uint8_t, 32> hmacSha256(std::string_view msg) const;

    // Short fingerprint of the current key — same 16-hex-char form used
    // by getInfo(). Stored in every audit row so operators can prove
    // which key epoch signed which chain segment across rotations.
    std::string fingerprint() const;

    bool loaded() const noexcept { return loaded_; }

private:
    std::string key_file_path_;
    std::array<std::uint8_t, kKeyBytes> key_{};
    bool loaded_{false};
    Source source_{Source::Unknown};
};

}  // namespace liveqx::auth
