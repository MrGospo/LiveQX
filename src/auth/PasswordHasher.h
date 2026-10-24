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

    // Every hash and every check takes 64 MiB for its short life. A burst of logins (wrong passwords
    // too: the hash is computed before the answer is known) would hold one such buffer per worker thread
    // of the HTTP server at once, so only this many run at the same time; the rest wait their turn
    // (a login takes about a tenth of a second).
    static constexpr int kMaxConcurrent = 4;
    // For tests: the most that ran at once since the last reset.
    static int  peakInFlight();
    static void resetPeak();
};

}  // namespace liveqx::auth
