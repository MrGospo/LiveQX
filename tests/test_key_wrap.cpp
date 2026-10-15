// Unit tests for auth/KeyWrap — passphrase wrapping of the master key.
//
// Coverage: round trip, wrong passphrase, every kind of damage to the file,
// KDF parameter bounds (a hostile file must not make us allocate gigabytes),
// randomness of salt/nonce, passphrase change leaves K untouched.

#include "auth/KeyWrap.h"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <sodium.h>

namespace kw = liveqx::auth::keywrap;
using nlohmann::json;

namespace {

kw::Key sampleKey(std::uint8_t fill = 0x5A) {
    kw::Key k{};
    k.fill(fill);
    return k;
}

std::string wrapFast(const kw::Key& k, std::string_view pass) {
    return kw::wrap(k, pass, kw::KdfParams::minimal());
}

json bodyOf(const std::string& file) {
    return json::parse(file.substr(kw::kMagic.size()));
}

std::string withBody(const json& j) {
    return std::string(kw::kMagic) + j.dump();
}

}  // namespace

TEST(KeyWrap, RoundTripReturnsTheSameKey) {
    const auto key = sampleKey();
    const auto file = wrapFast(key, "correct horse battery staple");
    ASSERT_FALSE(file.empty());
    EXPECT_TRUE(kw::isWrapped(file));

    kw::Key out{};
    EXPECT_EQ(kw::unwrap(file, "correct horse battery staple", out), kw::UnwrapStatus::Ok);
    EXPECT_EQ(out, key);
}

TEST(KeyWrap, FileStartsWithMagicAndContainsNoKeyMaterial) {
    const auto key = sampleKey(0x41);
    const auto file = wrapFast(key, "passphrase-123");
    ASSERT_GT(file.size(), kw::kMagic.size());
    EXPECT_EQ(file.compare(0, kw::kMagic.size(), kw::kMagic), 0);
    EXPECT_EQ(file.find(std::string(kw::kKeyBytes, 'A')), std::string::npos);
    const auto j = bodyOf(file);
    EXPECT_EQ(j.at("version"), 1);
    EXPECT_EQ(j.at("kdf").at("alg"), "argon2id13");
    EXPECT_EQ(j.at("fingerprint"), kw::fingerprintOf(key));
}

TEST(KeyWrap, WrongPassphraseIsRejected) {
    const auto file = wrapFast(sampleKey(), "right-passphrase");
    kw::Key out{};
    EXPECT_EQ(kw::unwrap(file, "wrong-passphrase", out), kw::UnwrapStatus::WrongPassphrase);
    EXPECT_EQ(kw::unwrap(file, "", out), kw::UnwrapStatus::WrongPassphrase);
}

TEST(KeyWrap, EmptyPassphraseCannotWrap) {
    EXPECT_TRUE(kw::wrap(sampleKey(), "", kw::KdfParams::minimal()).empty());
}

TEST(KeyWrap, TwoWrapsOfTheSameKeyDiffer) {
    const auto a = wrapFast(sampleKey(), "same-passphrase");
    const auto b = wrapFast(sampleKey(), "same-passphrase");
    EXPECT_NE(a, b) << "salt and nonce must be random";
}

TEST(KeyWrap, BareKeyIsNotWrapped) {
    const std::string bare(kw::kKeyBytes, 'x');
    EXPECT_FALSE(kw::isWrapped(bare));
    kw::Key out{};
    EXPECT_EQ(kw::unwrap(bare, "whatever", out), kw::UnwrapStatus::NotWrapped);
}

TEST(KeyWrap, GarbageAfterMagicIsMalformed) {
    kw::Key out{};
    EXPECT_EQ(kw::unwrap(std::string(kw::kMagic) + "not json", "p", out),
              kw::UnwrapStatus::Malformed);
    EXPECT_EQ(kw::unwrap(std::string(kw::kMagic), "p", out), kw::UnwrapStatus::Malformed);
    EXPECT_EQ(kw::unwrap(std::string(kw::kMagic) + "[1,2,3]", "p", out),
              kw::UnwrapStatus::Malformed);
}

TEST(KeyWrap, MissingOrWrongTypedFieldsAreMalformed) {
    const auto file = wrapFast(sampleKey(), "pass-phrase-1");
    kw::Key out{};
    for (const char* field : {"version", "kdf", "wrapped", "fingerprint"}) {
        auto j = bodyOf(file);
        j.erase(field);
        EXPECT_EQ(kw::unwrap(withBody(j), "pass-phrase-1", out),
                  kw::UnwrapStatus::Malformed) << field;
    }
    auto j = bodyOf(file);
    j["wrapped"] = 42;
    EXPECT_EQ(kw::unwrap(withBody(j), "pass-phrase-1", out), kw::UnwrapStatus::Malformed);
    j = bodyOf(file);
    j["wrapped"] = "!!!not base64!!!";
    EXPECT_EQ(kw::unwrap(withBody(j), "pass-phrase-1", out), kw::UnwrapStatus::Malformed);
}

TEST(KeyWrap, UnknownVersionOrKdfIsMalformed) {
    const auto file = wrapFast(sampleKey(), "pass-phrase-1");
    kw::Key out{};
    auto j = bodyOf(file);
    j["version"] = 2;
    EXPECT_EQ(kw::unwrap(withBody(j), "pass-phrase-1", out), kw::UnwrapStatus::Malformed);
    j = bodyOf(file);
    j["kdf"]["alg"] = "scrypt";
    EXPECT_EQ(kw::unwrap(withBody(j), "pass-phrase-1", out), kw::UnwrapStatus::Malformed);
}

TEST(KeyWrap, TamperedBlobIsRejectedAsWrongPassphrase) {
    const auto file = wrapFast(sampleKey(), "pass-phrase-1");
    auto j = bodyOf(file);
    std::string b64 = j["wrapped"];
    b64[b64.size() / 2] = (b64[b64.size() / 2] == 'A') ? 'B' : 'A';
    j["wrapped"] = b64;
    kw::Key out{};
    EXPECT_EQ(kw::unwrap(withBody(j), "pass-phrase-1", out),
              kw::UnwrapStatus::WrongPassphrase);
}

TEST(KeyWrap, TamperedKdfParametersMakeTheFileUnreadable) {
    const auto file = wrapFast(sampleKey(), "pass-phrase-1");
    auto j = bodyOf(file);
    j["kdf"]["opslimit"] = j["kdf"]["opslimit"].get<unsigned long long>() + 1;
    kw::Key out{};
    EXPECT_EQ(kw::unwrap(withBody(j), "pass-phrase-1", out),
              kw::UnwrapStatus::WrongPassphrase);
}

TEST(KeyWrap, FingerprintOfAnotherKeyIsRejected) {
    const auto file = wrapFast(sampleKey(0x01), "pass-phrase-1");
    auto j = bodyOf(file);
    j["fingerprint"] = kw::fingerprintOf(sampleKey(0x02));
    kw::Key out{};
    EXPECT_EQ(kw::unwrap(withBody(j), "pass-phrase-1", out),
              kw::UnwrapStatus::WrongPassphrase);
}

TEST(KeyWrap, HostileKdfParametersAreRefusedBeforeAnyWork) {
    const auto file = wrapFast(sampleKey(), "pass-phrase-1");
    kw::Key out{};
    struct Case { unsigned long long ops, mem; };
    // 64 GiB of memory, zero iterations, absurd iteration count.
    for (Case c : {Case{3, 64ULL << 30}, Case{0, 8192}, Case{1ULL << 40, 8192}}) {
        auto j = bodyOf(file);
        j["kdf"]["opslimit"] = c.ops;
        j["kdf"]["memlimit"] = c.mem;
        EXPECT_EQ(kw::unwrap(withBody(j), "pass-phrase-1", out),
                  kw::UnwrapStatus::BadParams) << c.ops << "/" << c.mem;
    }
}

TEST(KeyWrap, WrapRefusesOutOfRangeParameters) {
    EXPECT_TRUE(kw::wrap(sampleKey(), "pass-phrase-1", {0, 8192}).empty());
    EXPECT_TRUE(kw::wrap(sampleKey(), "pass-phrase-1", {3, 64ULL << 30}).empty());
}

TEST(KeyWrap, ChangingThePassphraseKeepsTheKey) {
    const auto key = sampleKey(0x77);
    const auto first = wrapFast(key, "first-passphrase");
    kw::Key opened{};
    ASSERT_EQ(kw::unwrap(first, "first-passphrase", opened), kw::UnwrapStatus::Ok);
    const auto second = wrapFast(opened, "second-passphrase");

    kw::Key again{};
    EXPECT_EQ(kw::unwrap(second, "second-passphrase", again), kw::UnwrapStatus::Ok);
    EXPECT_EQ(again, key) << "K must survive a passphrase change";
    EXPECT_EQ(kw::unwrap(second, "first-passphrase", again),
              kw::UnwrapStatus::WrongPassphrase);
}

TEST(KeyWrap, FingerprintMatchesTheMasterKeyFormat) {
    const auto fp = kw::fingerprintOf(sampleKey());
    ASSERT_EQ(fp.size(), 16u);
    for (char c : fp) EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
}

TEST(KeyWrap, ModerateParametersAreWithinTheAcceptedRange) {
    // The default used for real files must be accepted by unwrap().
    const auto p = kw::KdfParams::moderate();
    EXPECT_GE(p.opslimit, 2u);
    EXPECT_GE(p.memlimit, 64ULL << 20);
    EXPECT_LE(p.memlimit, 1ULL << 30);
}
