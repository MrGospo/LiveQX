// Passphrase wrapping of the master key — see auth/KeyWrap.h for the format.

#include "auth/KeyWrap.h"

#include <nlohmann/json.hpp>

#include <sodium.h>

#include <cstring>
#include <vector>

namespace liveqx::auth::keywrap {
namespace {

using nlohmann::json;

constexpr unsigned long long kMaxOpslimit = 20;
constexpr unsigned long long kMaxMemlimit = 1ULL << 30;   // 1 GiB

bool sodiumReady() {
    static const bool ok = (sodium_init() >= 0);
    return ok;
}

// The parameters come from a file an attacker may have edited: refuse
// anything that would make the KDF allocate absurd amounts or do no work.
bool paramsInRange(const KdfParams& p) {
    return p.opslimit >= crypto_pwhash_OPSLIMIT_MIN &&
           p.opslimit <= kMaxOpslimit &&
           p.memlimit >= crypto_pwhash_MEMLIMIT_MIN &&
           p.memlimit <= kMaxMemlimit;
}

std::string toBase64(const unsigned char* data, std::size_t len) {
    std::string out(sodium_base64_ENCODED_LEN(len, sodium_base64_VARIANT_ORIGINAL), '\0');
    sodium_bin2base64(out.data(), out.size(), data, len,
                      sodium_base64_VARIANT_ORIGINAL);
    out.resize(std::strlen(out.c_str()));
    return out;
}

bool fromBase64(const std::string& in, std::vector<unsigned char>& out) {
    out.assign(in.size(), 0);
    std::size_t bin_len = 0;
    if (sodium_base642bin(out.data(), out.size(), in.c_str(), in.size(),
                          nullptr, &bin_len, nullptr,
                          sodium_base64_VARIANT_ORIGINAL) != 0)
        return false;
    out.resize(bin_len);
    return true;
}

// Derives the 32-byte key-encryption key from the passphrase.
bool deriveKek(std::string_view passphrase, const unsigned char* salt,
               const KdfParams& p, unsigned char* kek) {
    return crypto_pwhash(kek, crypto_secretbox_KEYBYTES,
                         passphrase.data(), passphrase.size(), salt,
                         p.opslimit, static_cast<std::size_t>(p.memlimit),
                         crypto_pwhash_ALG_ARGON2ID13) == 0;
}

}  // namespace

KdfParams KdfParams::moderate() {
    return {crypto_pwhash_OPSLIMIT_MODERATE, crypto_pwhash_MEMLIMIT_MODERATE};
}

KdfParams KdfParams::minimal() {
    return {crypto_pwhash_OPSLIMIT_MIN, crypto_pwhash_MEMLIMIT_MIN};
}

bool isWrapped(std::string_view file_bytes) {
    return file_bytes.size() >= kMagic.size() &&
           file_bytes.compare(0, kMagic.size(), kMagic) == 0;
}

std::string fingerprintOf(const Key& key) {
    if (!sodiumReady()) return {};
    unsigned char digest[crypto_hash_sha256_BYTES];
    crypto_hash_sha256(digest, key.data(), key.size());
    char hex[17];
    sodium_bin2hex(hex, sizeof hex, digest, 8);
    return hex;
}

std::string wrap(const Key& key, std::string_view passphrase,
                 const KdfParams& params) {
    if (!sodiumReady() || passphrase.empty() || !paramsInRange(params)) return {};

    unsigned char salt[crypto_pwhash_SALTBYTES];
    randombytes_buf(salt, sizeof salt);
    unsigned char kek[crypto_secretbox_KEYBYTES];
    if (!deriveKek(passphrase, salt, params, kek)) return {};

    std::vector<unsigned char> blob(crypto_secretbox_NONCEBYTES +
                                    crypto_secretbox_MACBYTES + key.size());
    randombytes_buf(blob.data(), crypto_secretbox_NONCEBYTES);
    crypto_secretbox_easy(blob.data() + crypto_secretbox_NONCEBYTES,
                          key.data(), key.size(), blob.data(), kek);
    sodium_memzero(kek, sizeof kek);

    json j = {
        {"version", 1},
        {"kdf", {{"alg", "argon2id13"},
                 {"salt", toBase64(salt, sizeof salt)},
                 {"opslimit", params.opslimit},
                 {"memlimit", params.memlimit}}},
        {"wrapped", toBase64(blob.data(), blob.size())},
        {"fingerprint", fingerprintOf(key)},
    };
    return std::string(kMagic) + j.dump(2) + "\n";
}

UnwrapStatus unwrap(std::string_view file_bytes, std::string_view passphrase,
                    Key& out) {
    if (!isWrapped(file_bytes)) return UnwrapStatus::NotWrapped;
    if (!sodiumReady()) return UnwrapStatus::Malformed;

    const json j = json::parse(file_bytes.substr(kMagic.size()), nullptr, false);
    if (j.is_discarded() || !j.is_object()) return UnwrapStatus::Malformed;

    if (!j.contains("version") || !j["version"].is_number_integer() ||
        j["version"].get<int>() != 1)
        return UnwrapStatus::Malformed;
    if (!j.contains("kdf") || !j["kdf"].is_object() ||
        !j.contains("wrapped") || !j["wrapped"].is_string() ||
        !j.contains("fingerprint") || !j["fingerprint"].is_string())
        return UnwrapStatus::Malformed;

    const json& kdf = j["kdf"];
    if (!kdf.contains("alg") || !kdf["alg"].is_string() ||
        kdf["alg"].get<std::string>() != "argon2id13" ||
        !kdf.contains("salt") || !kdf["salt"].is_string() ||
        !kdf.contains("opslimit") || !kdf["opslimit"].is_number_unsigned() ||
        !kdf.contains("memlimit") || !kdf["memlimit"].is_number_unsigned())
        return UnwrapStatus::Malformed;

    std::vector<unsigned char> salt, blob;
    if (!fromBase64(kdf["salt"].get<std::string>(), salt) ||
        salt.size() != crypto_pwhash_SALTBYTES ||
        !fromBase64(j["wrapped"].get<std::string>(), blob) ||
        blob.size() != crypto_secretbox_NONCEBYTES + crypto_secretbox_MACBYTES + kKeyBytes)
        return UnwrapStatus::Malformed;

    const KdfParams params{kdf["opslimit"].get<unsigned long long>(),
                           kdf["memlimit"].get<unsigned long long>()};
    if (!paramsInRange(params)) return UnwrapStatus::BadParams;

    unsigned char kek[crypto_secretbox_KEYBYTES];
    if (!deriveKek(passphrase, salt.data(), params, kek))
        return UnwrapStatus::Malformed;   // allocation failure inside the KDF

    Key plain{};
    const int rc = crypto_secretbox_open_easy(
        plain.data(), blob.data() + crypto_secretbox_NONCEBYTES,
        blob.size() - crypto_secretbox_NONCEBYTES, blob.data(), kek);
    sodium_memzero(kek, sizeof kek);
    if (rc != 0) return UnwrapStatus::WrongPassphrase;

    if (fingerprintOf(plain) != j["fingerprint"].get<std::string>()) {
        sodium_memzero(plain.data(), plain.size());
        return UnwrapStatus::WrongPassphrase;
    }
    out = plain;
    sodium_memzero(plain.data(), plain.size());
    return UnwrapStatus::Ok;
}

}  // namespace liveqx::auth::keywrap
