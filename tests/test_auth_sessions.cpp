// fix32 B2 — own-sessions API.
//
// Тестируем тонкие обёртки AuthService::listOwnActiveSessions /
// revokeOwnSession над AuthDb::listActiveSessionsForUser /
// revokeSessionByJwtIdForUser. Также проверяем побочный эффект
// touchSession через verifyActiveAccess (last_seen_at заполняется).

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <thread>
#include <variant>
#include <vector>

#include "auth/AuthDb.h"
#include "auth/AuthService.h"
#include "auth/JwtIssuer.h"
#include "auth/PasswordHasher.h"

namespace sa = liveqx::auth;

namespace {

constexpr const char* kSecret = "0123456789abcdef0123456789abcdef";

class AuthSessionsTest : public ::testing::Test {
protected:
    std::filesystem::path tmp_;
    std::unique_ptr<sa::AuthDb>      db_;
    std::unique_ptr<sa::JwtIssuer>   jwt_;
    std::unique_ptr<sa::AuthService> svc_;

    void SetUp() override {
        tmp_ = std::filesystem::temp_directory_path() /
            ("auth_sessions_test_" +
             std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
             "_" + ::testing::UnitTest::GetInstance()->current_test_info()->name() +
             ".db");
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
                                 const std::string& password) {
        auto h = sa::PasswordHasher::hash(password);
        EXPECT_TRUE(h.has_value());
        sa::User u;
        u.username      = name;
        u.password_hash = *h;
        u.role          = sa::Role::Operator;
        u.source        = sa::Source::Local;
        auto id = db_->insertUser(u);
        EXPECT_TRUE(id.has_value());
        return *id;
    }

    sa::JwtIssuer::TokenPair loginPair(const std::string& name,
                                       const std::string& pw,
                                       const std::string& ip = "10.0.0.1",
                                       const std::string& ua = "ua/1.0") {
        auto lr = svc_->login(name, pw, ip, ua);
        auto* pair = std::get_if<sa::JwtIssuer::TokenPair>(&lr.outcome);
        EXPECT_NE(pair, nullptr);
        return *pair;
    }
};

}  // namespace

// ── listOwnActiveSessions ────────────────────────────────────────────

TEST_F(AuthSessionsTest, ListReturnsOnlyOwnSessions) {
    auto a_id = insertLocalUser("alice", "pw");
    insertLocalUser("bob",   "pw");
    loginPair("alice", "pw", "1.1.1.1");
    loginPair("alice", "pw", "1.1.1.2");
    loginPair("bob",   "pw", "2.2.2.2");

    auto a_sessions = svc_->listOwnActiveSessions(a_id);
    EXPECT_EQ(a_sessions.size(), 2u);
    for (const auto& s : a_sessions) EXPECT_EQ(s.user_id, a_id);
}

TEST_F(AuthSessionsTest, ListExcludesRevokedSessions) {
    auto uid = insertLocalUser("alice", "pw");
    auto p1 = loginPair("alice", "pw");
    loginPair("alice", "pw");  // вторая сессия активна

    EXPECT_TRUE(svc_->logout(p1.jwt_id));   // revoke первую

    auto active = svc_->listOwnActiveSessions(uid);
    EXPECT_EQ(active.size(), 1u);
    for (const auto& s : active) EXPECT_NE(s.jwt_id, p1.jwt_id);
}

TEST_F(AuthSessionsTest, VerifyActiveAccessTouchesLastSeen) {
    auto uid = insertLocalUser("alice", "pw");
    auto pair = loginPair("alice", "pw");

    // До verifyActiveAccess last_seen_at NULL — touchSession ещё не вызывался.
    auto before = db_->findSessionByJwtId(pair.jwt_id);
    ASSERT_TRUE(before.has_value());
    EXPECT_FALSE(before->last_seen_at.has_value());

    auto claims = svc_->verifyActiveAccess(pair.access_token);
    ASSERT_TRUE(claims.has_value());

    auto after = db_->findSessionByJwtId(pair.jwt_id);
    ASSERT_TRUE(after.has_value());
    EXPECT_TRUE(after->last_seen_at.has_value());
    EXPECT_GT(*after->last_seen_at, 0);

    // Список активных сессий должен теперь содержать last_seen_at.
    auto active = svc_->listOwnActiveSessions(uid);
    ASSERT_EQ(active.size(), 1u);
    ASSERT_TRUE(active[0].last_seen_at.has_value());
    EXPECT_EQ(active[0].last_seen_at, after->last_seen_at);
}

// ── revokeOwnSession ─────────────────────────────────────────────────

TEST_F(AuthSessionsTest, RevokeOwnSessionInvalidatesIt) {
    auto uid = insertLocalUser("alice", "pw");
    auto pair = loginPair("alice", "pw");

    // Изначально valid.
    EXPECT_TRUE(svc_->verifyActiveAccess(pair.access_token).has_value());

    EXPECT_TRUE(svc_->revokeOwnSession(pair.jwt_id, uid));

    // После revoke токен больше не валиден.
    EXPECT_FALSE(svc_->verifyActiveAccess(pair.access_token).has_value());

    // listOwnActiveSessions больше не возвращает её.
    EXPECT_TRUE(svc_->listOwnActiveSessions(uid).empty());
}

TEST_F(AuthSessionsTest, RevokeOwnSessionIsIdempotent) {
    auto uid = insertLocalUser("alice", "pw");
    auto pair = loginPair("alice", "pw");

    EXPECT_TRUE (svc_->revokeOwnSession(pair.jwt_id, uid));
    EXPECT_FALSE(svc_->revokeOwnSession(pair.jwt_id, uid));  // already revoked
}

TEST_F(AuthSessionsTest, RevokeRefusesForeignSession) {
    auto a_id = insertLocalUser("alice", "pw");
    auto b_id = insertLocalUser("bob",   "pw");
    auto bob_pair = loginPair("bob", "pw");

    // Alice пытается ревокать сессию Bob'а — должно вернуть false и
    // НЕ ревокнуть строку.
    EXPECT_FALSE(svc_->revokeOwnSession(bob_pair.jwt_id, a_id));

    // Сессия Bob'а всё ещё активна.
    EXPECT_TRUE(svc_->verifyActiveAccess(bob_pair.access_token).has_value());
    EXPECT_EQ(svc_->listOwnActiveSessions(b_id).size(), 1u);
}

TEST_F(AuthSessionsTest, RevokeUnknownJtiReturnsFalse) {
    auto uid = insertLocalUser("alice", "pw");
    EXPECT_FALSE(svc_->revokeOwnSession("does-not-exist", uid));
}

// ── touchSession throttle ────────────────────────────────────────────
//
// SQL-throttle 60s в verifyActiveAccess: повторный вызов в ту же
// секунду НЕ обновляет last_seen_at (поведение throttle'а — sql
// возвращает 0 changes; функция возвращает false).

TEST_F(AuthSessionsTest, TouchSessionThrottlesWithin60s) {
    insertLocalUser("alice", "pw");
    auto pair = loginPair("alice", "pw");

    const auto t = 1'700'000'000LL;
    EXPECT_TRUE (db_->touchSession(pair.jwt_id, t,      /*throttle_sec=*/60));
    EXPECT_FALSE(db_->touchSession(pair.jwt_id, t + 30, /*throttle_sec=*/60));
    EXPECT_TRUE (db_->touchSession(pair.jwt_id, t + 61, /*throttle_sec=*/60));
}

TEST_F(AuthSessionsTest, TouchSessionRefusesRevokedSession) {
    auto uid = insertLocalUser("alice", "pw");
    auto pair = loginPair("alice", "pw");
    EXPECT_TRUE(svc_->revokeOwnSession(pair.jwt_id, uid));

    EXPECT_FALSE(db_->touchSession(pair.jwt_id, 1'700'000'000LL,
                                   /*throttle_sec=*/60));
}
