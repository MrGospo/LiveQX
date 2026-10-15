#pragma once
//
// Passphrase wrapping of the master key (file format "LQXKEY1").
//
// The 32-byte data key K stays exactly what it was — every blob in the
// databases is still encrypted with K. Wrapping only changes how K is stored:
// a key-encryption key (KEK) is derived from the operator's passphrase with
// Argon2id, and K is sealed under it with crypto_secretbox. Changing the
// passphrase re-wraps this one small blob; nothing else is touched.
//
// File layout:
//   "LQXKEY1\n"  followed by a JSON object
//   {
//     "version": 1,
//     "kdf": { "alg": "argon2id13", "salt": <base64, 16 bytes>,
//              "opslimit": <int>, "memlimit": <bytes> },
//     "wrapped": <base64: nonce(24) || secretbox(K)>,
//     "fingerprint": <hex, first 8 bytes of sha256(K)>
//   }
// A file of exactly 32 bytes without the magic is the legacy bare key.
//
// Everything here is pure: no files, no environment.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace liveqx::auth::keywrap {

inline constexpr std::string_view kMagic = "LQXKEY1\n";
inline constexpr std::size_t kKeyBytes = 32;

using Key = std::array<std::uint8_t, kKeyBytes>;

struct KdfParams {
    unsigned long long opslimit{0};
    unsigned long long memlimit{0};   // bytes

    // libsodium "moderate": the default for real files (~0.7 s, 256 MiB).
    static KdfParams moderate();
    // libsodium minimum: for tests only.
    static KdfParams minimal();
};

enum class UnwrapStatus {
    Ok,
    NotWrapped,        // no magic: not an LQXKEY1 file
    Malformed,         // magic present but the body is not valid
    BadParams,         // KDF parameters outside the accepted range
    WrongPassphrase,   // decryption failed: wrong passphrase or damaged file
};

// True if `file_bytes` starts with the LQXKEY1 magic.
bool isWrapped(std::string_view file_bytes);

// Seals `key` under `passphrase`. Returns the complete file contents, or an
// empty string if the passphrase is empty, the parameters are out of range or
// libsodium fails.
std::string wrap(const Key& key, std::string_view passphrase,
                 const KdfParams& params = KdfParams::moderate());

// Opens a wrapped file. On Ok, `out` holds K. The fingerprint stored in the
// file is checked after decryption, so a file whose blob was swapped for
// another valid one is rejected too.
UnwrapStatus unwrap(std::string_view file_bytes, std::string_view passphrase,
                    Key& out);

// Short fingerprint of a key: first 8 bytes of sha256(K), lowercase hex.
std::string fingerprintOf(const Key& key);

}  // namespace liveqx::auth::keywrap
