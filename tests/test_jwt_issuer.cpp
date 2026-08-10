// fix22 commit 4/24 — JWT issuer/verifier тесты.

#include <gtest/gtest.h>

#include <set>
#include <stdexcept>
#include <string>
#include <thread>

#include "auth/JwtIssuer.h"

namespace sa = liveqx::auth;

namespace {

sa::User makeUser() {
    sa::User u;
    u.id                   = 42;
    u.username             = "alice";
    u.email                = "alice@example.com";
    u.role                 = sa::Role::Operator;
    u.must_change_password = true;
    return u;
}

std::vector<sa::ChannelGrant> makeGrants() {
    return {
        {7,  sa::ChannelPermission::View},
        {99, sa::ChannelPermission::Operate},
    };
}

// 32+ ASCII chars — нижняя граница HS256.
constexpr const char* kSecret = "0123456789abcdef0123456789abcdef";

}  // namespace

TEST(JwtIssuer, ConstructorRejectsShortSecret) {
    EXPECT_THROW(sa::JwtIssuer{std::string("short")}, std::invalid_argument);
    EXPECT_THROW(sa::JwtIssuer{std::string(31, 'x')}, std::invalid_argument);
    EXPECT_NO_THROW(sa::JwtIssuer{std::string(32, 'x')});
}

TEST(JwtIssuer, IssueAndVerifyRoundTrip) {
    sa::JwtIssuer issuer(kSecret);
    auto user = makeUser();
    auto grants = makeGrants();

    auto pair = issuer.issue(user, grants);
    EXPECT_FALSE(pair.access_token.empty());
    EXPECT_FALSE(pair.refresh_token.empty());
    EXPECT_FALSE(pair.jwt_id.empty());
    EXPECT_GT(pair.access_expires_at, 0);
    EXPECT_GT(pair.refresh_expires_at, pair.access_expires_at);

    auto claims = issuer.verify(pair.access_token);
    ASSERT_TRUE(claims.has_value());
    EXPECT_EQ(claims->user_id,             user.id);
    EXPECT_EQ(claims->username,            user.username);
    EXPECT_EQ(claims->role,                user.role);
    EXPECT_EQ(claims->must_change_password, user.must_change_password);
    EXPECT_EQ(claims->jti,                 pair.jwt_id);
    EXPECT_EQ(claims->exp,                 pair.access_expires_at);

    ASSERT_EQ(claims->channel_grants.size(), grants.size());
    EXPECT_EQ(claims->channel_grants[0].channel_id, 7);
    EXPECT_EQ(claims->channel_grants[0].permission, sa::ChannelPermission::View);
    EXPECT_EQ(claims->channel_grants[1].channel_id, 99);
    EXPECT_EQ(claims->channel_grants[1].permission, sa::ChannelPermission::Operate);
}

TEST(JwtIssuer, VerifyRejectsWrongSecret) {
    sa::JwtIssuer issuer_a(kSecret);
    sa::JwtIssuer issuer_b(std::string(32, 'Z'));

    auto pair = issuer_a.issue(makeUser(), {});
    EXPECT_FALSE(issuer_b.verify(pair.access_token).has_value());
    // Своим секретом — норм.
    EXPECT_TRUE(issuer_a.verify(pair.access_token).has_value());
}

TEST(JwtIssuer, VerifyRejectsTamperedSignature) {
    sa::JwtIssuer issuer(kSecret);
    auto pair = issuer.issue(makeUser(), {});

    auto tampered = pair.access_token;
    // Меняем последний байт подписи.
    tampered.back() = (tampered.back() == 'A' ? 'B' : 'A');
    EXPECT_FALSE(issuer.verify(tampered).has_value());
}

TEST(JwtIssuer, VerifyRejectsTamperedPayload) {
    sa::JwtIssuer issuer(kSecret);
    auto pair = issuer.issue(makeUser(), {});

    // JWT = header.payload.signature; ломаем серединный сегмент.
    auto first_dot = pair.access_token.find('.');
    ASSERT_NE(first_dot, std::string::npos);
    auto tampered = pair.access_token;
    tampered[first_dot + 1] = (tampered[first_dot + 1] == 'A' ? 'B' : 'A');
    EXPECT_FALSE(issuer.verify(tampered).has_value());
}

TEST(JwtIssuer, VerifyRejectsMalformedToken) {
    sa::JwtIssuer issuer(kSecret);
    EXPECT_FALSE(issuer.verify("").has_value());
    EXPECT_FALSE(issuer.verify("not-a-jwt").has_value());
    EXPECT_FALSE(issuer.verify("aaa.bbb.ccc").has_value());
}

TEST(JwtIssuer, VerifyRejectsRefreshToken) {
    // Refresh — opaque random base64, не JWT. jwt::decode на нём бросит.
    sa::JwtIssuer issuer(kSecret);
    auto pair = issuer.issue(makeUser(), {});
    EXPECT_FALSE(issuer.verify(pair.refresh_token).has_value());
}

TEST(JwtIssuer, JtiUniqueAcrossIssues) {
    sa::JwtIssuer issuer(kSecret);
    auto user = makeUser();

    std::set<std::string> jtis;
    for (int i = 0; i < 100; ++i) {
        auto pair = issuer.issue(user, {});
        EXPECT_TRUE(jtis.insert(pair.jwt_id).second);
        EXPECT_TRUE(jtis.insert(pair.refresh_token).second);
    }
}

TEST(JwtIssuer, AccessAndRefreshTokensDiffer) {
    sa::JwtIssuer issuer(kSecret);
    auto pair = issuer.issue(makeUser(), {});
    EXPECT_NE(pair.access_token, pair.refresh_token);
    EXPECT_NE(pair.jwt_id,       pair.refresh_token);
}

TEST(JwtIssuer, MustChangePasswordSurvivesRoundTripWhenFalse) {
    sa::JwtIssuer issuer(kSecret);
    auto user = makeUser();
    user.must_change_password = false;

    auto pair = issuer.issue(user, {});
    auto claims = issuer.verify(pair.access_token);
    ASSERT_TRUE(claims.has_value());
    EXPECT_FALSE(claims->must_change_password);
}

TEST(JwtIssuer, EmptyGrantsRoundTrip) {
    sa::JwtIssuer issuer(kSecret);
    auto pair = issuer.issue(makeUser(), {});
    auto claims = issuer.verify(pair.access_token);
    ASSERT_TRUE(claims.has_value());
    EXPECT_TRUE(claims->channel_grants.empty());
}

TEST(JwtIssuer, AllRolesRoundTrip) {
    sa::JwtIssuer issuer(kSecret);
    for (auto r : {sa::Role::Viewer, sa::Role::Operator, sa::Role::Admin}) {
        auto u = makeUser();
        u.role = r;
        auto pair = issuer.issue(u, {});
        auto claims = issuer.verify(pair.access_token);
        ASSERT_TRUE(claims.has_value());
        EXPECT_EQ(claims->role, r);
    }
}

TEST(JwtIssuer, ExpiryIsAccessTtlFromNow) {
    sa::JwtIssuer issuer(kSecret);
    const auto t0 = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    auto pair = issuer.issue(makeUser(), {});

    const auto access_secs = std::chrono::duration_cast<std::chrono::seconds>(
        sa::JwtIssuer::kAccessTtl).count();
    // 5 sec slack на разницу между двумя now() выше и внутри issue().
    EXPECT_NEAR(pair.access_expires_at - t0, access_secs, 5);

    const auto refresh_secs = std::chrono::duration_cast<std::chrono::seconds>(
        sa::JwtIssuer::kRefreshTtl).count();
    EXPECT_NEAR(pair.refresh_expires_at - t0, refresh_secs, 5);
}
