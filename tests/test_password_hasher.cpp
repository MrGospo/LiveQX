// fix22 commit 3/24 — argon2id wrapper тесты.

#include <gtest/gtest.h>

#include <set>
#include <string>

#include "auth/PasswordHasher.h"

namespace sa = liveqx::auth;

TEST(PasswordHasher, HashRoundTrip) {
    auto h = sa::PasswordHasher::hash("correct horse battery staple");
    ASSERT_TRUE(h.has_value());
    EXPECT_FALSE(h->empty());

    EXPECT_TRUE(sa::PasswordHasher::verify("correct horse battery staple", *h));
    EXPECT_FALSE(sa::PasswordHasher::verify("incorrect horse",            *h));
}

TEST(PasswordHasher, HashIsArgon2idEncoded) {
    auto h = sa::PasswordHasher::hash("hunter2");
    ASSERT_TRUE(h.has_value());

    // libsodium всегда выдаёт строку, начинающуюся с $argon2id$ для
    // crypto_pwhash_ALG_ARGON2ID13.
    EXPECT_EQ(h->rfind("$argon2id$", 0), 0u);
}

TEST(PasswordHasher, EachHashUsesFreshSalt) {
    // Один и тот же plaintext → разные encoded-строки (разный salt).
    auto h1 = sa::PasswordHasher::hash("same-password");
    auto h2 = sa::PasswordHasher::hash("same-password");
    ASSERT_TRUE(h1.has_value());
    ASSERT_TRUE(h2.has_value());
    EXPECT_NE(*h1, *h2);

    // Но обе verify'нутся.
    EXPECT_TRUE(sa::PasswordHasher::verify("same-password", *h1));
    EXPECT_TRUE(sa::PasswordHasher::verify("same-password", *h2));
}

TEST(PasswordHasher, VerifyRejectsTamperedHash) {
    auto h = sa::PasswordHasher::hash("real");
    ASSERT_TRUE(h.has_value());

    auto tampered = *h;
    tampered.back() = (tampered.back() == 'A' ? 'B' : 'A');
    EXPECT_FALSE(sa::PasswordHasher::verify("real", tampered));
}

TEST(PasswordHasher, VerifyRejectsEmptyInputs) {
    auto h = sa::PasswordHasher::hash("real");
    ASSERT_TRUE(h.has_value());

    EXPECT_FALSE(sa::PasswordHasher::verify("",     *h));
    EXPECT_FALSE(sa::PasswordHasher::verify("real", ""));
}

TEST(PasswordHasher, VerifyRejectsMalformedHash) {
    EXPECT_FALSE(sa::PasswordHasher::verify("real", "not-a-real-hash"));
    EXPECT_FALSE(sa::PasswordHasher::verify("real",
        std::string(200, 'x')));  // явно длиннее STRBYTES
}

TEST(PasswordHasher, HashRejectsEmptyPassword) {
    auto h = sa::PasswordHasher::hash("");
    EXPECT_FALSE(h.has_value());
}

TEST(PasswordHasher, GenerateRandomLengthIsApproxCorrect) {
    auto p = sa::PasswordHasher::generateRandom();
    // 24 байта → URL-safe base64 без padding = ceil(24*4/3) = 32 chars.
    EXPECT_EQ(p.size(), 32u);
}

TEST(PasswordHasher, GenerateRandomIsUrlSafeAlphabet) {
    auto p = sa::PasswordHasher::generateRandom();
    ASSERT_FALSE(p.empty());
    for (char c : p) {
        const bool ok = (c >= 'A' && c <= 'Z') ||
                        (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') ||
                        c == '-' || c == '_';
        EXPECT_TRUE(ok) << "unexpected char: " << static_cast<int>(c);
    }
}

TEST(PasswordHasher, GenerateRandomDoesNotRepeat) {
    // Дешёвый sanity-test: 100 generate'ов → 100 уникальных значений.
    // (Шанс коллизии ~ 100*100 / 2^192 — пренебрежимо.)
    std::set<std::string> seen;
    for (int i = 0; i < 100; ++i) {
        auto p = sa::PasswordHasher::generateRandom();
        ASSERT_FALSE(p.empty());
        EXPECT_TRUE(seen.insert(p).second);
    }
}

TEST(PasswordHasher, GeneratedRandomIsValidPasswordInput) {
    // Hash → verify полного round-trip с случайным паролем.
    auto pw = sa::PasswordHasher::generateRandom();
    auto h  = sa::PasswordHasher::hash(pw);
    ASSERT_TRUE(h.has_value());
    EXPECT_TRUE(sa::PasswordHasher::verify(pw, *h));
}

TEST(PasswordHasher, UnicodePasswordsHandled) {
    const std::string pw = "пароль-с-юникодом-✅";
    auto h = sa::PasswordHasher::hash(pw);
    ASSERT_TRUE(h.has_value());
    EXPECT_TRUE(sa::PasswordHasher::verify(pw, *h));
    EXPECT_FALSE(sa::PasswordHasher::verify("пароль-с-юникодом",  *h));
}
