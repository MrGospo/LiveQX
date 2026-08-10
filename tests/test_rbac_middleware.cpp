// fix22 commit 6/24 — RbacMiddleware unit-тесты.
//
// Покрытие:
//   - registerEndpoint + ruleCount + clearRules
//   - findRule literal + pattern + method-mismatch + segment-count-mismatch
//   - authorize: NotConfigured, open, Unauthorized (no/bad/revoked),
//                Forbidden (role/ACL), PasswordChangeRequired,
//                Admin bypass per-channel ACL,
//                channel_id<0 пропускает ACL,
//                out_ctx populated correctly.

#include <gtest/gtest.h>

#include <filesystem>

#include "auth/AuthDb.h"
#include "auth/AuthService.h"
#include "auth/JwtIssuer.h"
#include "auth/PasswordHasher.h"
#include "auth/RbacMiddleware.h"

namespace sa = liveqx::auth;

namespace {

constexpr const char* kSecret = "0123456789abcdef0123456789abcdef";

class RbacTest : public ::testing::Test {
protected:
    std::filesystem::path tmp_;
    std::unique_ptr<sa::AuthDb>          db_;
    std::unique_ptr<sa::JwtIssuer>       jwt_;
    std::unique_ptr<sa::AuthService>     svc_;
    std::unique_ptr<sa::RbacMiddleware>  mw_;
    std::vector<sa::ChannelGrant>        next_grants_;

    void SetUp() override {
        tmp_ = std::filesystem::temp_directory_path() /
            ("rbac_test_" + std::string(::testing::UnitTest::GetInstance()
                ->current_test_info()->name()) + ".db");
        std::filesystem::remove(tmp_);

        db_  = std::make_unique<sa::AuthDb>(tmp_);
        ASSERT_TRUE(db_->open());
        jwt_ = std::make_unique<sa::JwtIssuer>(std::string(kSecret));
        svc_ = std::make_unique<sa::AuthService>(*db_, *jwt_,
            [this](std::int64_t) { return next_grants_; });
        mw_  = std::make_unique<sa::RbacMiddleware>(*db_, *jwt_);
    }

    void TearDown() override {
        mw_.reset(); svc_.reset(); jwt_.reset(); db_.reset();
        std::error_code ec;
        std::filesystem::remove(tmp_, ec);
        std::filesystem::remove(tmp_.string() + "-wal", ec);
        std::filesystem::remove(tmp_.string() + "-shm", ec);
    }

    std::int64_t makeUser(const std::string& name, sa::Role role,
                          bool mcp = false) {
        auto h = sa::PasswordHasher::hash("p");
        sa::User u;
        u.username             = name;
        u.password_hash        = *h;
        u.role                 = role;
        u.source               = sa::Source::Local;
        u.must_change_password = mcp;
        return *db_->insertUser(u);
    }

    // Login и вытащить access_token
    std::string loginToken(const std::string& name) {
        auto lr = svc_->login(name, "p", "ip", "ua");
        auto* pair = std::get_if<sa::JwtIssuer::TokenPair>(&lr.outcome);
        EXPECT_NE(pair, nullptr);
        return pair->access_token;
    }
};

}  // namespace

TEST_F(RbacTest, RegisterAndCountAndClear) {
    EXPECT_EQ(mw_->ruleCount(), 0u);
    mw_->registerEndpoint("GET /healthz", {sa::Role::Viewer, false, sa::ChannelPermission::View, true});
    mw_->registerEndpoint("GET /api/channels", {sa::Role::Viewer, false, sa::ChannelPermission::View, false});
    mw_->registerEndpoint("POST /api/channels/{id}/play",
        {sa::Role::Operator, true, sa::ChannelPermission::Operate, false});
    EXPECT_EQ(mw_->ruleCount(), 3u);
    mw_->clearRules();
    EXPECT_EQ(mw_->ruleCount(), 0u);
}

TEST_F(RbacTest, NotConfiguredWhenNoRule) {
    EXPECT_EQ(mw_->authorize("GET", "/nope", "", -1, nullptr),
              sa::RbacMiddleware::Decision::NotConfigured);
}

TEST_F(RbacTest, OpenEndpointAllowsWithoutToken) {
    mw_->registerEndpoint("GET /healthz",
        {sa::Role::Viewer, false, sa::ChannelPermission::View, true});
    EXPECT_EQ(mw_->authorize("GET", "/healthz", "", -1, nullptr),
              sa::RbacMiddleware::Decision::Allow);
    // Plus с garbage-Bearer тоже Allow.
    EXPECT_EQ(mw_->authorize("GET", "/healthz", "garbage", -1, nullptr),
              sa::RbacMiddleware::Decision::Allow);
}

TEST_F(RbacTest, ClosedEndpointWithoutTokenUnauthorized) {
    mw_->registerEndpoint("GET /api/channels",
        {sa::Role::Viewer, false, sa::ChannelPermission::View, false});
    EXPECT_EQ(mw_->authorize("GET", "/api/channels", "", -1, nullptr),
              sa::RbacMiddleware::Decision::Unauthorized);
}

TEST_F(RbacTest, BadTokenUnauthorized) {
    mw_->registerEndpoint("GET /api/channels",
        {sa::Role::Viewer, false, sa::ChannelPermission::View, false});
    EXPECT_EQ(mw_->authorize("GET", "/api/channels", "aaa.bbb.ccc", -1, nullptr),
              sa::RbacMiddleware::Decision::Unauthorized);
}

TEST_F(RbacTest, ValidTokenAllow) {
    makeUser("alice", sa::Role::Operator);
    auto tok = loginToken("alice");

    mw_->registerEndpoint("GET /api/channels",
        {sa::Role::Operator, false, sa::ChannelPermission::View, false});

    sa::RequestContext ctx;
    EXPECT_EQ(mw_->authorize("GET", "/api/channels", tok, -1, &ctx),
              sa::RbacMiddleware::Decision::Allow);
    EXPECT_EQ(ctx.username, "alice");
    EXPECT_EQ(ctx.role,     sa::Role::Operator);
}

TEST_F(RbacTest, RevokedSessionUnauthorized) {
    makeUser("alice", sa::Role::Operator);
    auto tok = loginToken("alice");

    // Найти jti и ревокнуть сессию.
    auto claims = jwt_->verify(tok);
    ASSERT_TRUE(claims.has_value());
    db_->revokeSessionByJwtId(claims->jti, 12345);

    mw_->registerEndpoint("GET /api/channels",
        {sa::Role::Operator, false, sa::ChannelPermission::View, false});

    EXPECT_EQ(mw_->authorize("GET", "/api/channels", tok, -1, nullptr),
              sa::RbacMiddleware::Decision::Unauthorized);
}

TEST_F(RbacTest, InsufficientRoleForbidden) {
    makeUser("viewer_bob", sa::Role::Viewer);
    auto tok = loginToken("viewer_bob");

    mw_->registerEndpoint("DELETE /api/channels/{id}",
        {sa::Role::Admin, false, sa::ChannelPermission::View, false});

    EXPECT_EQ(mw_->authorize("DELETE", "/api/channels/1", tok, 1, nullptr),
              sa::RbacMiddleware::Decision::Forbidden);
}

TEST_F(RbacTest, OperatorPassesViewerRule) {
    makeUser("opr", sa::Role::Operator);
    auto tok = loginToken("opr");

    mw_->registerEndpoint("GET /api/channels",
        {sa::Role::Viewer, false, sa::ChannelPermission::View, false});

    EXPECT_EQ(mw_->authorize("GET", "/api/channels", tok, -1, nullptr),
              sa::RbacMiddleware::Decision::Allow);
}

TEST_F(RbacTest, ChannelAclMissingForbidden) {
    makeUser("opr", sa::Role::Operator);
    auto tok = loginToken("opr");

    mw_->registerEndpoint("POST /api/channels/{id}/play",
        {sa::Role::Operator, true, sa::ChannelPermission::Operate, false});

    EXPECT_EQ(mw_->authorize("POST", "/api/channels/7/play", tok, 7, nullptr),
              sa::RbacMiddleware::Decision::Forbidden);
}

TEST_F(RbacTest, ChannelAclViewSatisfiedByAnyGrant) {
    next_grants_ = {{7, sa::ChannelPermission::View}};
    makeUser("opr", sa::Role::Operator);
    auto tok = loginToken("opr");

    mw_->registerEndpoint("GET /api/channels/{id}",
        {sa::Role::Viewer, true, sa::ChannelPermission::View, false});

    EXPECT_EQ(mw_->authorize("GET", "/api/channels/7", tok, 7, nullptr),
              sa::RbacMiddleware::Decision::Allow);
}

TEST_F(RbacTest, ChannelAclOperateRequiresOperateGrant) {
    next_grants_ = {{7, sa::ChannelPermission::View}};  // только View
    makeUser("opr", sa::Role::Operator);
    auto tok = loginToken("opr");

    mw_->registerEndpoint("POST /api/channels/{id}/play",
        {sa::Role::Operator, true, sa::ChannelPermission::Operate, false});

    EXPECT_EQ(mw_->authorize("POST", "/api/channels/7/play", tok, 7, nullptr),
              sa::RbacMiddleware::Decision::Forbidden);
}

TEST_F(RbacTest, ChannelAclOperateAllowedWithOperateGrant) {
    next_grants_ = {{7, sa::ChannelPermission::Operate}};
    makeUser("opr", sa::Role::Operator);
    auto tok = loginToken("opr");

    mw_->registerEndpoint("POST /api/channels/{id}/play",
        {sa::Role::Operator, true, sa::ChannelPermission::Operate, false});

    EXPECT_EQ(mw_->authorize("POST", "/api/channels/7/play", tok, 7, nullptr),
              sa::RbacMiddleware::Decision::Allow);
}

TEST_F(RbacTest, AdminBypassesChannelAcl) {
    // admin без явных grants всё равно проходит.
    next_grants_ = {};
    makeUser("root", sa::Role::Admin);
    auto tok = loginToken("root");

    mw_->registerEndpoint("POST /api/channels/{id}/play",
        {sa::Role::Operator, true, sa::ChannelPermission::Operate, false});

    EXPECT_EQ(mw_->authorize("POST", "/api/channels/42/play", tok, 42, nullptr),
              sa::RbacMiddleware::Decision::Allow);
}

TEST_F(RbacTest, ChannelIdNegativeSkipsAclCheck) {
    // Endpoint у которого URL не содержит channel_id (например, общий
    // GET /api/channels). ACL чек не должен резать.
    makeUser("opr", sa::Role::Operator);
    auto tok = loginToken("opr");

    mw_->registerEndpoint("GET /api/channels",
        {sa::Role::Operator, true, sa::ChannelPermission::View, false});

    EXPECT_EQ(mw_->authorize("GET", "/api/channels", tok, -1, nullptr),
              sa::RbacMiddleware::Decision::Allow);
}

TEST_F(RbacTest, MustChangePasswordBlocksOtherEndpoints) {
    makeUser("alice", sa::Role::Operator, /*mcp=*/true);
    auto tok = loginToken("alice");

    mw_->registerEndpoint("GET /api/channels",
        {sa::Role::Operator, false, sa::ChannelPermission::View, false});

    EXPECT_EQ(mw_->authorize("GET", "/api/channels", tok, -1, nullptr),
              sa::RbacMiddleware::Decision::PasswordChangeRequired);
}

TEST_F(RbacTest, MustChangePasswordAllowsSelfPasswordEndpoint) {
    makeUser("alice", sa::Role::Operator, /*mcp=*/true);
    auto tok = loginToken("alice");

    mw_->registerEndpoint("POST /api/auth/me/password",
        {sa::Role::Viewer, false, sa::ChannelPermission::View, false});

    EXPECT_EQ(mw_->authorize("POST", "/api/auth/me/password", tok, -1, nullptr),
              sa::RbacMiddleware::Decision::Allow);
}

TEST_F(RbacTest, PatternMatchingPicksRightRule) {
    makeUser("opr", sa::Role::Operator);
    auto tok = loginToken("opr");

    mw_->registerEndpoint("POST /api/channels/{id}/play",
        {sa::Role::Operator, false, sa::ChannelPermission::View, false});

    // Любой числовой id матчит.
    EXPECT_EQ(mw_->authorize("POST", "/api/channels/12345/play", tok, 12345, nullptr),
              sa::RbacMiddleware::Decision::Allow);
    // Слэш внутри сегмента — не матчит.
    EXPECT_EQ(mw_->authorize("POST", "/api/channels/12345/play/extra", tok, -1, nullptr),
              sa::RbacMiddleware::Decision::NotConfigured);
}

TEST_F(RbacTest, MethodMismatchTreatedAsUnregistered) {
    mw_->registerEndpoint("POST /api/channels",
        {sa::Role::Admin, false, sa::ChannelPermission::View, false});
    EXPECT_EQ(mw_->authorize("GET", "/api/channels", "", -1, nullptr),
              sa::RbacMiddleware::Decision::NotConfigured);
}

TEST_F(RbacTest, LiteralRulePreferredOverPattern) {
    // Literal "POST /api/auth/me/password" — open для любых;
    // pattern "POST /api/auth/{id}/password" — admin-only.
    // Запрос на конкретный literal должен матчить literal-правило.
    mw_->registerEndpoint("POST /api/auth/me/password",
        {sa::Role::Viewer, false, sa::ChannelPermission::View, true});
    mw_->registerEndpoint("POST /api/auth/{id}/password",
        {sa::Role::Admin, false, sa::ChannelPermission::View, false});

    EXPECT_EQ(mw_->authorize("POST", "/api/auth/me/password", "", -1, nullptr),
              sa::RbacMiddleware::Decision::Allow);  // literal hit, open=true
}

TEST_F(RbacTest, OutCtxNotPopulatedOnDenial) {
    makeUser("viewer_bob", sa::Role::Viewer);
    auto tok = loginToken("viewer_bob");

    mw_->registerEndpoint("DELETE /api/channels/{id}",
        {sa::Role::Admin, false, sa::ChannelPermission::View, false});

    sa::RequestContext ctx;
    ctx.username = "untouched";
    EXPECT_EQ(mw_->authorize("DELETE", "/api/channels/1", tok, 1, &ctx),
              sa::RbacMiddleware::Decision::Forbidden);
    // На отказе ctx не трогаем — caller не должен на него полагаться.
    EXPECT_EQ(ctx.username, "untouched");
}
