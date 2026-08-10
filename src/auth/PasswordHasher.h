#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace liveqx::auth {

// argon2id wrapper around libsodium's crypto_pwhash. Lands in commit 3/24
// of fix22; the skeleton fixes the interface so AuthService can be
// written against a stable signature.
class PasswordHasher {
public:
    // Hash a plaintext password. Returns the encoded argon2id string
    // ($argon2id$v=19$m=...$t=...$p=...$salt$hash) suitable for storing
    // in users.password_hash. std::nullopt on memory exhaustion.
    static std::optional<std::string> hash(std::string_view plaintext);

    // Verify plaintext against an encoded hash from the database. Returns
    // false on mismatch, malformed hash, or libsodium error.
    static bool verify(std::string_view plaintext, std::string_view encoded_hash);

    // Generate a cryptographically random password (RFC 4648 base64,
    // ~32 chars from 24 random bytes) — used for bootstrap admin and
    // admin-triggered resets.
    static std::string generateRandom();
};

}  // namespace liveqx::auth
