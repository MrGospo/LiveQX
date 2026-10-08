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
    EXPECT_EQ(sqlite3_column_int(st, 0), 8);
    sqlite3_finalize(st);
    sqlite3_close(raw);
}

// ── schema v8: legacy auth_audit is archived, not dropped ──────────────────

namespace {

void rawExec(const fs::path& db_path, const char* sql) {
    sqlite3* raw = nullptr;
    ASSERT_EQ(sqlite3_open(db_path.string().c_str(), &raw), SQLITE_OK);
    char* err = nullptr;
    ASSERT_EQ(sqlite3_exec(raw, sql, nullptr, nullptr, &err), SQLITE_OK)
        << (err ? err : "");
    sqlite3_free(err);
    sqlite3_close(raw);
}

// First column of the first row, or -1 when the query fails / returns nothing.
int rawScalar(const fs::path& db_path, const char* sql) {
    sqlite3* raw = nullptr;
    if (sqlite3_open(db_path.string().c_str(), &raw) != SQLITE_OK) return -1;
    sqlite3_stmt* st = nullptr;
    int v = -1;
    if (sqlite3_prepare_v2(raw, sql, -1, &st, nullptr) == SQLITE_OK
            && sqlite3_step(st) == SQLITE_ROW) {
        v = sqlite3_column_int(st, 0);
    }
    sqlite3_finalize(st);
    sqlite3_close(raw);
    return v;
}

constexpr const char* kLegacyAuditDdl =
    "CREATE TABLE IF NOT EXISTS auth_audit ("
    " id INTEGER PRIMARY KEY, ts INTEGER NOT NULL, event TEXT NOT NULL,"
    " user_id INTEGER, username TEXT, ip TEXT, details_json TEXT);"
    "CREATE INDEX IF NOT EXISTS idx_audit_ts ON auth_audit(ts);"
    "CREATE INDEX IF NOT EXISTS idx_audit_user ON auth_audit(user_id);";

}  // namespace

TEST_F(AuthDbTest, UpgradeFromV7ArchivesLegacyAuditAndKeepsUsers) {
    {   // A user that must survive the upgrade.
        sa::AuthDb db(dbPath());
        ASSERT_TRUE(db.open());
        ASSERT_TRUE(db.insertUser(makeAdmin("keeper")).has_value());
    }
    // Turn the file into what a v7 install looks like: a populated
    // auth_audit table and user_version = 7.
    rawExec(dbPath(), kLegacyAuditDdl);
    rawExec(dbPath(),
        "INSERT INTO auth_audit(ts,event,username,ip) VALUES"
        " (1700000001,'login.ok','keeper','10.0.0.1'),"
        " (1700000002,'login.fail','keeper','10.0.0.2');"
        "PRAGMA user_version=7;");

    sa::AuthDb db(dbPath());
    ASSERT_TRUE(db.open());

    EXPECT_EQ(rawScalar(dbPath(),
        "SELECT count(*) FROM sqlite_master WHERE name='auth_audit'"), 0);
    EXPECT_EQ(rawScalar(dbPath(), "SELECT count(*) FROM auth_audit_archived"), 2);
    EXPECT_EQ(rawScalar(dbPath(), "PRAGMA user_version"), 8);
    EXPECT_EQ(rawScalar(dbPath(),
        "SELECT count(*) FROM sqlite_master WHERE name LIKE 'idx_audit_%'"), 0);
    ASSERT_TRUE(db.findUserByUsername("keeper").has_value());
}

TEST_F(AuthDbTest, ReopenAfterUpgradeDoesNotTouchArchive) {
    {
        sa::AuthDb db(dbPath());
        ASSERT_TRUE(db.open());
    }
    rawExec(dbPath(), kLegacyAuditDdl);
    rawExec(dbPath(),
        "INSERT INTO auth_audit(ts,event) VALUES (1,'login.ok');"
        "PRAGMA user_version=7;");
    {
        sa::AuthDb db(dbPath());
        ASSERT_TRUE(db.open());
    }
    {
        sa::AuthDb db(dbPath());
        ASSERT_TRUE(db.open());
    }
    EXPECT_EQ(rawScalar(dbPath(), "SELECT count(*) FROM auth_audit_archived"), 1);
    EXPECT_EQ(rawScalar(dbPath(), "PRAGMA user_version"), 8);
}

TEST_F(AuthDbTest, UpgradeKeepsBothWhenArchiveNameIsTaken) {
    // An earlier archive must never be overwritten or merged away.
    {
        sa::AuthDb db(dbPath());
        ASSERT_TRUE(db.open());
    }
    rawExec(dbPath(), kLegacyAuditDdl);
    rawExec(dbPath(),
        "INSERT INTO auth_audit(ts,event) VALUES (1,'a'),(2,'b');"
        "CREATE TABLE auth_audit_archived (id INTEGER PRIMARY KEY, ts INTEGER);"
        "INSERT INTO auth_audit_archived(ts) VALUES (99);"
        "PRAGMA user_version=7;");

    sa::AuthDb db(dbPath());
    ASSERT_TRUE(db.open());

    EXPECT_EQ(rawScalar(dbPath(), "SELECT count(*) FROM auth_audit_archived"), 1);
    EXPECT_EQ(rawScalar(dbPath(),
        "SELECT count(*) FROM sqlite_master"
        " WHERE type='table' AND name LIKE 'auth_audit_archived_%'"), 1);
    EXPECT_EQ(rawScalar(dbPath(),
        "SELECT count(*) FROM sqlite_master WHERE name='auth_audit'"), 0);
}

TEST_F(AuthDbTest, FreshDbHasNoLegacyAuditTables) {
    sa::AuthDb db(dbPath());
    ASSERT_TRUE(db.open());
    EXPECT_EQ(rawScalar(dbPath(),
        "SELECT count(*) FROM sqlite_master WHERE name LIKE 'auth_audit%'"), 0);
    EXPECT_EQ(rawScalar(dbPath(), "PRAGMA user_version"), 8);
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
