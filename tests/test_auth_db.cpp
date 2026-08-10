// fix22 commit 2/24 — AuthDb open + миграция + базовые user CRUD.
// Полные сценарии (login/lockout/RBAC) — в последующих коммитах.

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>

#include <sqlite3.h>

#include "auth/AuthDb.h"
#include "auth/AuthTypes.h"

namespace fs = std::filesystem;
namespace sa = liveqx::auth;

namespace {

class AuthDbTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto stem = "auth_db_test_" + std::to_string(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        tmp_dir_ = fs::temp_directory_path() / stem;
        fs::create_directories(tmp_dir_);
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(tmp_dir_, ec);
    }

    fs::path dbPath() const { return tmp_dir_ / "auth.db"; }
    fs::path tmp_dir_;
};

sa::User makeAdmin(std::string name = "admin") {
    sa::User u;
    u.username      = std::move(name);
    u.email         = "ops@example.com";
    u.password_hash = "$argon2id$placeholder";  // фактический hash приходит в commit 3
    u.source        = sa::Source::Local;
    u.role          = sa::Role::Admin;
    u.must_change_password         = true;
    u.initial_password_expires_at  = 1700000000;
    u.created_at                   = 1600000000;
    return u;
}

}  // namespace

TEST_F(AuthDbTest, OpenCreatesFileAndAppliesSchema) {
    sa::AuthDb db(dbPath());
    ASSERT_TRUE(db.open());
    EXPECT_TRUE(db.ok());
    EXPECT_TRUE(fs::exists(dbPath()));

    // PRAGMA user_version должен совпасть с current schema_version
    // (бампается при добавлении миграций; v7 — system_time_config).
    sqlite3* raw = nullptr;
    ASSERT_EQ(sqlite3_open(dbPath().string().c_str(), &raw), SQLITE_OK);
    sqlite3_stmt* st = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(raw, "PRAGMA user_version;", -1, &st, nullptr),
              SQLITE_OK);
    ASSERT_EQ(sqlite3_step(st), SQLITE_ROW);
    EXPECT_EQ(sqlite3_column_int(st, 0), 7);
    sqlite3_finalize(st);
    sqlite3_close(raw);
}

TEST_F(AuthDbTest, FreshDbHasNoAdmin) {
    sa::AuthDb db(dbPath());
    ASSERT_TRUE(db.open());
    EXPECT_FALSE(db.hasAdminUser());
}

TEST_F(AuthDbTest, InsertAdminThenHasAdminUserReturnsTrue) {
    sa::AuthDb db(dbPath());
    ASSERT_TRUE(db.open());

    auto id = db.insertUser(makeAdmin());
    ASSERT_TRUE(id.has_value());
    EXPECT_GT(*id, 0);
    EXPECT_TRUE(db.hasAdminUser());
}

TEST_F(AuthDbTest, DisabledAdminDoesNotCount) {
    sa::AuthDb db(dbPath());
    ASSERT_TRUE(db.open());

    auto u = makeAdmin();
    u.disabled = true;
    auto id = db.insertUser(u);
    ASSERT_TRUE(id.has_value());
    EXPECT_FALSE(db.hasAdminUser());
}

TEST_F(AuthDbTest, InsertDuplicateUsernameFails) {
    sa::AuthDb db(dbPath());
    ASSERT_TRUE(db.open());
    auto first = db.insertUser(makeAdmin());
    ASSERT_TRUE(first.has_value());

    auto second = db.insertUser(makeAdmin());
    EXPECT_FALSE(second.has_value());
}

TEST_F(AuthDbTest, FindUserByUsernameRoundTrip) {
    sa::AuthDb db(dbPath());
    ASSERT_TRUE(db.open());
    auto id = db.insertUser(makeAdmin("alice"));
    ASSERT_TRUE(id.has_value());

    auto found = db.findUserByUsername("alice");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->id,                    *id);
    EXPECT_EQ(found->username,              "alice");
    EXPECT_EQ(found->email,                 "ops@example.com");
    EXPECT_EQ(found->password_hash,         "$argon2id$placeholder");
    EXPECT_EQ(found->source,                sa::Source::Local);
    EXPECT_EQ(found->role,                  sa::Role::Admin);
    EXPECT_TRUE(found->must_change_password);
    ASSERT_TRUE(found->initial_password_expires_at.has_value());
    EXPECT_EQ(*found->initial_password_expires_at, 1700000000);
    EXPECT_FALSE(found->disabled);

    auto missing = db.findUserByUsername("nobody");
    EXPECT_FALSE(missing.has_value());
}

TEST_F(AuthDbTest, FindUserByIdMirrorsByUsername) {
    sa::AuthDb db(dbPath());
    ASSERT_TRUE(db.open());
    auto id = db.insertUser(makeAdmin("bob"));
    ASSERT_TRUE(id.has_value());

    auto by_id = db.findUserById(*id);
    ASSERT_TRUE(by_id.has_value());
    EXPECT_EQ(by_id->username, "bob");

    EXPECT_FALSE(db.findUserById(999999).has_value());
}

TEST_F(AuthDbTest, UpdatePasswordHashClearsMustChange) {
    sa::AuthDb db(dbPath());
    ASSERT_TRUE(db.open());
    auto id = db.insertUser(makeAdmin());
    ASSERT_TRUE(id.has_value());

    EXPECT_TRUE(db.updatePasswordHash(*id, "$argon2id$new", 1700000005,
                                       /*clear_must_change=*/true));

    auto u = db.findUserById(*id);
    ASSERT_TRUE(u.has_value());
    EXPECT_EQ(u->password_hash, "$argon2id$new");
    ASSERT_TRUE(u->password_changed_at.has_value());
    EXPECT_EQ(*u->password_changed_at, 1700000005);
    EXPECT_FALSE(u->must_change_password);
}

TEST_F(AuthDbTest, UpdatePasswordHashKeepsMustChangeWhenNotCleared) {
    sa::AuthDb db(dbPath());
    ASSERT_TRUE(db.open());
    auto id = db.insertUser(makeAdmin());
    ASSERT_TRUE(id.has_value());

    EXPECT_TRUE(db.updatePasswordHash(*id, "$argon2id$another", 1700000010,
                                       /*clear_must_change=*/false));

    auto u = db.findUserById(*id);
    ASSERT_TRUE(u.has_value());
    EXPECT_TRUE(u->must_change_password);  // флаг сохранился
}

TEST_F(AuthDbTest, SetLastLoginPersists) {
    sa::AuthDb db(dbPath());
    ASSERT_TRUE(db.open());
    auto id = db.insertUser(makeAdmin());
    ASSERT_TRUE(id.has_value());

    EXPECT_TRUE(db.setLastLogin(*id, 1700001234, "10.0.0.5"));
    auto u = db.findUserById(*id);
    ASSERT_TRUE(u.has_value());
    ASSERT_TRUE(u->last_login_at.has_value());
    EXPECT_EQ(*u->last_login_at, 1700001234);
    EXPECT_EQ(u->last_login_ip,  "10.0.0.5");
}

TEST_F(AuthDbTest, ListUsersReturnsAllInIdOrder) {
    sa::AuthDb db(dbPath());
    ASSERT_TRUE(db.open());
    auto id1 = db.insertUser(makeAdmin("alice"));
    auto id2 = db.insertUser(makeAdmin("bob"));
    ASSERT_TRUE(id1.has_value());
    ASSERT_TRUE(id2.has_value());

    auto rows = db.listUsers();
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows[0].username, "alice");
    EXPECT_EQ(rows[1].username, "bob");
}

TEST_F(AuthDbTest, SetDisabledFlipsHasAdmin) {
    sa::AuthDb db(dbPath());
    ASSERT_TRUE(db.open());
    auto id = db.insertUser(makeAdmin());
    ASSERT_TRUE(id.has_value());
    EXPECT_TRUE(db.hasAdminUser());

    EXPECT_TRUE(db.setDisabled(*id, true));
    EXPECT_FALSE(db.hasAdminUser());

    EXPECT_TRUE(db.setDisabled(*id, false));
    EXPECT_TRUE(db.hasAdminUser());
}

TEST_F(AuthDbTest, ClearInitialPasswordExpiry) {
    sa::AuthDb db(dbPath());
    ASSERT_TRUE(db.open());
    auto id = db.insertUser(makeAdmin());
    ASSERT_TRUE(id.has_value());

    EXPECT_TRUE(db.clearInitialPasswordExpiry(*id));
    auto u = db.findUserById(*id);
    ASSERT_TRUE(u.has_value());
    EXPECT_FALSE(u->initial_password_expires_at.has_value());
}

TEST_F(AuthDbTest, ReopenSeesPersistedRows) {
    auto path = dbPath();
    {
        sa::AuthDb db(path);
        ASSERT_TRUE(db.open());
        auto id = db.insertUser(makeAdmin("persisted"));
        ASSERT_TRUE(id.has_value());
    }
    sa::AuthDb db2(path);
    ASSERT_TRUE(db2.open());
    auto u = db2.findUserByUsername("persisted");
    ASSERT_TRUE(u.has_value());
    EXPECT_EQ(u->role, sa::Role::Admin);
}

TEST_F(AuthDbTest, RoleNameRoundTrip) {
    EXPECT_STREQ(sa::roleName(sa::Role::Admin),    "admin");
    EXPECT_STREQ(sa::roleName(sa::Role::Operator), "operator");
    EXPECT_STREQ(sa::roleName(sa::Role::Viewer),   "viewer");

    EXPECT_EQ(sa::roleFromString("admin"),    sa::Role::Admin);
    EXPECT_EQ(sa::roleFromString("operator"), sa::Role::Operator);
    EXPECT_EQ(sa::roleFromString("viewer"),   sa::Role::Viewer);
    EXPECT_FALSE(sa::roleFromString("root").has_value());
}

// ─── Audit log (commit 12/24) ─────────────────────────────────────────

TEST_F(AuthDbTest, InsertAuditEventRoundTrip) {
    sa::AuthDb db(dbPath());
    ASSERT_TRUE(db.open());

    sa::AuditEvent e;
    e.ts           = 1'700'000'000;
    e.event        = "login.ok";
    e.user_id      = 42;
    e.username     = "alice";
    e.ip           = "10.0.0.1";
    e.details_json = R"({"jti":"abc"})";
    ASSERT_TRUE(db.insertAuditEvent(e));

    sa::AuditFilter f;
    auto rows = db.listAuditEvents(f);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].event, "login.ok");
    ASSERT_TRUE(rows[0].user_id.has_value());
    EXPECT_EQ(*rows[0].user_id, 42);
    EXPECT_EQ(rows[0].username, "alice");
    EXPECT_EQ(rows[0].ip, "10.0.0.1");
    EXPECT_EQ(rows[0].details_json, R"({"jti":"abc"})");
    EXPECT_GT(rows[0].id, 0);
    EXPECT_EQ(rows[0].ts, 1'700'000'000);
}

TEST_F(AuthDbTest, ListAuditEventsOrderByTsDesc) {
    sa::AuthDb db(dbPath());
    ASSERT_TRUE(db.open());

    for (int i = 0; i < 5; ++i) {
        sa::AuditEvent e;
        e.ts    = 1'700'000'000 + i;
        e.event = "login.ok";
        ASSERT_TRUE(db.insertAuditEvent(e));
    }

    sa::AuditFilter f;
    auto rows = db.listAuditEvents(f);
    ASSERT_EQ(rows.size(), 5u);
    // DESC по ts: первый — самый поздний.
    EXPECT_EQ(rows.front().ts, 1'700'000'004);
    EXPECT_EQ(rows.back().ts,  1'700'000'000);
}

TEST_F(AuthDbTest, ListAuditEventsFilters) {
    sa::AuthDb db(dbPath());
    ASSERT_TRUE(db.open());

    auto put = [&](std::int64_t ts, const std::string& event,
                   std::optional<std::int64_t> uid,
                   const std::string& uname) {
        sa::AuditEvent e;
        e.ts = ts; e.event = event; e.user_id = uid; e.username = uname;
        ASSERT_TRUE(db.insertAuditEvent(e));
    };
    put(100, "login.ok",   1, "a");
    put(200, "login.fail", 2, "b");
    put(300, "logout",     1, "a");
    put(400, "login.ok",   3, "c");

    sa::AuditFilter f;
    f.event = "login.ok";
    auto rows = db.listAuditEvents(f);
    ASSERT_EQ(rows.size(), 2u);
    for (const auto& r : rows) EXPECT_EQ(r.event, "login.ok");

    sa::AuditFilter f2;
    f2.user_id = 1;
    auto rows2 = db.listAuditEvents(f2);
    ASSERT_EQ(rows2.size(), 2u);

    sa::AuditFilter f3;
    f3.from_ts = 200; f3.to_ts = 400;
    auto rows3 = db.listAuditEvents(f3);
    ASSERT_EQ(rows3.size(), 2u);
    EXPECT_EQ(rows3[0].ts, 300);
    EXPECT_EQ(rows3[1].ts, 200);
}

TEST_F(AuthDbTest, ListAuditEventsLimitOffset) {
    sa::AuthDb db(dbPath());
    ASSERT_TRUE(db.open());

    for (int i = 0; i < 10; ++i) {
        sa::AuditEvent e;
        e.ts = 1'700'000'000 + i; e.event = "x";
        ASSERT_TRUE(db.insertAuditEvent(e));
    }

    sa::AuditFilter f;
    f.limit = 3; f.offset = 0;
    auto a = db.listAuditEvents(f);
    EXPECT_EQ(a.size(), 3u);

    f.offset = 3;
    auto b = db.listAuditEvents(f);
    EXPECT_EQ(b.size(), 3u);
    EXPECT_NE(a[0].id, b[0].id);

    // Cap 1000 — даже если попросили 99999, не должно крэшнуться.
    f.limit = 99999; f.offset = 0;
    auto c = db.listAuditEvents(f);
    EXPECT_EQ(c.size(), 10u);
}

TEST_F(AuthDbTest, PurgeAuditOlderThan) {
    sa::AuthDb db(dbPath());
    ASSERT_TRUE(db.open());

    for (int i = 0; i < 5; ++i) {
        sa::AuditEvent e;
        e.ts = 100 + i * 100; e.event = "x";
        ASSERT_TRUE(db.insertAuditEvent(e));
    }
    // Purge всё с ts < 250 — это записи на ts=100 и ts=200.
    EXPECT_EQ(db.purgeAuditOlderThan(250), 2);
    sa::AuditFilter f;
    auto rows = db.listAuditEvents(f);
    EXPECT_EQ(rows.size(), 3u);
    for (const auto& r : rows) EXPECT_GE(r.ts, 250);

    // Idempotent: повторный purge с тем же cutoff ничего не удаляет.
    EXPECT_EQ(db.purgeAuditOlderThan(250), 0);
}

// ── Brute-force lockout (commit 13/24) ───────────────────────────────────

TEST_F(AuthDbTest, FreshUserHasZeroFailedLoginAndNoLock) {
    sa::AuthDb db(dbPath());
    ASSERT_TRUE(db.open());
    auto u = makeAdmin("victim");
    auto id = db.insertUser(u);
    ASSERT_TRUE(id.has_value());

    auto found = db.findUserById(*id);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->failed_login_count, 0);
    EXPECT_FALSE(found->locked_until.has_value());
}

TEST_F(AuthDbTest, RecordFailedLoginPersistsCountAndLockTimestamp) {
    sa::AuthDb db(dbPath());
    ASSERT_TRUE(db.open());
    auto id = db.insertUser(makeAdmin("victim"));
    ASSERT_TRUE(id.has_value());

    EXPECT_TRUE(db.recordFailedLogin(*id, 5, std::int64_t{2'000'000'000}));
    auto u = db.findUserById(*id);
    ASSERT_TRUE(u.has_value());
    EXPECT_EQ(u->failed_login_count, 5);
    ASSERT_TRUE(u->locked_until.has_value());
    EXPECT_EQ(*u->locked_until, 2'000'000'000);

    // Запись с std::nullopt в качестве lock — снимает lock но оставляет count.
    EXPECT_TRUE(db.recordFailedLogin(*id, 6, std::nullopt));
    u = db.findUserById(*id);
    ASSERT_TRUE(u.has_value());
    EXPECT_EQ(u->failed_login_count, 6);
    EXPECT_FALSE(u->locked_until.has_value());
}

TEST_F(AuthDbTest, JwtSecretInsertReadRotate) {
    sa::AuthDb db(dbPath());
    ASSERT_TRUE(db.open());
    EXPECT_FALSE(db.hasJwtSecret());

    std::vector<std::uint8_t> ct1{1, 2, 3, 4, 5, 6, 7, 8};
    ASSERT_TRUE(db.storeJwtSecret(ct1, /*rotated_at=*/1700000000));
    EXPECT_TRUE(db.hasJwtSecret());

    auto got = db.readJwtSecret();
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(*got, ct1);

    // Rotation — UPSERT перезаписывает.
    std::vector<std::uint8_t> ct2{9, 9, 9, 9};
    ASSERT_TRUE(db.storeJwtSecret(ct2, /*rotated_at=*/1700000999));
    auto got2 = db.readJwtSecret();
    ASSERT_TRUE(got2.has_value());
    EXPECT_EQ(*got2, ct2);
}

TEST_F(AuthDbTest, JwtSecretEmptyCiphertextRejected) {
    sa::AuthDb db(dbPath());
    ASSERT_TRUE(db.open());
    EXPECT_FALSE(db.storeJwtSecret({}, 1));
    EXPECT_FALSE(db.hasJwtSecret());
}

TEST_F(AuthDbTest, ClearFailedLoginResetsBothFields) {
    sa::AuthDb db(dbPath());
    ASSERT_TRUE(db.open());
    auto id = db.insertUser(makeAdmin("victim"));
    ASSERT_TRUE(id.has_value());

    ASSERT_TRUE(db.recordFailedLogin(*id, 7, std::int64_t{1'900'000'000}));
    EXPECT_TRUE(db.clearFailedLogin(*id));

    auto u = db.findUserById(*id);
    ASSERT_TRUE(u.has_value());
    EXPECT_EQ(u->failed_login_count, 0);
    EXPECT_FALSE(u->locked_until.has_value());

    // Идемпотентно: повторный clear на чистом юзере остаётся true.
    EXPECT_TRUE(db.clearFailedLogin(*id));
}

// ── LDAP groups cache (commit 20/24) ──────────────────────────────────

TEST_F(AuthDbTest, FreshUserHasEmptyLdapGroupsCache) {
    sa::AuthDb db(dbPath());
    ASSERT_TRUE(db.open());
    auto id = db.insertUser(makeAdmin("alice"));
    ASSERT_TRUE(id.has_value());

    auto u = db.findUserById(*id);
    ASSERT_TRUE(u.has_value());
    EXPECT_TRUE(u->ldap_groups_json.empty());
    EXPECT_FALSE(u->ldap_groups_cached_at.has_value());
}

TEST_F(AuthDbTest, UpdateLdapGroupsCachePersists) {
    sa::AuthDb db(dbPath());
    ASSERT_TRUE(db.open());
    auto id = db.insertUser(makeAdmin("bob"));
    ASSERT_TRUE(id.has_value());

    const std::string groups = R"(["cn=admins,ou=g","cn=ops,ou=g"])";
    ASSERT_TRUE(db.updateLdapGroupsCache(*id, groups, 1'700'000'000));

    auto u = db.findUserById(*id);
    ASSERT_TRUE(u.has_value());
    EXPECT_EQ(u->ldap_groups_json, groups);
    ASSERT_TRUE(u->ldap_groups_cached_at.has_value());
    EXPECT_EQ(*u->ldap_groups_cached_at, 1'700'000'000);
}

TEST_F(AuthDbTest, UpdateLdapGroupsCacheOverwrites) {
    sa::AuthDb db(dbPath());
    ASSERT_TRUE(db.open());
    auto id = db.insertUser(makeAdmin("carol"));
    ASSERT_TRUE(id.has_value());

    ASSERT_TRUE(db.updateLdapGroupsCache(*id, R"(["a"])", 1'700'000'000));
    ASSERT_TRUE(db.updateLdapGroupsCache(*id, R"(["x","y"])", 1'700'001'000));

    auto u = db.findUserById(*id);
    ASSERT_TRUE(u.has_value());
    EXPECT_EQ(u->ldap_groups_json, R"(["x","y"])");
    EXPECT_EQ(*u->ldap_groups_cached_at, 1'700'001'000);
}

TEST_F(AuthDbTest, UpdateLdapGroupsCacheUnknownUser) {
    sa::AuthDb db(dbPath());
    ASSERT_TRUE(db.open());
    EXPECT_FALSE(db.updateLdapGroupsCache(99999, "[]", 1));
}

// ── channel_permissions CRUD (commit 22/24) ──────────────────────────

TEST_F(AuthDbTest, SetChannelPermissionInsertsRow) {
    sa::AuthDb db(dbPath());
    ASSERT_TRUE(db.open());
    auto admin_id = db.insertUser(makeAdmin("admin"));
    auto user_u = makeAdmin("operator1"); user_u.role = sa::Role::Operator;
    auto user_id  = db.insertUser(user_u);
    ASSERT_TRUE(admin_id.has_value() && user_id.has_value());

    EXPECT_TRUE(db.setChannelPermission(*user_id, /*ch=*/7,
                                        sa::ChannelPermission::Operate,
                                        *admin_id, 1'700'000'000));

    auto grants = db.listChannelGrantsForUser(*user_id);
    ASSERT_EQ(grants.size(), 1u);
    EXPECT_EQ(grants[0].channel_id, 7);
    EXPECT_EQ(grants[0].permission, sa::ChannelPermission::Operate);
}

TEST_F(AuthDbTest, SetChannelPermissionUpserts) {
    sa::AuthDb db(dbPath());
    ASSERT_TRUE(db.open());
    auto admin_id = db.insertUser(makeAdmin("admin"));
    auto user_u = makeAdmin("op2"); user_u.role = sa::Role::Operator;
    auto user_id  = db.insertUser(user_u);
    ASSERT_TRUE(admin_id.has_value() && user_id.has_value());

    EXPECT_TRUE(db.setChannelPermission(*user_id, 5,
                                        sa::ChannelPermission::View,
                                        *admin_id, 1'700'000'000));
    // Повтор с той же парой → UPSERT обновляет permission.
    EXPECT_TRUE(db.setChannelPermission(*user_id, 5,
                                        sa::ChannelPermission::Operate,
                                        *admin_id, 1'700'001'000));

    auto rows = db.listChannelPermissionsForUser(*user_id);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].permission, sa::ChannelPermission::Operate);
    EXPECT_EQ(rows[0].granted_at, 1'700'001'000);
}

TEST_F(AuthDbTest, RemoveChannelPermissionDeletesRow) {
    sa::AuthDb db(dbPath());
    ASSERT_TRUE(db.open());
    auto admin_id = db.insertUser(makeAdmin("admin"));
    auto user_u = makeAdmin("op3"); user_u.role = sa::Role::Operator;
    auto user_id  = db.insertUser(user_u);
    ASSERT_TRUE(admin_id.has_value() && user_id.has_value());

    EXPECT_TRUE(db.setChannelPermission(*user_id, 1,
                                        sa::ChannelPermission::View,
                                        *admin_id, 1));
    EXPECT_TRUE(db.removeChannelPermission(*user_id, 1));
    EXPECT_TRUE(db.listChannelGrantsForUser(*user_id).empty());

    // Idempotent: повторный delete возвращает false (row не было) — нормально.
    EXPECT_FALSE(db.removeChannelPermission(*user_id, 1));
}

TEST_F(AuthDbTest, RemoveAllChannelPermissionsForUser) {
    sa::AuthDb db(dbPath());
    ASSERT_TRUE(db.open());
    auto admin_id = db.insertUser(makeAdmin("admin"));
    auto user_u = makeAdmin("op4"); user_u.role = sa::Role::Operator;
    auto user_id  = db.insertUser(user_u);

    db.setChannelPermission(*user_id, 1, sa::ChannelPermission::View,    *admin_id, 1);
    db.setChannelPermission(*user_id, 2, sa::ChannelPermission::Operate, *admin_id, 1);
    db.setChannelPermission(*user_id, 3, sa::ChannelPermission::View,    *admin_id, 1);

    EXPECT_EQ(db.removeAllChannelPermissionsForUser(*user_id), 3);
    EXPECT_TRUE(db.listChannelGrantsForUser(*user_id).empty());
}

TEST_F(AuthDbTest, ListChannelPermissionsForChannelJoinsUsername) {
    sa::AuthDb db(dbPath());
    ASSERT_TRUE(db.open());
    auto admin_id = db.insertUser(makeAdmin("admin"));
    auto u1 = makeAdmin("alice"); u1.role = sa::Role::Operator;
    auto u2 = makeAdmin("bob");   u2.role = sa::Role::Operator;
    auto a_id = db.insertUser(u1);
    auto b_id = db.insertUser(u2);
    ASSERT_TRUE(a_id.has_value() && b_id.has_value());

    db.setChannelPermission(*a_id, 42, sa::ChannelPermission::View,    *admin_id, 100);
    db.setChannelPermission(*b_id, 42, sa::ChannelPermission::Operate, *admin_id, 200);
    db.setChannelPermission(*a_id, 99, sa::ChannelPermission::Operate, *admin_id, 300);

    auto rows = db.listChannelPermissionsForChannel(42);
    ASSERT_EQ(rows.size(), 2u);
    // ORDER BY username — alice, bob.
    EXPECT_EQ(rows[0].username, "alice");
    EXPECT_EQ(rows[0].channel_id, 42);
    EXPECT_EQ(rows[0].permission, sa::ChannelPermission::View);
    EXPECT_EQ(rows[1].username, "bob");
    EXPECT_EQ(rows[1].permission, sa::ChannelPermission::Operate);
    EXPECT_EQ(rows[1].granted_at, 200);
}

TEST_F(AuthDbTest, ListChannelGrantsForUserOrdersByChannel) {
    sa::AuthDb db(dbPath());
    ASSERT_TRUE(db.open());
    auto admin_id = db.insertUser(makeAdmin("admin"));
    auto u = makeAdmin("op5"); u.role = sa::Role::Operator;
    auto user_id = db.insertUser(u);

    db.setChannelPermission(*user_id, 9,  sa::ChannelPermission::View,    *admin_id, 1);
    db.setChannelPermission(*user_id, 1,  sa::ChannelPermission::Operate, *admin_id, 1);
    db.setChannelPermission(*user_id, 5,  sa::ChannelPermission::View,    *admin_id, 1);

    auto grants = db.listChannelGrantsForUser(*user_id);
    ASSERT_EQ(grants.size(), 3u);
    EXPECT_EQ(grants[0].channel_id, 1);
    EXPECT_EQ(grants[1].channel_id, 5);
    EXPECT_EQ(grants[2].channel_id, 9);
}
