// fix22 commit 5/24 — AuthService login/logout/refresh тесты.

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <variant>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include "auth/AuthDb.h"
#include "auth/AuthService.h"
#include "auth/JwtIssuer.h"
#include "auth/PasswordHasher.h"

namespace sa = liveqx::auth;

namespace {

constexpr const char* kSecret = "0123456789abcdef0123456789abcdef";

class AuthServiceTest : public ::testing::Test {
protected:
    std::filesystem::path tmp_;
    std::unique_ptr<sa::AuthDb>      db_;
    std::unique_ptr<sa::JwtIssuer>   jwt_;
    std::unique_ptr<sa::AuthService> svc_;

    void SetUp() override {
        tmp_ = std::filesystem::temp_directory_path() /
            ("auth_service_test_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
             "_" + ::testing::UnitTest::GetInstance()->current_test_info()->name() + ".db");
        std::filesystem::remove(tmp_);

        db_  = std::make_unique<sa::AuthDb>(tmp_);
        ASSERT_TRUE(db_->open());

        jwt_ = std::make_unique<sa::JwtIssuer>(std::string(kSecret));
        svc_ = std::make_unique<sa::AuthService>(*db_, *jwt_,
            [](std::int64_t) { return std::vector<sa::ChannelGrant>{}; });
    }

    void TearDown() override {
        svc_.reset();
        jwt_.reset();
        db_.reset();
        std::error_code ec;
        std::filesystem::remove(tmp_, ec);
        std::filesystem::remove(tmp_.string() + "-wal", ec);
        std::filesystem::remove(tmp_.string() + "-shm", ec);
    }

    std::int64_t insertLocalUser(const std::string& name,
                                 const std::string& password,
                                 sa::Role role = sa::Role::Operator,
                                 bool disabled = false) {
        auto h = sa::PasswordHasher::hash(password);
        EXPECT_TRUE(h.has_value());
        sa::User u;
        u.username      = name;
        u.password_hash = *h;
        u.role          = role;
        u.source        = sa::Source::Local;
        u.disabled      = disabled;
        auto id = db_->insertUser(u);
        EXPECT_TRUE(id.has_value());
        return *id;
    }
};

}  // namespace

TEST_F(AuthServiceTest, LoginSuccessReturnsTokenPairAndPersistsSession) {
    auto uid = insertLocalUser("alice", "hunter2");

    auto lr = svc_->login("alice", "hunter2", "10.0.0.1", "ua/1.0");
    auto* pair = std::get_if<sa::JwtIssuer::TokenPair>(&lr.outcome);
    ASSERT_NE(pair, nullptr);
    ASSERT_TRUE(lr.user.has_value());
    EXPECT_EQ(lr.user->id, uid);

    auto sess = db_->findSessionByJwtId(pair->jwt_id);
    ASSERT_TRUE(sess.has_value());
    EXPECT_EQ(sess->user_id, uid);
    EXPECT_FALSE(sess->revoked_at.has_value());
    EXPECT_EQ(sess->ip,         "10.0.0.1");
    EXPECT_EQ(sess->user_agent, "ua/1.0");
    EXPECT_EQ(sess->expires_at, pair->refresh_expires_at);
    // refresh_token в БД хранится как hash, не как plaintext.
    EXPECT_NE(sess->refresh_token_hash, pair->refresh_token);
    EXPECT_EQ(sess->refresh_token_hash,
              sa::AuthService::hashRefreshToken(pair->refresh_token));
}

TEST_F(AuthServiceTest, LoginUpdatesLastLogin) {
    auto uid = insertLocalUser("alice", "hunter2");
    auto u_before = db_->findUserById(uid);
    ASSERT_TRUE(u_before.has_value());
    EXPECT_FALSE(u_before->last_login_at.has_value());

    svc_->login("alice", "hunter2", "10.0.0.1", "ua");
    auto u_after = db_->findUserById(uid);
    ASSERT_TRUE(u_after.has_value());
    EXPECT_TRUE(u_after->last_login_at.has_value());
    EXPECT_EQ(u_after->last_login_ip, "10.0.0.1");
}

TEST_F(AuthServiceTest, LoginUnknownUserReturnsUserNotFound) {
    auto lr = svc_->login("nobody", "x", "ip", "ua");
    auto* err = std::get_if<sa::AuthService::LoginError>(&lr.outcome);
    ASSERT_NE(err, nullptr);
    EXPECT_EQ(*err, sa::AuthService::LoginError::UserNotFound);
}

TEST_F(AuthServiceTest, LoginWrongPasswordReturnsInvalidPassword) {
    insertLocalUser("alice", "hunter2");
    auto lr = svc_->login("alice", "WRONG", "ip", "ua");
    auto* err = std::get_if<sa::AuthService::LoginError>(&lr.outcome);
    ASSERT_NE(err, nullptr);
    EXPECT_EQ(*err, sa::AuthService::LoginError::InvalidPassword);
}

TEST_F(AuthServiceTest, LoginDisabledUserReturnsUserDisabled) {
    insertLocalUser("alice", "hunter2", sa::Role::Operator, true);
    auto lr = svc_->login("alice", "hunter2", "ip", "ua");
    auto* err = std::get_if<sa::AuthService::LoginError>(&lr.outcome);
    ASSERT_NE(err, nullptr);
    EXPECT_EQ(*err, sa::AuthService::LoginError::UserDisabled);
}

TEST_F(AuthServiceTest, LoginLdapSourceWithoutAuthenticatorReturnsInternalError) {
    // commit 18/24: existing LDAP-юзер + authenticator не выставлен — это
    // misconfig сервера, отдаём InternalError, чтобы оператор увидел
    // в логах. Раньше (до commit 18) маскировали под InvalidPassword.
    auto h = sa::PasswordHasher::hash("anything");
    sa::User u;
    u.username      = "ldap_alice";
    u.password_hash = *h;
    u.source        = sa::Source::Ldap;
    u.role          = sa::Role::Viewer;
    db_->insertUser(u);

    auto lr = svc_->login("ldap_alice", "anything", "ip", "ua");
    auto* err = std::get_if<sa::AuthService::LoginError>(&lr.outcome);
    ASSERT_NE(err, nullptr);
    EXPECT_EQ(*err, sa::AuthService::LoginError::InternalError);
}

TEST_F(AuthServiceTest, RefreshIssuesNewPairAndRevokesOld) {
    insertLocalUser("alice", "hunter2");
    auto lr = svc_->login("alice", "hunter2", "ip", "ua");
    auto& pair = std::get<sa::JwtIssuer::TokenPair>(lr.outcome);
    const auto old_jti = pair.jwt_id;
    const auto old_refresh = pair.refresh_token;

    auto rr = svc_->refresh(old_refresh, "ip2", "ua2");
    auto* new_pair = std::get_if<sa::JwtIssuer::TokenPair>(&rr.outcome);
    ASSERT_NE(new_pair, nullptr);
    EXPECT_NE(new_pair->jwt_id,        old_jti);
    EXPECT_NE(new_pair->refresh_token, old_refresh);

    // Старая сессия должна быть revoked.
    auto old_sess = db_->findSessionByJwtId(old_jti);
    ASSERT_TRUE(old_sess.has_value());
    EXPECT_TRUE(old_sess->revoked_at.has_value());
    // Новая — активна.
    auto new_sess = db_->findSessionByJwtId(new_pair->jwt_id);
    ASSERT_TRUE(new_sess.has_value());
    EXPECT_FALSE(new_sess->revoked_at.has_value());
    EXPECT_EQ(new_sess->ip, "ip2");
}

TEST_F(AuthServiceTest, RefreshReplayDetectedAfterRotation) {
    insertLocalUser("alice", "hunter2");
    auto lr = svc_->login("alice", "hunter2", "ip", "ua");
    const auto first_refresh = std::get<sa::JwtIssuer::TokenPair>(lr.outcome).refresh_token;

    // Первый refresh — OK.
    auto r1 = svc_->refresh(first_refresh, "ip", "ua");
    ASSERT_TRUE(std::holds_alternative<sa::JwtIssuer::TokenPair>(r1.outcome));

    // Второй с тем же — replay, отлуп.
    auto r2 = svc_->refresh(first_refresh, "ip", "ua");
    auto* err = std::get_if<sa::AuthService::RefreshError>(&r2.outcome);
    ASSERT_NE(err, nullptr);
    EXPECT_EQ(*err, sa::AuthService::RefreshError::TokenRevoked);
}

TEST_F(AuthServiceTest, RefreshUnknownTokenReturnsTokenNotFound) {
    auto rr = svc_->refresh("not-a-real-refresh-token", "ip", "ua");
    auto* err = std::get_if<sa::AuthService::RefreshError>(&rr.outcome);
    ASSERT_NE(err, nullptr);
    EXPECT_EQ(*err, sa::AuthService::RefreshError::TokenNotFound);
}

TEST_F(AuthServiceTest, RefreshEmptyTokenRejected) {
    auto rr = svc_->refresh("", "ip", "ua");
    EXPECT_TRUE(std::holds_alternative<sa::AuthService::RefreshError>(rr.outcome));
}

TEST_F(AuthServiceTest, RefreshDisabledUserBlocked) {
    auto uid = insertLocalUser("alice", "hunter2");
    auto lr = svc_->login("alice", "hunter2", "ip", "ua");
    const auto rt = std::get<sa::JwtIssuer::TokenPair>(lr.outcome).refresh_token;

    db_->setDisabled(uid, true);
    auto rr = svc_->refresh(rt, "ip", "ua");
    auto* err = std::get_if<sa::AuthService::RefreshError>(&rr.outcome);
    ASSERT_NE(err, nullptr);
    EXPECT_EQ(*err, sa::AuthService::RefreshError::UserDisabled);
}

TEST_F(AuthServiceTest, LogoutRevokesSession) {
    insertLocalUser("alice", "hunter2");
    auto lr = svc_->login("alice", "hunter2", "ip", "ua");
    const auto& pair = std::get<sa::JwtIssuer::TokenPair>(lr.outcome);

    EXPECT_TRUE(svc_->logout(pair.jwt_id));
    auto sess = db_->findSessionByJwtId(pair.jwt_id);
    ASSERT_TRUE(sess.has_value());
    EXPECT_TRUE(sess->revoked_at.has_value());
}

TEST_F(AuthServiceTest, LogoutUnknownJtiReturnsFalse) {
    EXPECT_FALSE(svc_->logout("does-not-exist"));
    EXPECT_FALSE(svc_->logout(""));
}

TEST_F(AuthServiceTest, LogoutIdempotentDoesNotShiftTimestamp) {
    insertLocalUser("alice", "hunter2");
    auto lr = svc_->login("alice", "hunter2", "ip", "ua");
    const auto& pair = std::get<sa::JwtIssuer::TokenPair>(lr.outcome);

    EXPECT_TRUE(svc_->logout(pair.jwt_id));
    auto first = db_->findSessionByJwtId(pair.jwt_id);
    ASSERT_TRUE(first.has_value());
    const auto first_ts = *first->revoked_at;

    // Re-logout — row should still be UPDATE'd (jwt_id matched), but
    // revoked_at preserved by COALESCE.
    svc_->logout(pair.jwt_id);
    auto second = db_->findSessionByJwtId(pair.jwt_id);
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(*second->revoked_at, first_ts);
}

TEST_F(AuthServiceTest, VerifyActiveAccessRequiresUnrevokedSession) {
    insertLocalUser("alice", "hunter2");
    auto lr = svc_->login("alice", "hunter2", "ip", "ua");
    const auto& pair = std::get<sa::JwtIssuer::TokenPair>(lr.outcome);

    auto c1 = svc_->verifyActiveAccess(pair.access_token);
    EXPECT_TRUE(c1.has_value());

    svc_->logout(pair.jwt_id);
    auto c2 = svc_->verifyActiveAccess(pair.access_token);
    EXPECT_FALSE(c2.has_value());
}

TEST_F(AuthServiceTest, VerifyActiveAccessRejectsBareJwtWithoutSession) {
    // Подписанный токен, но в БД нет сессии — отлуп. Защищает от случая
    // когда auth.db wiped, а старые токены ещё ходят.
    insertLocalUser("alice", "hunter2");
    auto lr = svc_->login("alice", "hunter2", "ip", "ua");
    const auto& pair = std::get<sa::JwtIssuer::TokenPair>(lr.outcome);

    // Простучиваем, что normal verify проходит.
    EXPECT_TRUE(svc_->verifyActiveAccess(pair.access_token).has_value());

    // Затираем sessions table.
    auto u = db_->findUserById(lr.user->id);
    ASSERT_TRUE(u.has_value());
    db_->revokeAllSessionsForUser(u->id, 1);

    // После revoke — не пускаем.
    EXPECT_FALSE(svc_->verifyActiveAccess(pair.access_token).has_value());
}

TEST_F(AuthServiceTest, GrantsResolverCallbackPopulatesJwt) {
    auto uid = insertLocalUser("alice", "hunter2");
    auto svc = std::make_unique<sa::AuthService>(*db_, *jwt_,
        [uid](std::int64_t user_id) {
            EXPECT_EQ(user_id, uid);
            return std::vector<sa::ChannelGrant>{
                {1, sa::ChannelPermission::Operate},
                {2, sa::ChannelPermission::View},
            };
        });

    auto lr = svc->login("alice", "hunter2", "ip", "ua");
    auto* pair = std::get_if<sa::JwtIssuer::TokenPair>(&lr.outcome);
    ASSERT_NE(pair, nullptr);

    auto claims = jwt_->verify(pair->access_token);
    ASSERT_TRUE(claims.has_value());
    ASSERT_EQ(claims->channel_grants.size(), 2u);
    EXPECT_EQ(claims->channel_grants[0].channel_id, 1);
    EXPECT_EQ(claims->channel_grants[0].permission, sa::ChannelPermission::Operate);
}

// ─── Admin user CRUD (commit 7/24) ─────────────────────────────────────

TEST_F(AuthServiceTest, AdminCreateUserAutoGeneratesPasswordWhenEmpty) {
    sa::AuthService::CreateUserRequest req;
    req.username = "bob";
    req.role     = sa::Role::Operator;
    auto out = svc_->adminCreateUser(req);
    auto* created = std::get_if<sa::AuthService::CreatedUser>(&out);
    ASSERT_NE(created, nullptr);
    EXPECT_EQ(created->user.username, "bob");
    EXPECT_EQ(created->user.role, sa::Role::Operator);
    EXPECT_EQ(created->user.source, sa::Source::Local);
    EXPECT_FALSE(created->plaintext_password.empty());
    // Сгенерированный — должен пройти hasher на верификацию.
    auto u = db_->findUserByUsername("bob");
    ASSERT_TRUE(u.has_value());
    EXPECT_TRUE(sa::PasswordHasher::verify(created->plaintext_password, u->password_hash));
}

TEST_F(AuthServiceTest, AdminCreateUserKeepsCallerPasswordSilent) {
    sa::AuthService::CreateUserRequest req;
    req.username = "carol";
    req.password = "secret-set-by-admin";
    req.role     = sa::Role::Viewer;
    auto out = svc_->adminCreateUser(req);
    auto* created = std::get_if<sa::AuthService::CreatedUser>(&out);
    ASSERT_NE(created, nullptr);
    // Plaintext возвращаем ТОЛЬКО при auto-generate. Admin сам ввёл — пусто.
    EXPECT_TRUE(created->plaintext_password.empty());
    // Но запись пароля корректна.
    auto u = db_->findUserByUsername("carol");
    ASSERT_TRUE(u.has_value());
    EXPECT_TRUE(sa::PasswordHasher::verify("secret-set-by-admin", u->password_hash));
}

TEST_F(AuthServiceTest, AdminCreateUserUsernameTaken) {
    insertLocalUser("dup", "p1");
    sa::AuthService::CreateUserRequest req;
    req.username = "dup";
    req.password = "p2";
    auto out = svc_->adminCreateUser(req);
    auto* err = std::get_if<sa::AuthService::AdminError>(&out);
    ASSERT_NE(err, nullptr);
    EXPECT_EQ(*err, sa::AuthService::AdminError::UsernameTaken);
}

TEST_F(AuthServiceTest, AdminCreateUserUsernameInvalid) {
    auto try_name = [&](const std::string& n) {
        sa::AuthService::CreateUserRequest req;
        req.username = n;
        req.password = "x";
        return svc_->adminCreateUser(req);
    };
    const std::vector<std::string> bads{
        "", " ", "with space", "has/slash", "has:colon", std::string(65, 'a')};
    for (const std::string& bad : bads) {
        auto out = try_name(bad);
        auto* err = std::get_if<sa::AuthService::AdminError>(&out);
        ASSERT_NE(err, nullptr) << "expected fail for: '" << bad << "'";
        EXPECT_EQ(*err, sa::AuthService::AdminError::UsernameInvalid)
            << "for '" << bad << "'";
    }
}

TEST_F(AuthServiceTest, AdminCreateUserEmailInvalid) {
    auto try_email = [&](const std::string& e) {
        sa::AuthService::CreateUserRequest req;
        req.username = "u" + std::to_string(std::hash<std::string>{}(e) % 1000);
        req.email    = e;
        req.password = "x";
        return svc_->adminCreateUser(req);
    };
    const std::vector<std::string> bads{
        "noatsign", "@startsat", "endsat@", "two@@signs"};
    for (const std::string& bad : bads) {
        auto out = try_email(bad);
        auto* err = std::get_if<sa::AuthService::AdminError>(&out);
        ASSERT_NE(err, nullptr) << "expected fail for: '" << bad << "'";
        EXPECT_EQ(*err, sa::AuthService::AdminError::EmailInvalid)
            << "for '" << bad << "'";
    }
}

TEST_F(AuthServiceTest, AdminCreateUserEmptyEmailIsAllowed) {
    sa::AuthService::CreateUserRequest req;
    req.username = "noemail";
    req.email    = "";
    req.password = "x";
    auto out = svc_->adminCreateUser(req);
    EXPECT_TRUE(std::holds_alternative<sa::AuthService::CreatedUser>(out));
}

TEST_F(AuthServiceTest, AdminUpdateUserRoleAndEmail) {
    auto uid = insertLocalUser("eve", "p", sa::Role::Viewer);
    sa::AuthService::UpdateUserRequest req;
    req.email  = "eve@example.com";
    req.role   = sa::Role::Operator;
    req.source = sa::Source::Local;
    auto out = svc_->adminUpdateUser(uid, req);
    auto* user = std::get_if<sa::User>(&out);
    ASSERT_NE(user, nullptr);
    EXPECT_EQ(user->email, "eve@example.com");
    EXPECT_EQ(user->role, sa::Role::Operator);
}

TEST_F(AuthServiceTest, AdminUpdateUserUserNotFound) {
    sa::AuthService::UpdateUserRequest req;
    req.role = sa::Role::Viewer;
    auto out = svc_->adminUpdateUser(99999, req);
    auto* err = std::get_if<sa::AuthService::AdminError>(&out);
    ASSERT_NE(err, nullptr);
    EXPECT_EQ(*err, sa::AuthService::AdminError::UserNotFound);
}

TEST_F(AuthServiceTest, AdminSetDisabledRevokesActiveSessions) {
    auto uid = insertLocalUser("frank", "p");
    auto lr = svc_->login("frank", "p", "ip", "ua");
    auto* pair = std::get_if<sa::JwtIssuer::TokenPair>(&lr.outcome);
    ASSERT_NE(pair, nullptr);

    EXPECT_TRUE(svc_->verifyActiveAccess(pair->access_token).has_value());

    EXPECT_TRUE(svc_->adminSetDisabled(uid, true));
    // После disable — JWT сам по себе ещё валиден, но сессия revoked.
    EXPECT_FALSE(svc_->verifyActiveAccess(pair->access_token).has_value());
}

TEST_F(AuthServiceTest, AdminResetPasswordGeneratesNewPlaintextAndForcesMustChange) {
    auto uid = insertLocalUser("greg", "old-pw");
    auto plaintext = svc_->adminResetPassword(uid);
    ASSERT_TRUE(plaintext.has_value());
    EXPECT_FALSE(plaintext->empty());
    EXPECT_NE(*plaintext, "old-pw");

    auto u = db_->findUserById(uid);
    ASSERT_TRUE(u.has_value());
    EXPECT_TRUE(u->must_change_password);
    EXPECT_TRUE(sa::PasswordHasher::verify(*plaintext, u->password_hash));
    EXPECT_FALSE(sa::PasswordHasher::verify("old-pw", u->password_hash));
}

TEST_F(AuthServiceTest, AdminResetPasswordRevokesAllSessions) {
    auto uid = insertLocalUser("helen", "p");
    auto lr = svc_->login("helen", "p", "ip", "ua");
    auto* pair = std::get_if<sa::JwtIssuer::TokenPair>(&lr.outcome);
    ASSERT_NE(pair, nullptr);
    EXPECT_TRUE(svc_->verifyActiveAccess(pair->access_token).has_value());

    EXPECT_TRUE(svc_->adminResetPassword(uid).has_value());

    // Все сессии должны быть revoked.
    EXPECT_FALSE(svc_->verifyActiveAccess(pair->access_token).has_value());
}

TEST_F(AuthServiceTest, AdminResetPasswordUserNotFound) {
    auto plaintext = svc_->adminResetPassword(123456);
    EXPECT_FALSE(plaintext.has_value());
}

TEST_F(AuthServiceTest, AdminListUsersIncludesNewlyCreated) {
    insertLocalUser("a", "p");
    insertLocalUser("b", "p");
    auto users = svc_->adminListUsers();
    EXPECT_EQ(users.size(), 2u);
}

TEST_F(AuthServiceTest, AdminGetUserNotFound) {
    EXPECT_FALSE(svc_->adminGetUser(7777).has_value());
}

// ─── Self-change password (commit 8/24) ────────────────────────────────

TEST_F(AuthServiceTest, ChangeOwnPasswordHappyPath) {
    insertLocalUser("ivan", "old-secret-12");
    auto lr = svc_->login("ivan", "old-secret-12", "ip", "ua");
    auto* pair = std::get_if<sa::JwtIssuer::TokenPair>(&lr.outcome);
    ASSERT_NE(pair, nullptr);

    auto err = svc_->changeOwnPassword(pair->access_token,
                                       "old-secret-12",
                                       "new-secret-34");
    EXPECT_FALSE(err.has_value());

    // Old password больше не работает.
    auto lr_old = svc_->login("ivan", "old-secret-12", "ip", "ua");
    EXPECT_TRUE(std::holds_alternative<sa::AuthService::LoginError>(lr_old.outcome));

    // New — работает.
    auto lr_new = svc_->login("ivan", "new-secret-34", "ip", "ua");
    EXPECT_TRUE(std::holds_alternative<sa::JwtIssuer::TokenPair>(lr_new.outcome));
}

TEST_F(AuthServiceTest, ChangeOwnPasswordKeepsCurrentSessionRevokesOthers) {
    insertLocalUser("jane", "p-12345678");
    auto a = svc_->login("jane", "p-12345678", "ip", "ua-A");
    auto b = svc_->login("jane", "p-12345678", "ip", "ua-B");
    auto* pa = std::get_if<sa::JwtIssuer::TokenPair>(&a.outcome);
    auto* pb = std::get_if<sa::JwtIssuer::TokenPair>(&b.outcome);
    ASSERT_NE(pa, nullptr);
    ASSERT_NE(pb, nullptr);

    auto err = svc_->changeOwnPassword(pa->access_token,
                                       "p-12345678",
                                       "p-87654321");
    EXPECT_FALSE(err.has_value());

    // Сессия A (current) — жива.
    EXPECT_TRUE(svc_->verifyActiveAccess(pa->access_token).has_value());
    // Сессия B (другой девайс) — revoked.
    EXPECT_FALSE(svc_->verifyActiveAccess(pb->access_token).has_value());
}

TEST_F(AuthServiceTest, ChangeOwnPasswordClearsMustChangeFlag) {
    auto uid = insertLocalUser("kate", "init-pass-123");
    db_->setMustChangePassword(uid, true);
    auto u = db_->findUserById(uid);
    ASSERT_TRUE(u && u->must_change_password);

    auto lr = svc_->login("kate", "init-pass-123", "ip", "ua");
    auto* pair = std::get_if<sa::JwtIssuer::TokenPair>(&lr.outcome);
    ASSERT_NE(pair, nullptr);

    auto err = svc_->changeOwnPassword(pair->access_token,
                                       "init-pass-123",
                                       "new-strong-pw");
    EXPECT_FALSE(err.has_value());

    auto u2 = db_->findUserById(uid);
    ASSERT_TRUE(u2.has_value());
    EXPECT_FALSE(u2->must_change_password);
}

TEST_F(AuthServiceTest, ChangeOwnPasswordWrongCurrentRejected) {
    insertLocalUser("leo", "the-actual-pw");
    auto lr = svc_->login("leo", "the-actual-pw", "ip", "ua");
    auto* pair = std::get_if<sa::JwtIssuer::TokenPair>(&lr.outcome);
    ASSERT_NE(pair, nullptr);

    auto err = svc_->changeOwnPassword(pair->access_token,
                                       "wrong-current-pw",
                                       "new-strong-pw");
    ASSERT_TRUE(err.has_value());
    EXPECT_EQ(*err, sa::AuthService::SelfPasswordError::CurrentPasswordWrong);
}

TEST_F(AuthServiceTest, ChangeOwnPasswordRejectsTooShort) {
    insertLocalUser("mary", "the-actual-pw");
    auto lr = svc_->login("mary", "the-actual-pw", "ip", "ua");
    auto* pair = std::get_if<sa::JwtIssuer::TokenPair>(&lr.outcome);
    ASSERT_NE(pair, nullptr);

    auto err = svc_->changeOwnPassword(pair->access_token,
                                       "the-actual-pw",
                                       "short");
    ASSERT_TRUE(err.has_value());
    EXPECT_EQ(*err, sa::AuthService::SelfPasswordError::NewPasswordWeak);
}

TEST_F(AuthServiceTest, ChangeOwnPasswordRejectsSameAsCurrent) {
    insertLocalUser("nick", "good-strong-pw");
    auto lr = svc_->login("nick", "good-strong-pw", "ip", "ua");
    auto* pair = std::get_if<sa::JwtIssuer::TokenPair>(&lr.outcome);
    ASSERT_NE(pair, nullptr);

    auto err = svc_->changeOwnPassword(pair->access_token,
                                       "good-strong-pw",
                                       "good-strong-pw");
    ASSERT_TRUE(err.has_value());
    EXPECT_EQ(*err, sa::AuthService::SelfPasswordError::NewPasswordWeak);
}

TEST_F(AuthServiceTest, ChangeOwnPasswordRejectsInvalidSession) {
    auto err = svc_->changeOwnPassword("not-a-valid-jwt",
                                       "any", "newstrongpw");
    ASSERT_TRUE(err.has_value());
    EXPECT_EQ(*err, sa::AuthService::SelfPasswordError::InvalidSession);
}

TEST_F(AuthServiceTest, ChangeOwnPasswordRejectsRevokedSession) {
    insertLocalUser("oleg", "p-12345678");
    auto lr = svc_->login("oleg", "p-12345678", "ip", "ua");
    auto* pair = std::get_if<sa::JwtIssuer::TokenPair>(&lr.outcome);
    ASSERT_NE(pair, nullptr);
    EXPECT_TRUE(svc_->logout(pair->jwt_id));

    auto err = svc_->changeOwnPassword(pair->access_token,
                                       "p-12345678",
                                       "p-87654321");
    ASSERT_TRUE(err.has_value());
    EXPECT_EQ(*err, sa::AuthService::SelfPasswordError::InvalidSession);
}

// ─── Bootstrap initial admin (commit 9/24) ─────────────────────────────

TEST_F(AuthServiceTest, BootstrapCreatesAdminWhenDbEmpty) {
    auto out = svc_->bootstrapInitialAdmin();
    EXPECT_TRUE(out.created);
    EXPECT_EQ(out.username, "admin");
    EXPECT_FALSE(out.plaintext_password.empty());
    EXPECT_GT(out.user_id, 0);

    auto u = db_->findUserByUsername("admin");
    ASSERT_TRUE(u.has_value());
    EXPECT_EQ(u->role, sa::Role::Admin);
    EXPECT_EQ(u->source, sa::Source::Local);
    EXPECT_TRUE(u->must_change_password);
    EXPECT_FALSE(u->disabled);
    EXPECT_TRUE(sa::PasswordHasher::verify(out.plaintext_password, u->password_hash));
}

TEST_F(AuthServiceTest, BootstrapNoOpWhenAdminExists) {
    insertLocalUser("rootguy", "p", sa::Role::Admin);
    auto out = svc_->bootstrapInitialAdmin();
    EXPECT_FALSE(out.created);
    EXPECT_TRUE(out.plaintext_password.empty());
    EXPECT_EQ(out.user_id, 0);

    // "admin" не должен появиться.
    EXPECT_FALSE(db_->findUserByUsername("admin").has_value());
}

TEST_F(AuthServiceTest, BootstrapTriggersAgainIfOnlyDisabledAdminExists) {
    auto uid = insertLocalUser("disabled-admin", "p", sa::Role::Admin);
    db_->setDisabled(uid, true);
    // hasAdminUser возвращает false, потому что есть только disabled admin.
    auto out = svc_->bootstrapInitialAdmin();
    EXPECT_TRUE(out.created);
    auto u = db_->findUserByUsername("admin");
    ASSERT_TRUE(u.has_value());
    EXPECT_EQ(u->role, sa::Role::Admin);
}

TEST_F(AuthServiceTest, BootstrapHonoursCustomUsername) {
    auto out = svc_->bootstrapInitialAdmin("rootcli");
    EXPECT_TRUE(out.created);
    EXPECT_EQ(out.username, "rootcli");
    auto u = db_->findUserByUsername("rootcli");
    ASSERT_TRUE(u.has_value());
}

TEST_F(AuthServiceTest, BootstrapPasswordWorksForLogin) {
    auto out = svc_->bootstrapInitialAdmin();
    ASSERT_TRUE(out.created);

    auto lr = svc_->login("admin", out.plaintext_password, "ip", "ua");
    auto* pair = std::get_if<sa::JwtIssuer::TokenPair>(&lr.outcome);
    ASSERT_NE(pair, nullptr);
}

// fix35 (A3.16) — bootstrap writes the plaintext password into a 0600
// file when one was wired in, and reports it back via password_file_path.
TEST_F(AuthServiceTest, BootstrapWritesPasswordFileWhenWired) {
    const auto pw_path = tmp_.parent_path() /
        ("init_admin_pw_bootstrap_" +
         std::string(::testing::UnitTest::GetInstance()
                          ->current_test_info()->name()) + ".txt");
    std::error_code ec;
    std::filesystem::remove(pw_path, ec);

    svc_->setInitialAdminPasswordFile(pw_path);
    auto out = svc_->bootstrapInitialAdmin();
    ASSERT_TRUE(out.created);
    EXPECT_EQ(out.password_file_path, pw_path);

    ASSERT_TRUE(std::filesystem::exists(pw_path));
    struct stat st{};
    ASSERT_EQ(::stat(pw_path.c_str(), &st), 0);
    EXPECT_EQ(st.st_mode & 0777, 0600);

    // File contains the plaintext + a single trailing newline. Login
    // with that contents (newline stripped) must succeed.
    std::ifstream f(pw_path);
    std::string from_file{
        std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
    ASSERT_FALSE(from_file.empty());
    if (from_file.back() == '\n') from_file.pop_back();
    EXPECT_EQ(from_file, out.plaintext_password);

    std::filesystem::remove(pw_path, ec);
}

// fix35 (A3.17) — the file gets removed automatically the first time
// the bootstrap admin self-changes their password.
TEST_F(AuthServiceTest, InitialPasswordFileRemovedAfterFirstChange) {
    const auto pw_path = tmp_.parent_path() /
        ("init_admin_pw_remove_" +
         std::string(::testing::UnitTest::GetInstance()
                          ->current_test_info()->name()) + ".txt");
    std::error_code ec;
    std::filesystem::remove(pw_path, ec);

    svc_->setInitialAdminPasswordFile(pw_path);
    auto out = svc_->bootstrapInitialAdmin();
    ASSERT_TRUE(out.created);
    ASSERT_TRUE(std::filesystem::exists(pw_path));

    // Login with the bootstrap password.
    auto lr = svc_->login("admin", out.plaintext_password, "ip", "ua");
    auto* pair = std::get_if<sa::JwtIssuer::TokenPair>(&lr.outcome);
    ASSERT_NE(pair, nullptr);
    ASSERT_TRUE(lr.user.has_value());
    EXPECT_TRUE(lr.user->must_change_password);
    EXPECT_EQ(lr.user->role, sa::Role::Admin);

    // First self-password-change must wipe the file AND clear the flag.
    auto err = svc_->changeOwnPassword(pair->access_token,
                                       out.plaintext_password,
                                       "new-strong-pass-1234");
    EXPECT_FALSE(err.has_value()) << "changeOwnPassword should succeed";

    EXPECT_FALSE(std::filesystem::exists(pw_path))
        << "initial_admin_password file should be auto-deleted";

    auto u = db_->findUserById(out.user_id);
    ASSERT_TRUE(u.has_value());
    EXPECT_FALSE(u->must_change_password);
}

// fix35 (A3.16) — non-bootstrap users (Operator/Viewer) must NOT trigger
// the file deletion even if a path is wired and the file happens to
// exist. Removal is gated on (Role::Admin && previous_must_change).
TEST_F(AuthServiceTest, InitialPasswordFileSurvivesNonBootstrapChange) {
    const auto pw_path = tmp_.parent_path() /
        ("init_admin_pw_survive_" +
         std::string(::testing::UnitTest::GetInstance()
                          ->current_test_info()->name()) + ".txt");
    std::error_code ec;
    std::filesystem::remove(pw_path, ec);
    {
        std::ofstream f(pw_path);
        f << "stale-bootstrap-pw\n";
    }
    ::chmod(pw_path.c_str(), 0600);
    ASSERT_TRUE(std::filesystem::exists(pw_path));

    svc_->setInitialAdminPasswordFile(pw_path);

    // Operator user, no must_change flag — change must NOT touch file.
    insertLocalUser("opuser", "current-pw-1234", sa::Role::Operator);
    auto lr = svc_->login("opuser", "current-pw-1234", "ip", "ua");
    auto* pair = std::get_if<sa::JwtIssuer::TokenPair>(&lr.outcome);
    ASSERT_NE(pair, nullptr);

    auto err = svc_->changeOwnPassword(pair->access_token,
                                       "current-pw-1234",
                                       "next-pw-87654321");
    EXPECT_FALSE(err.has_value());
    EXPECT_TRUE(std::filesystem::exists(pw_path))
        << "non-admin password change must NOT delete bootstrap pw file";

    std::filesystem::remove(pw_path, ec);
}

TEST_F(AuthServiceTest, ChangeOwnPasswordRejectsDisabledUser) {
    auto uid = insertLocalUser("petr", "p-12345678");
    auto lr = svc_->login("petr", "p-12345678", "ip", "ua");
    auto* pair = std::get_if<sa::JwtIssuer::TokenPair>(&lr.outcome);
    ASSERT_NE(pair, nullptr);
    db_->setDisabled(uid, true);

    auto err = svc_->changeOwnPassword(pair->access_token,
                                       "p-12345678",
                                       "p-87654321");
    ASSERT_TRUE(err.has_value());
    EXPECT_EQ(*err, sa::AuthService::SelfPasswordError::UserDisabled);
}

// ─── 24h TTL initial password (commit 11/24) ───────────────────────────

TEST_F(AuthServiceTest, BootstrapSetsInitialPasswordExpiry) {
    auto out = svc_->bootstrapInitialAdmin();
    ASSERT_TRUE(out.created);
    auto u = db_->findUserById(out.user_id);
    ASSERT_TRUE(u.has_value());
    ASSERT_TRUE(u->initial_password_expires_at.has_value());
    // Окно ровно 24h от created_at.
    EXPECT_EQ(*u->initial_password_expires_at, u->created_at + 24*60*60);
}

TEST_F(AuthServiceTest, AdminCreateUserMustChangeSetsExpiry) {
    sa::AuthService::CreateUserRequest req;
    req.username = "newop";
    req.role     = sa::Role::Operator;
    req.must_change_password = true;
    auto res = svc_->adminCreateUser(req);
    auto* ok = std::get_if<sa::AuthService::CreatedUser>(&res);
    ASSERT_NE(ok, nullptr);
    ASSERT_TRUE(ok->user.initial_password_expires_at.has_value());
    EXPECT_EQ(*ok->user.initial_password_expires_at,
              ok->user.created_at + 24*60*60);
}

TEST_F(AuthServiceTest, AdminCreateUserNoMustChangeNoExpiry) {
    // Пермпароль (must_change=false) — никакого TTL.
    sa::AuthService::CreateUserRequest req;
    req.username = "perm";
    req.password = "very-strong-password-12345";
    req.role     = sa::Role::Viewer;
    req.must_change_password = false;
    auto res = svc_->adminCreateUser(req);
    auto* ok = std::get_if<sa::AuthService::CreatedUser>(&res);
    ASSERT_NE(ok, nullptr);
    EXPECT_FALSE(ok->user.initial_password_expires_at.has_value());
}

TEST_F(AuthServiceTest, AdminResetPasswordSetsFreshExpiry) {
    auto uid = insertLocalUser("foo", "old-password", sa::Role::Operator);
    // Сразу сделаем "expired" сценарий через setInitialPasswordExpiry в
    // прошлое — потом reset должен заново открыть окно.
    ASSERT_TRUE(db_->setInitialPasswordExpiry(uid, 1));  // long expired
    auto pw = svc_->adminResetPassword(uid);
    ASSERT_TRUE(pw.has_value());
    auto u = db_->findUserById(uid);
    ASSERT_TRUE(u.has_value());
    ASSERT_TRUE(u->initial_password_expires_at.has_value());
    // Новое окно — далёкое будущее.
    EXPECT_GT(*u->initial_password_expires_at, 1'000'000'000);
    EXPECT_TRUE(u->must_change_password);
}

TEST_F(AuthServiceTest, LoginRejectsExpiredInitialPassword) {
    auto uid = insertLocalUser("oldie", "still-good", sa::Role::Operator);
    ASSERT_TRUE(db_->setMustChangePassword(uid, true));
    ASSERT_TRUE(db_->setInitialPasswordExpiry(uid, 1));  // эпоха назад

    auto lr = svc_->login("oldie", "still-good", "ip", "ua");
    auto* err = std::get_if<sa::AuthService::LoginError>(&lr.outcome);
    ASSERT_NE(err, nullptr);
    EXPECT_EQ(*err, sa::AuthService::LoginError::InitialPasswordExpired);
}

TEST_F(AuthServiceTest, LoginAcceptsUnexpiredInitialPassword) {
    auto uid = insertLocalUser("freshy", "ok-password", sa::Role::Operator);
    ASSERT_TRUE(db_->setMustChangePassword(uid, true));
    // 1 час в будущее — окно ещё открыто.
    const auto now =
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    ASSERT_TRUE(db_->setInitialPasswordExpiry(uid, now + 3600));

    auto lr = svc_->login("freshy", "ok-password", "ip", "ua");
    auto* pair = std::get_if<sa::JwtIssuer::TokenPair>(&lr.outcome);
    ASSERT_NE(pair, nullptr);
}

TEST_F(AuthServiceTest, LoginRejectsExpiredBeforeWrongPassword) {
    // Если пароль неверен, мы НЕ должны раскрывать факт expired —
    // значит сначала проверяется password, потом expiry. Тест: с чужим
    // паролем при просроченной сессии должны получить InvalidPassword,
    // а не InitialPasswordExpired.
    auto uid = insertLocalUser("careful", "good-password", sa::Role::Operator);
    ASSERT_TRUE(db_->setMustChangePassword(uid, true));
    ASSERT_TRUE(db_->setInitialPasswordExpiry(uid, 1));

    auto lr = svc_->login("careful", "WRONG", "ip", "ua");
    auto* err = std::get_if<sa::AuthService::LoginError>(&lr.outcome);
    ASSERT_NE(err, nullptr);
    EXPECT_EQ(*err, sa::AuthService::LoginError::InvalidPassword);
}

TEST_F(AuthServiceTest, ChangeOwnPasswordClearsInitialPasswordExpiry) {
    auto uid = insertLocalUser("changer", "old-password-1", sa::Role::Operator);
    ASSERT_TRUE(db_->setMustChangePassword(uid, true));
    const auto now =
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    ASSERT_TRUE(db_->setInitialPasswordExpiry(uid, now + 3600));

    // Login успешен — окно не истекло.
    auto lr = svc_->login("changer", "old-password-1", "ip", "ua");
    auto* pair = std::get_if<sa::JwtIssuer::TokenPair>(&lr.outcome);
    ASSERT_NE(pair, nullptr);

    auto err = svc_->changeOwnPassword(pair->access_token,
                                       "old-password-1",
                                       "new-password-2");
    EXPECT_FALSE(err.has_value());

    auto u = db_->findUserById(uid);
    ASSERT_TRUE(u.has_value());
    EXPECT_FALSE(u->must_change_password);
    EXPECT_FALSE(u->initial_password_expires_at.has_value());
}

TEST_F(AuthServiceTest, AdminResetClearsExpiryAfterUserChangesPassword) {
    // Полный жизненный цикл: admin резет → юзер логинится по временному →
    // юзер меняет пароль → expiry должен исчезнуть.
    auto uid = insertLocalUser("cycle", "initial-password",
                               sa::Role::Operator);
    auto reset_pw = svc_->adminResetPassword(uid);
    ASSERT_TRUE(reset_pw.has_value());

    auto u_after_reset = db_->findUserById(uid);
    ASSERT_TRUE(u_after_reset.has_value());
    ASSERT_TRUE(u_after_reset->initial_password_expires_at.has_value());
    EXPECT_TRUE(u_after_reset->must_change_password);

    auto lr = svc_->login("cycle", *reset_pw, "ip", "ua");
    auto* pair = std::get_if<sa::JwtIssuer::TokenPair>(&lr.outcome);
    ASSERT_NE(pair, nullptr);

    auto err = svc_->changeOwnPassword(pair->access_token,
                                       *reset_pw,
                                       "permanent-password-9");
    EXPECT_FALSE(err.has_value());

    auto u_final = db_->findUserById(uid);
    ASSERT_TRUE(u_final.has_value());
    EXPECT_FALSE(u_final->must_change_password);
    EXPECT_FALSE(u_final->initial_password_expires_at.has_value());
}

// ─── Audit log emission (commit 12/24) ─────────────────────────────────

namespace {
int countAudit(sa::AuthDb& db, const std::string& event) {
    sa::AuditFilter f;
    f.event = event;
    return static_cast<int>(db.listAuditEvents(f).size());
}
}  // namespace

TEST_F(AuthServiceTest, AuditLoginOk) {
    insertLocalUser("alice", "p-12345678");
    auto lr = svc_->login("alice", "p-12345678", "10.0.0.1", "ua");
    ASSERT_TRUE(std::holds_alternative<sa::JwtIssuer::TokenPair>(lr.outcome));
    EXPECT_EQ(countAudit(*db_, "login.ok"), 1);
}

TEST_F(AuthServiceTest, AuditLoginFailUnknownUser) {
    svc_->login("ghost", "x", "10.0.0.2", "ua");
    sa::AuditFilter f; f.event = "login.fail";
    auto rows = db_->listAuditEvents(f);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_FALSE(rows[0].user_id.has_value());
    EXPECT_EQ(rows[0].username, "ghost");
    EXPECT_EQ(rows[0].ip, "10.0.0.2");
    // details содержат reason
    EXPECT_NE(rows[0].details_json.find("user_not_found"), std::string::npos);
}

TEST_F(AuthServiceTest, AuditLoginFailWrongPassword) {
    insertLocalUser("alice", "p-correct");
    svc_->login("alice", "WRONG", "ip", "ua");
    sa::AuditFilter f; f.event = "login.fail";
    auto rows = db_->listAuditEvents(f);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_NE(rows[0].details_json.find("invalid_password"), std::string::npos);
}

TEST_F(AuthServiceTest, AuditLogoutEmitsEvent) {
    insertLocalUser("alice", "p-12345678");
    auto lr = svc_->login("alice", "p-12345678", "ip", "ua");
    auto* pair = std::get_if<sa::JwtIssuer::TokenPair>(&lr.outcome);
    ASSERT_NE(pair, nullptr);
    EXPECT_TRUE(svc_->logout(pair->jwt_id));
    EXPECT_EQ(countAudit(*db_, "logout"), 1);
}

TEST_F(AuthServiceTest, AuditRefreshOkAndReplay) {
    insertLocalUser("alice", "p-12345678");
    auto lr = svc_->login("alice", "p-12345678", "ip", "ua");
    auto* pair = std::get_if<sa::JwtIssuer::TokenPair>(&lr.outcome);
    ASSERT_NE(pair, nullptr);

    auto rr1 = svc_->refresh(pair->refresh_token, "ip", "ua");
    ASSERT_TRUE(std::holds_alternative<sa::JwtIssuer::TokenPair>(rr1.outcome));
    EXPECT_EQ(countAudit(*db_, "refresh.ok"), 1);

    // Re-use старого refresh — replay.
    auto rr2 = svc_->refresh(pair->refresh_token, "ip", "ua");
    ASSERT_TRUE(std::holds_alternative<sa::AuthService::RefreshError>(rr2.outcome));
    EXPECT_EQ(countAudit(*db_, "refresh.replay"), 1);
}

TEST_F(AuthServiceTest, AuditPasswordChanged) {
    insertLocalUser("alice", "p-12345678");
    auto lr = svc_->login("alice", "p-12345678", "ip", "ua");
    auto* pair = std::get_if<sa::JwtIssuer::TokenPair>(&lr.outcome);
    ASSERT_NE(pair, nullptr);
    auto err = svc_->changeOwnPassword(pair->access_token,
                                       "p-12345678",
                                       "new-password-1");
    EXPECT_FALSE(err.has_value());
    EXPECT_EQ(countAudit(*db_, "password.changed"), 1);
}

TEST_F(AuthServiceTest, AuditBootstrapEmitsEvent) {
    auto out = svc_->bootstrapInitialAdmin();
    ASSERT_TRUE(out.created);
    EXPECT_EQ(countAudit(*db_, "bootstrap.created"), 1);
}

TEST_F(AuthServiceTest, AuditPurgeOlderThanDays) {
    // Вставим 3 события с разными ts вручную.
    sa::AuditEvent old1; old1.ts = 1; old1.event = "old";
    sa::AuditEvent old2; old2.ts = 2; old2.event = "old";
    sa::AuditEvent fresh; fresh.event = "fresh";
    // ts=0 → AuthDb проставит nowUnixSec().
    ASSERT_TRUE(db_->insertAuditEvent(old1));
    ASSERT_TRUE(db_->insertAuditEvent(old2));
    ASSERT_TRUE(db_->insertAuditEvent(fresh));

    // 1 день назад — fresh выживает, "old" с ts=1/2 всегда гораздо
    // старше — будут вычищены.
    const int removed = svc_->purgeAuditOlderThanDays(1);
    EXPECT_GE(removed, 2);

    EXPECT_EQ(countAudit(*db_, "old"), 0);
    EXPECT_EQ(countAudit(*db_, "fresh"), 1);
}

TEST_F(AuthServiceTest, AuditPurgeNegativeDaysIsNoop) {
    sa::AuditEvent e; e.event = "x";
    ASSERT_TRUE(db_->insertAuditEvent(e));
    EXPECT_EQ(svc_->purgeAuditOlderThanDays(0),  0);
    EXPECT_EQ(svc_->purgeAuditOlderThanDays(-7), 0);
    EXPECT_EQ(countAudit(*db_, "x"), 1);
}

// ── Brute-force lockout (commit 13/24) ──────────────────────────────────

TEST_F(AuthServiceTest, FailedLoginsBelowThresholdDoNotLock) {
    auto uid = insertLocalUser("victim", "correct-pw");
    sa::AuthService::LockoutPolicy p; p.threshold = 5; p.base_delay_sec = 30;
    svc_->setLockoutPolicy(p);

    for (int i = 0; i < 4; ++i) {
        auto lr = svc_->login("victim", "WRONG", "10.0.0.1", "ua");
        auto* err = std::get_if<sa::AuthService::LoginError>(&lr.outcome);
        ASSERT_NE(err, nullptr);
        EXPECT_EQ(*err, sa::AuthService::LoginError::InvalidPassword);
    }
    auto u = db_->findUserById(uid);
    ASSERT_TRUE(u.has_value());
    EXPECT_EQ(u->failed_login_count, 4);
    EXPECT_FALSE(u->locked_until.has_value());

    // Корректный пароль ВСЁ ещё работает — лок не должен был сработать.
    auto ok = svc_->login("victim", "correct-pw", "10.0.0.1", "ua");
    auto* pair = std::get_if<sa::JwtIssuer::TokenPair>(&ok.outcome);
    ASSERT_NE(pair, nullptr);
}

TEST_F(AuthServiceTest, ReachingThresholdLocksAccount) {
    insertLocalUser("victim", "correct-pw");
    sa::AuthService::LockoutPolicy p; p.threshold = 3; p.base_delay_sec = 60;
    svc_->setLockoutPolicy(p);

    for (int i = 0; i < 3; ++i) {
        auto lr = svc_->login("victim", "WRONG", "10.0.0.1", "ua");
        auto* err = std::get_if<sa::AuthService::LoginError>(&lr.outcome);
        ASSERT_NE(err, nullptr);
        EXPECT_EQ(*err, sa::AuthService::LoginError::InvalidPassword);
    }

    // 4-я попытка должна попасть в lockout — даже если пароль теперь верный.
    auto lr = svc_->login("victim", "correct-pw", "10.0.0.1", "ua");
    auto* err = std::get_if<sa::AuthService::LoginError>(&lr.outcome);
    ASSERT_NE(err, nullptr);
    EXPECT_EQ(*err, sa::AuthService::LoginError::AccountLocked);

    EXPECT_GE(countAudit(*db_, "login.locked"), 1);
}

TEST_F(AuthServiceTest, ExponentialDelayDoublesEachFailureAfterThreshold) {
    auto uid = insertLocalUser("victim", "correct-pw");
    sa::AuthService::LockoutPolicy p;
    p.threshold      = 2;
    p.base_delay_sec = 100;
    p.max_lock_sec   = 1'000'000;   // достаточно высокий, чтобы клампинг
                                    // не вмешался в первые 4 шага.
    svc_->setLockoutPolicy(p);

    // 1-я неудача — count=1, lock не нужен.
    svc_->login("victim", "WRONG", "10.0.0.1", "ua");
    auto u = db_->findUserById(uid);
    ASSERT_TRUE(u.has_value());
    EXPECT_EQ(u->failed_login_count, 1);
    EXPECT_FALSE(u->locked_until.has_value());

    // 2-я неудача — достигаем threshold, lock = base.
    svc_->login("victim", "WRONG", "10.0.0.1", "ua");
    u = db_->findUserById(uid);
    ASSERT_TRUE(u->locked_until.has_value());
    const auto lock1 = *u->locked_until;

    // Перед следующим тестом снимем lock и оставим count, чтобы
    // следующая попытка увидела expired-lock и пошла в проверку пароля.
    ASSERT_TRUE(db_->recordFailedLogin(uid, 2, std::nullopt));

    // 3-я неудача — count=3, лок = base * 2^1 = 200с.
    svc_->login("victim", "WRONG", "10.0.0.1", "ua");
    u = db_->findUserById(uid);
    ASSERT_TRUE(u->locked_until.has_value());
    const auto lock2 = *u->locked_until;

    ASSERT_TRUE(db_->recordFailedLogin(uid, 3, std::nullopt));
    svc_->login("victim", "WRONG", "10.0.0.1", "ua");
    u = db_->findUserById(uid);
    ASSERT_TRUE(u->locked_until.has_value());
    const auto lock3 = *u->locked_until;

    // Окна должны расти (с поправкой на ~now()): lock2 - now ≈ 2*base, lock3 - now ≈ 4*base.
    // Проверяем relative-ordering, не абсолютные значения.
    EXPECT_GT(lock2 - lock1, 90);   // ~+100с
    EXPECT_GT(lock3 - lock2, 180);  // ~+200с
}

TEST_F(AuthServiceTest, MaxLockSecCapsExponentialEscalation) {
    auto uid = insertLocalUser("victim", "correct-pw");
    sa::AuthService::LockoutPolicy p;
    p.threshold      = 1;
    p.base_delay_sec = 10;
    p.max_lock_sec   = 50;
    svc_->setLockoutPolicy(p);

    // Эмулируем 10 неудач — без cap'а получили бы 10*2^9=5120с;
    // с cap'ом каждый lock не больше 50с.
    for (int i = 0; i < 10; ++i) {
        ASSERT_TRUE(db_->recordFailedLogin(uid, i, std::nullopt));
        svc_->login("victim", "WRONG", "10.0.0.1", "ua");
        auto u = db_->findUserById(uid);
        ASSERT_TRUE(u->locked_until.has_value());
        const auto delta = *u->locked_until -
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
        EXPECT_LE(delta, 51);  // +1 на jitter тестовых часов
    }
}

TEST_F(AuthServiceTest, SuccessfulLoginClearsLockoutCounter) {
    auto uid = insertLocalUser("victim", "correct-pw");
    sa::AuthService::LockoutPolicy p; p.threshold = 100; p.base_delay_sec = 60;
    svc_->setLockoutPolicy(p);  // не блокируем, просто копим счётчик

    for (int i = 0; i < 3; ++i)
        svc_->login("victim", "WRONG", "10.0.0.1", "ua");
    auto u = db_->findUserById(uid);
    EXPECT_EQ(u->failed_login_count, 3);

    auto ok = svc_->login("victim", "correct-pw", "10.0.0.1", "ua");
    ASSERT_NE(std::get_if<sa::JwtIssuer::TokenPair>(&ok.outcome), nullptr);

    u = db_->findUserById(uid);
    EXPECT_EQ(u->failed_login_count, 0);
    EXPECT_FALSE(u->locked_until.has_value());
}

TEST_F(AuthServiceTest, AdminUnlockResetsCounterAndAllowsLogin) {
    auto uid = insertLocalUser("victim", "correct-pw");
    sa::AuthService::LockoutPolicy p; p.threshold = 2; p.base_delay_sec = 3600;
    svc_->setLockoutPolicy(p);

    svc_->login("victim", "WRONG", "10.0.0.1", "ua");
    svc_->login("victim", "WRONG", "10.0.0.1", "ua");
    {
        auto lr = svc_->login("victim", "correct-pw", "10.0.0.1", "ua");
        auto* err = std::get_if<sa::AuthService::LoginError>(&lr.outcome);
        ASSERT_NE(err, nullptr);
        EXPECT_EQ(*err, sa::AuthService::LoginError::AccountLocked);
    }

    EXPECT_TRUE(svc_->adminUnlockUser(uid));
    auto u = db_->findUserById(uid);
    EXPECT_EQ(u->failed_login_count, 0);
    EXPECT_FALSE(u->locked_until.has_value());

    auto ok = svc_->login("victim", "correct-pw", "10.0.0.1", "ua");
    EXPECT_NE(std::get_if<sa::JwtIssuer::TokenPair>(&ok.outcome), nullptr);
}

TEST_F(AuthServiceTest, AdminUnlockMissingUserReturnsFalse) {
    EXPECT_FALSE(svc_->adminUnlockUser(99999));
}

TEST_F(AuthServiceTest, LockoutEmitsLoginLockedAndLoginFailAuditEvents) {
    insertLocalUser("victim", "correct-pw");
    sa::AuthService::LockoutPolicy p; p.threshold = 2; p.base_delay_sec = 60;
    svc_->setLockoutPolicy(p);

    svc_->login("victim", "WRONG", "10.0.0.1", "ua");
    svc_->login("victim", "WRONG", "10.0.0.1", "ua");

    // 2 login.fail (по одному на каждую попытку) и 1 login.locked
    // (создан вторым промахом, который дотянул до threshold).
    EXPECT_EQ(countAudit(*db_, "login.fail"),   2);
    EXPECT_EQ(countAudit(*db_, "login.locked"), 1);
}

TEST_F(AuthServiceTest, ExpiredLockAllowsLoginAttemptAgain) {
    auto uid = insertLocalUser("victim", "correct-pw");
    // Ставим lock в прошлое — сервис должен пропустить проверку пароля.
    ASSERT_TRUE(db_->recordFailedLogin(uid, 5, std::int64_t{1}));
    auto ok = svc_->login("victim", "correct-pw", "10.0.0.1", "ua");
    EXPECT_NE(std::get_if<sa::JwtIssuer::TokenPair>(&ok.outcome), nullptr);

    auto u = db_->findUserById(uid);
    EXPECT_EQ(u->failed_login_count, 0);
    EXPECT_FALSE(u->locked_until.has_value());
}

TEST_F(AuthServiceTest, NonLocalSourceDoesNotIncrementCounter) {
    sa::User u;
    u.username = "ldap-user";
    u.password_hash = "$argon2id$placeholder";
    u.role          = sa::Role::Operator;
    u.source        = sa::Source::Ldap;
    auto id = db_->insertUser(u);
    ASSERT_TRUE(id.has_value());

    sa::AuthService::LockoutPolicy p; p.threshold = 2; p.base_delay_sec = 60;
    svc_->setLockoutPolicy(p);
    for (int i = 0; i < 5; ++i)
        svc_->login("ldap-user", "anything", "10.0.0.1", "ua");

    auto found = db_->findUserById(*id);
    EXPECT_EQ(found->failed_login_count, 0);
    EXPECT_FALSE(found->locked_until.has_value());
}

// ─── LDAP login flow (commit 18/24) ────────────────────────────────────
//
// Используем stub-authenticator, отдающий canned-результаты, — без
// зависимости от живого DC. Проверяем маршрутизацию (Local vs LDAP),
// автопровиженинг, обновление role/email при смене группы, маппинг
// reason→LoginError, отказ на NoRoleMapped и невмешательство в lockout.
class LdapAuthServiceTest : public ::testing::Test {
protected:
    std::filesystem::path tmp_;
    std::unique_ptr<sa::AuthDb>      db_;
    std::unique_ptr<sa::JwtIssuer>   jwt_;
    std::unique_ptr<sa::AuthService> svc_;

    sa::AuthService::LdapLoginContext stub_response_;
    int stub_calls_{0};
    std::string last_username_;
    std::string last_password_;

    void SetUp() override {
        tmp_ = std::filesystem::temp_directory_path() /
            ("ldap_auth_service_" +
             std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
             "_" + ::testing::UnitTest::GetInstance()->current_test_info()->name() + ".db");
        std::filesystem::remove(tmp_);

        db_  = std::make_unique<sa::AuthDb>(tmp_);
        ASSERT_TRUE(db_->open());
        jwt_ = std::make_unique<sa::JwtIssuer>(std::string(kSecret));

        // Default — LDAP «отключён», как-будто authenticator не выставлен.
        // Каждый тест переопределяет stub_response_ при необходимости.
        stub_response_.reason = sa::AuthService::LdapReason::Disabled;

        auto authn = [this](std::string_view u, std::string_view p) {
            ++stub_calls_;
            last_username_ = std::string(u);
            last_password_ = std::string(p);
            return stub_response_;
        };
        svc_ = std::make_unique<sa::AuthService>(
            *db_, *jwt_,
            [](std::int64_t) { return std::vector<sa::ChannelGrant>{}; },
            std::move(authn));
    }

    void TearDown() override {
        svc_.reset();
        jwt_.reset();
        db_.reset();
        std::error_code ec;
        std::filesystem::remove(tmp_, ec);
        std::filesystem::remove(tmp_.string() + "-wal", ec);
        std::filesystem::remove(tmp_.string() + "-shm", ec);
    }

    void setOk(sa::Role role,
               std::string email = "alice@corp",
               std::vector<std::string> groups = {"cn=Admins,ou=Groups"}) {
        stub_response_ = {};
        stub_response_.ok     = true;
        stub_response_.role   = role;
        stub_response_.email  = std::move(email);
        stub_response_.groups = std::move(groups);
        stub_response_.reason = sa::AuthService::LdapReason::Ok;
    }

    void setFail(sa::AuthService::LdapReason r,
                 std::string err = "stub fail") {
        stub_response_ = {};
        stub_response_.ok     = false;
        stub_response_.reason = r;
        stub_response_.error  = std::move(err);
    }
};

TEST_F(LdapAuthServiceTest, UnknownUserOkAutoProvisionsAndIssuesToken) {
    setOk(sa::Role::Operator);

    auto lr = svc_->login("alice", "secret", "10.0.0.1", "ua/1");
    auto* pair = std::get_if<sa::JwtIssuer::TokenPair>(&lr.outcome);
    ASSERT_NE(pair, nullptr) << "login should succeed";
    EXPECT_EQ(stub_calls_, 1);
    EXPECT_EQ(last_username_, "alice");
    EXPECT_EQ(last_password_, "secret");

    auto found = db_->findUserByUsername("alice");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->source, sa::Source::Ldap);
    EXPECT_EQ(found->role,   sa::Role::Operator);
    EXPECT_EQ(found->email,  "alice@corp");
    EXPECT_TRUE(found->password_hash.empty());
}

TEST_F(LdapAuthServiceTest, UnknownUserOkProvisionsWithMappedRoleAndEmail) {
    setOk(sa::Role::Admin, "bob@corp", {"cn=admins,ou=groups"});

    auto lr = svc_->login("bob", "secret", "ip", "ua");
    ASSERT_NE(std::get_if<sa::JwtIssuer::TokenPair>(&lr.outcome), nullptr);

    auto found = db_->findUserByUsername("bob");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->role,  sa::Role::Admin);
    EXPECT_EQ(found->email, "bob@corp");
}

TEST_F(LdapAuthServiceTest, ExistingLdapUserRoleUpdateOnNextLogin) {
    setOk(sa::Role::Viewer, "alice@corp");
    {
        auto lr = svc_->login("alice", "pw", "ip", "ua");
        ASSERT_NE(std::get_if<sa::JwtIssuer::TokenPair>(&lr.outcome), nullptr);
    }
    auto u1 = db_->findUserByUsername("alice");
    ASSERT_TRUE(u1.has_value());
    EXPECT_EQ(u1->role, sa::Role::Viewer);

    // Группа сменилась → новая роль на следующем логине.
    setOk(sa::Role::Operator, "alice@corp");
    {
        auto lr = svc_->login("alice", "pw", "ip", "ua");
        ASSERT_NE(std::get_if<sa::JwtIssuer::TokenPair>(&lr.outcome), nullptr);
    }
    auto u2 = db_->findUserByUsername("alice");
    ASSERT_TRUE(u2.has_value());
    EXPECT_EQ(u2->role, sa::Role::Operator);
    EXPECT_EQ(u2->source, sa::Source::Ldap);
}

TEST_F(LdapAuthServiceTest, InvalidCredentialsMappedTo401) {
    setFail(sa::AuthService::LdapReason::InvalidCredentials);

    auto lr = svc_->login("alice", "wrong", "ip", "ua");
    auto* err = std::get_if<sa::AuthService::LoginError>(&lr.outcome);
    ASSERT_NE(err, nullptr);
    EXPECT_EQ(*err, sa::AuthService::LoginError::InvalidPassword);
    // unknown user не должен быть автопровижнен на fail.
    EXPECT_FALSE(db_->findUserByUsername("alice").has_value());
}

TEST_F(LdapAuthServiceTest, UserNotFoundMappedTo401) {
    setFail(sa::AuthService::LdapReason::UserNotFound);
    auto lr = svc_->login("ghost", "x", "ip", "ua");
    auto* err = std::get_if<sa::AuthService::LoginError>(&lr.outcome);
    ASSERT_NE(err, nullptr);
    EXPECT_EQ(*err, sa::AuthService::LoginError::InvalidPassword);
}

TEST_F(LdapAuthServiceTest, ConnectionFailedMappedToInternalError) {
    setFail(sa::AuthService::LdapReason::ConnectionFailed, "dns fail");
    auto lr = svc_->login("alice", "pw", "ip", "ua");
    auto* err = std::get_if<sa::AuthService::LoginError>(&lr.outcome);
    ASSERT_NE(err, nullptr);
    EXPECT_EQ(*err, sa::AuthService::LoginError::InternalError);
}

TEST_F(LdapAuthServiceTest, NoRoleMappedReturnsUserDisabled) {
    setFail(sa::AuthService::LdapReason::NoRoleMapped);
    auto lr = svc_->login("alice", "pw", "ip", "ua");
    auto* err = std::get_if<sa::AuthService::LoginError>(&lr.outcome);
    ASSERT_NE(err, nullptr);
    EXPECT_EQ(*err, sa::AuthService::LoginError::UserDisabled);
}

TEST_F(LdapAuthServiceTest, EmptyPasswordRejectedBeforeAuthenticator) {
    setOk(sa::Role::Admin);  // даже если authenticator вернул бы Ok
    auto lr = svc_->login("alice", "", "ip", "ua");
    auto* err = std::get_if<sa::AuthService::LoginError>(&lr.outcome);
    ASSERT_NE(err, nullptr);
    EXPECT_EQ(*err, sa::AuthService::LoginError::InvalidPassword);
    EXPECT_EQ(stub_calls_, 0) << "empty password must short-circuit before LDAP";
}

TEST_F(LdapAuthServiceTest, LocalUserStaysLocalEvenWithLdapEnabled) {
    // Local юзер «alice» с известным паролем; LDAP-stub отдал бы Admin
    // на любой пароль — но Local-маршрут даже не должен вызывать stub.
    auto h = sa::PasswordHasher::hash("hunter2");
    sa::User u;
    u.username = "alice";
    u.password_hash = *h;
    u.role          = sa::Role::Operator;
    u.source        = sa::Source::Local;
    ASSERT_TRUE(db_->insertUser(u).has_value());

    setOk(sa::Role::Admin);
    auto lr = svc_->login("alice", "hunter2", "ip", "ua");
    ASSERT_NE(std::get_if<sa::JwtIssuer::TokenPair>(&lr.outcome), nullptr);
    EXPECT_EQ(stub_calls_, 0) << "Local user should not consult LDAP";

    auto found = db_->findUserByUsername("alice");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->source, sa::Source::Local);
    EXPECT_EQ(found->role,   sa::Role::Operator);  // не подменили
}

TEST_F(LdapAuthServiceTest, LdapDisabledFallsThroughToLocalUserNotFound) {
    // Authenticator отдаёт Disabled (admin выключил LDAP). Unknown user
    // должен получить UserNotFound — не InvalidPassword, не InternalError.
    stub_response_.reason = sa::AuthService::LdapReason::Disabled;
    stub_response_.ok     = false;

    auto lr = svc_->login("ghost", "x", "ip", "ua");
    auto* err = std::get_if<sa::AuthService::LoginError>(&lr.outcome);
    ASSERT_NE(err, nullptr);
    EXPECT_EQ(*err, sa::AuthService::LoginError::UserNotFound);
}

TEST_F(LdapAuthServiceTest, LdapDisabledExistingLdapUserReturnsInternalError) {
    // existing LDAP-user в БД, но authenticator вернул Disabled —
    // misconfig сервера, не «invalid creds».
    sa::User u;
    u.username = "existing";
    u.source   = sa::Source::Ldap;
    u.role     = sa::Role::Viewer;
    ASSERT_TRUE(db_->insertUser(u).has_value());

    stub_response_.reason = sa::AuthService::LdapReason::Disabled;
    stub_response_.ok     = false;
    auto lr = svc_->login("existing", "pw", "ip", "ua");
    auto* err = std::get_if<sa::AuthService::LoginError>(&lr.outcome);
    ASSERT_NE(err, nullptr);
    EXPECT_EQ(*err, sa::AuthService::LoginError::InternalError);
}

TEST_F(LdapAuthServiceTest, DisabledLdapUserBlockedAfterBindOk) {
    sa::User u;
    u.username = "frozen";
    u.source   = sa::Source::Ldap;
    u.role     = sa::Role::Viewer;
    u.disabled = true;
    ASSERT_TRUE(db_->insertUser(u).has_value());

    setOk(sa::Role::Operator);  // даже если bind успешен
    auto lr = svc_->login("frozen", "pw", "ip", "ua");
    auto* err = std::get_if<sa::AuthService::LoginError>(&lr.outcome);
    ASSERT_NE(err, nullptr);
    EXPECT_EQ(*err, sa::AuthService::LoginError::UserDisabled);
    EXPECT_EQ(stub_calls_, 0)
        << "disabled-flag должен отрезать ДО сетевого вызова";
}

TEST_F(LdapAuthServiceTest, LdapFailDoesNotIncrementLockoutCounter) {
    // commit 13 lockout — для LDAP не активируется. Серия неудач не
    // должна приводить к locked_until ни в одной row.
    setFail(sa::AuthService::LdapReason::InvalidCredentials);

    sa::AuthService::LockoutPolicy p; p.threshold = 2; p.base_delay_sec = 60;
    svc_->setLockoutPolicy(p);

    for (int i = 0; i < 5; ++i)
        svc_->login("alice", "wrong", "ip", "ua");

    // Юзер не существовал и не должен был быть провижнен на fail.
    EXPECT_FALSE(db_->findUserByUsername("alice").has_value());
}

// ─── pickRoleForGroups (commit 18/24) ─────────────────────────────────

TEST(PickRoleForGroups, PicksHighestPriorityWhenMultipleMatch) {
    std::vector<std::string> groups{
        "cn=Viewers,ou=Groups", "cn=Admins,ou=Groups", "cn=Misc"
    };
    std::map<std::string, sa::Role> map{
        {"cn=Viewers,ou=Groups",  sa::Role::Viewer},
        {"cn=Admins,ou=Groups",   sa::Role::Admin},
        {"cn=Operators,ou=Groups", sa::Role::Operator},
    };
    auto r = sa::AuthService::pickRoleForGroups(groups, map);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, sa::Role::Admin);
}

TEST(PickRoleForGroups, IsCaseInsensitiveOnDn) {
    std::vector<std::string> groups{"CN=Operators,OU=GROUPS,DC=corp"};
    std::map<std::string, sa::Role> map{
        {"cn=operators,ou=groups,dc=corp", sa::Role::Operator},
    };
    auto r = sa::AuthService::pickRoleForGroups(groups, map);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, sa::Role::Operator);
}

TEST(PickRoleForGroups, ReturnsNulloptWhenNoMatch) {
    std::vector<std::string> groups{"cn=Random,ou=Groups"};
    std::map<std::string, sa::Role> map{
        {"cn=Admins,ou=Groups", sa::Role::Admin},
    };
    EXPECT_FALSE(sa::AuthService::pickRoleForGroups(groups, map).has_value());
}

// ─── pickAclGrantsForGroups (commit 19/24) ────────────────────────────

TEST(PickAclGrantsForGroups, MapsMatchedGroupsToChannelGrants) {
    std::vector<std::string> groups{
        "cn=Editors,ou=Groups", "cn=Misc,ou=Groups"
    };
    std::vector<sa::AuthService::ChannelAclEntry> acl{
        {1, {{"cn=Editors,ou=Groups", sa::ChannelPermission::Operate}}},
        {2, {{"cn=Editors,ou=Groups", sa::ChannelPermission::View}}},
        {3, {{"cn=Admins,ou=Groups",  sa::ChannelPermission::Operate}}},
    };
    auto out = sa::AuthService::pickAclGrantsForGroups(groups, acl);
    ASSERT_EQ(out.size(), 2u);
    // Канал 3 не выдан — у юзера нет cn=Admins.
    bool got1 = false, got2 = false;
    for (const auto& g : out) {
        if (g.channel_id == 1) {
            EXPECT_EQ(g.permission, sa::ChannelPermission::Operate);
            got1 = true;
        }
        if (g.channel_id == 2) {
            EXPECT_EQ(g.permission, sa::ChannelPermission::View);
            got2 = true;
        }
        EXPECT_NE(g.channel_id, 3);
    }
    EXPECT_TRUE(got1);
    EXPECT_TRUE(got2);
}

TEST(PickAclGrantsForGroups, PicksHighestPermissionWhenMultipleGroupsMatch) {
    std::vector<std::string> groups{
        "cn=Viewers,ou=Groups", "cn=Editors,ou=Groups"
    };
    std::vector<sa::AuthService::ChannelAclEntry> acl{
        {7, {
            {"cn=Viewers,ou=Groups", sa::ChannelPermission::View},
            {"cn=Editors,ou=Groups", sa::ChannelPermission::Operate},
        }},
    };
    auto out = sa::AuthService::pickAclGrantsForGroups(groups, acl);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].channel_id, 7);
    EXPECT_EQ(out[0].permission, sa::ChannelPermission::Operate);
}

TEST(PickAclGrantsForGroups, MergesMultipleEntriesForSameChannelId) {
    // ACL может иметь дубликаты channel_id (например, из-за UI-merge'а
    // двух конфигов). Permission'ы сливаем в максимальную.
    std::vector<std::string> groups{"cn=A,ou=g", "cn=B,ou=g"};
    std::vector<sa::AuthService::ChannelAclEntry> acl{
        {5, {{"cn=A,ou=g", sa::ChannelPermission::View}}},
        {5, {{"cn=B,ou=g", sa::ChannelPermission::Operate}}},
    };
    auto out = sa::AuthService::pickAclGrantsForGroups(groups, acl);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].channel_id, 5);
    EXPECT_EQ(out[0].permission, sa::ChannelPermission::Operate);
}

TEST(PickAclGrantsForGroups, IsCaseInsensitive) {
    std::vector<std::string> groups{"CN=Admins,OU=GROUPS"};
    std::vector<sa::AuthService::ChannelAclEntry> acl{
        {1, {{"cn=admins,ou=groups", sa::ChannelPermission::Operate}}},
    };
    auto out = sa::AuthService::pickAclGrantsForGroups(groups, acl);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].permission, sa::ChannelPermission::Operate);
}

TEST(PickAclGrantsForGroups, EmptyInputsYieldEmptyResult) {
    EXPECT_TRUE(sa::AuthService::pickAclGrantsForGroups({}, {}).empty());
    EXPECT_TRUE(sa::AuthService::pickAclGrantsForGroups({"cn=x"}, {}).empty());
    std::vector<sa::AuthService::ChannelAclEntry> acl{
        {1, {{"cn=x", sa::ChannelPermission::View}}},
    };
    EXPECT_TRUE(sa::AuthService::pickAclGrantsForGroups({}, acl).empty());
}

TEST_F(LdapAuthServiceTest, ChannelGrantsBakedIntoJwtClaims) {
    setOk(sa::Role::Operator);
    stub_response_.channel_grants = {
        {1, sa::ChannelPermission::Operate},
        {2, sa::ChannelPermission::View},
    };

    auto lr = svc_->login("alice", "pw", "ip", "ua");
    auto* pair = std::get_if<sa::JwtIssuer::TokenPair>(&lr.outcome);
    ASSERT_NE(pair, nullptr);

    auto claims = jwt_->verify(pair->access_token);
    ASSERT_TRUE(claims.has_value());
    ASSERT_EQ(claims->channel_grants.size(), 2u);
    bool got1 = false, got2 = false;
    for (const auto& g : claims->channel_grants) {
        if (g.channel_id == 1) {
            got1 = true;
            EXPECT_EQ(g.permission, sa::ChannelPermission::Operate);
        }
        if (g.channel_id == 2) {
            got2 = true;
            EXPECT_EQ(g.permission, sa::ChannelPermission::View);
        }
    }
    EXPECT_TRUE(got1);
    EXPECT_TRUE(got2);
}

TEST_F(LdapAuthServiceTest, EmptyChannelGrantsIsValid) {
    // Юзер прошёл LDAP-bind, role замаплена, но channel_acl пуст —
    // JWT выдаётся с пустым channel_grants. RBAC применит только role.
    setOk(sa::Role::Admin);
    auto lr = svc_->login("admin", "pw", "ip", "ua");
    auto* pair = std::get_if<sa::JwtIssuer::TokenPair>(&lr.outcome);
    ASSERT_NE(pair, nullptr);

    auto claims = jwt_->verify(pair->access_token);
    ASSERT_TRUE(claims.has_value());
    EXPECT_TRUE(claims->channel_grants.empty());
    EXPECT_EQ(claims->role, sa::Role::Admin);
}

TEST(PickRoleForGroups, EmptyInputsReturnsNullopt) {
    EXPECT_FALSE(sa::AuthService::pickRoleForGroups({}, {}).has_value());
    EXPECT_FALSE(sa::AuthService::pickRoleForGroups({"cn=x"}, {}).has_value());
    std::map<std::string, sa::Role> map{{"cn=x", sa::Role::Viewer}};
    EXPECT_FALSE(sa::AuthService::pickRoleForGroups({}, map).has_value());
}

// ── LDAP cache for refresh (commit 20/24) ──────────────────────────────

TEST_F(LdapAuthServiceTest, LdapLoginPersistsGroupsCache) {
    setOk(sa::Role::Operator, "alice@corp",
          {"cn=Admins,ou=Groups", "cn=Ops,ou=Groups"});
    auto lr = svc_->login("alice", "pw", "ip", "ua");
    ASSERT_NE(std::get_if<sa::JwtIssuer::TokenPair>(&lr.outcome), nullptr);

    auto u = db_->findUserByUsername("alice");
    ASSERT_TRUE(u.has_value());
    ASSERT_TRUE(u->ldap_groups_cached_at.has_value());
    EXPECT_GT(*u->ldap_groups_cached_at, 0);

    // JSON-array нормализованный nlohmann/json — имена групп лежат
    // подстроками без потерь.
    EXPECT_NE(u->ldap_groups_json.find("cn=Admins,ou=Groups"), std::string::npos);
    EXPECT_NE(u->ldap_groups_json.find("cn=Ops,ou=Groups"), std::string::npos);
}

TEST_F(LdapAuthServiceTest, RefreshUsesCachedGroupsAndCallsResolver) {
    setOk(sa::Role::Operator, "alice@corp", {"cn=ops,ou=groups"});
    auto lr = svc_->login("alice", "pw", "ip", "ua");
    auto* pair = std::get_if<sa::JwtIssuer::TokenPair>(&lr.outcome);
    ASSERT_NE(pair, nullptr);
    const auto rt = pair->refresh_token;

    // Резолвер вернёт каналы из cached-группы. Это имитирует пересчёт
    // grants по живому ACL без обращения к LDAP.
    int resolver_calls = 0;
    std::vector<std::string> seen_groups;
    svc_->setLdapGrantsResolver(
        [&](const std::vector<std::string>& groups) {
            ++resolver_calls;
            seen_groups = groups;
            return std::vector<sa::ChannelGrant>{
                {42, sa::ChannelPermission::Operate}};
        });

    auto rr = svc_->refresh(rt, "ip2", "ua2");
    auto* rp = std::get_if<sa::JwtIssuer::TokenPair>(&rr.outcome);
    ASSERT_NE(rp, nullptr) << "refresh should succeed";

    EXPECT_EQ(resolver_calls, 1);
    ASSERT_EQ(seen_groups.size(), 1u);
    EXPECT_EQ(seen_groups[0], "cn=ops,ou=groups");

    // JWT должен содержать grants, посчитанные resolver'ом.
    auto claims = jwt_->verify(rp->access_token);
    ASSERT_TRUE(claims.has_value());
    ASSERT_EQ(claims->channel_grants.size(), 1u);
    EXPECT_EQ(claims->channel_grants[0].channel_id, 42);
    EXPECT_EQ(claims->channel_grants[0].permission,
              sa::ChannelPermission::Operate);
}

TEST_F(LdapAuthServiceTest, RefreshWithoutResolverYieldsEmptyGrants) {
    // Resolver не выставлен — refresh выдаёт токен с пустым channel_grants
    // (роль из user.role остаётся, per-channel access выключен).
    setOk(sa::Role::Operator, "alice@corp", {"cn=ops,ou=groups"});
    auto lr = svc_->login("alice", "pw", "ip", "ua");
    auto* pair = std::get_if<sa::JwtIssuer::TokenPair>(&lr.outcome);
    ASSERT_NE(pair, nullptr);

    auto rr = svc_->refresh(pair->refresh_token, "ip", "ua");
    auto* rp = std::get_if<sa::JwtIssuer::TokenPair>(&rr.outcome);
    ASSERT_NE(rp, nullptr);

    auto claims = jwt_->verify(rp->access_token);
    ASSERT_TRUE(claims.has_value());
    EXPECT_TRUE(claims->channel_grants.empty());
    EXPECT_EQ(claims->role, sa::Role::Operator);
}

TEST_F(LdapAuthServiceTest, RefreshLdapCacheExpiredReturnsErr) {
    setOk(sa::Role::Operator);
    auto lr = svc_->login("alice", "pw", "ip", "ua");
    auto* pair = std::get_if<sa::JwtIssuer::TokenPair>(&lr.outcome);
    ASSERT_NE(pair, nullptr);

    // Моделируем «очень старый кэш» — сдвигаем cached_at в далёкое
    // прошлое. Policy оставляем дефолтную (7 суток); 100 дней назад
    // гарантированно за окном.
    auto u = db_->findUserByUsername("alice");
    ASSERT_TRUE(u.has_value());
    ASSERT_TRUE(db_->updateLdapGroupsCache(
        u->id, u->ldap_groups_json,
        *u->ldap_groups_cached_at - 100 * 86400));

    auto rr = svc_->refresh(pair->refresh_token, "ip", "ua");
    auto* err = std::get_if<sa::AuthService::RefreshError>(&rr.outcome);
    ASSERT_NE(err, nullptr);
    EXPECT_EQ(*err, sa::AuthService::RefreshError::LdapCacheExpired);
}

TEST_F(LdapAuthServiceTest, RefreshCachePolicyShortensWindow) {
    // Меняем policy на 1 секунду; через 2 секунды refresh должен
    // отдать LdapCacheExpired. Здесь зануляем cached_at вручную, чтобы
    // не делать sleep — semantically эквивалентно.
    setOk(sa::Role::Operator);
    auto lr = svc_->login("alice", "pw", "ip", "ua");
    auto* pair = std::get_if<sa::JwtIssuer::TokenPair>(&lr.outcome);
    ASSERT_NE(pair, nullptr);

    sa::AuthService::LdapCachePolicy p;
    p.max_age_sec = 1;
    svc_->setLdapCachePolicy(p);

    // Сдвигаем cached_at на 5 секунд назад — > 1s.
    auto u = db_->findUserByUsername("alice");
    ASSERT_TRUE(u.has_value());
    ASSERT_TRUE(db_->updateLdapGroupsCache(
        u->id, u->ldap_groups_json,
        *u->ldap_groups_cached_at - 5));

    auto rr = svc_->refresh(pair->refresh_token, "ip", "ua");
    auto* err = std::get_if<sa::AuthService::RefreshError>(&rr.outcome);
    ASSERT_NE(err, nullptr);
    EXPECT_EQ(*err, sa::AuthService::RefreshError::LdapCacheExpired);
}

TEST(RefreshErrorName, LdapCacheExpiredHasName) {
    EXPECT_STREQ(
        sa::refreshErrorName(sa::AuthService::RefreshError::LdapCacheExpired),
        "ldap_cache_expired");
}
