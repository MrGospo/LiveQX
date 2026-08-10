#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "auth/AuthTypes.h"

struct sqlite3;

namespace liveqx::auth {

// Owner of state/auth.db. WAL mode, BEGIN IMMEDIATE for writes.
// Single instance for the whole process; all auth modules go through it.
//
// Schema version bumps are migrations. The current schema is v1
// (commit 2/24). Lower-version DBs are migrated forward; higher-version
// DBs are refused (operator may be downgrading — preserve forensics).
//
// Corrupt-file handling mirrors ChannelStatePersistence: any open/
// migration failure renames `<path>.corrupt-<unix_ns>` and the AuthDb
// stays in not-ok state. Callers MUST check ok() before issuing queries.
class AuthDb {
public:
    explicit AuthDb(std::filesystem::path db_path);
    ~AuthDb();

    AuthDb(const AuthDb&)            = delete;
    AuthDb& operator=(const AuthDb&) = delete;

    // Open or create the file, run PRAGMAs and migrations to current
    // schema version. Returns true if the database is queryable.
    bool open();

    bool ok() const noexcept { return db_ != nullptr; }
    const std::filesystem::path& path() const noexcept { return path_; }

    // True iff there is at least one non-disabled admin user.
    bool hasAdminUser();

    // Count non-disabled admin users. Used by adminPurgeUser to refuse
    // wiping the last enabled admin (lock-out guard).
    int  countEnabledAdmins();

    // Insert a new user. Returns assigned id on success, std::nullopt
    // on UNIQUE-constraint violation (username taken) or DB error.
    std::optional<std::int64_t> insertUser(const User& u);

    std::optional<User> findUserByUsername(std::string_view username);
    std::optional<User> findUserById(std::int64_t id);

    // Update only the columns that AuthService routinely changes.
    bool updatePasswordHash(std::int64_t user_id,
                            std::string_view new_hash,
                            std::int64_t password_changed_at,
                            bool clear_must_change);

    bool setLastLogin(std::int64_t user_id,
                      std::int64_t when,
                      std::string_view ip);

    // For tests + future commits.
    std::vector<User> listUsers();
    bool setDisabled(std::int64_t user_id, bool disabled);

    // Hard-delete: atomic transaction wipes the user row plus rows in
    // sessions, channel_permissions, password_resets keyed on user_id,
    // and NULLs out self-FKs in users.created_by, ldap_config.updated_by,
    // smtp_config.updated_by. auth_audit rows survive (they snapshot the
    // username, no FK on user_id) — purge stays compliant with audit
    // retention. Returns true iff the user row was actually removed.
    // Caller (AuthService) must enforce policy: not self, not last admin.
    bool purgeUser(std::int64_t user_id);

    // Admin update of mutable user fields (commit 7/24). Username is
    // immutable (renames break audit trail). password_hash and
    // must_change_password у admin-CRUD не трогаются — для смены
    // пароля есть updatePasswordHash + resetPassword path.
    bool updateUser(std::int64_t user_id,
                    std::string_view email,
                    Role role,
                    Source source);

    // Used by reset-password flow (commit 7) and bootstrap (commit 9).
    bool setMustChangePassword(std::int64_t user_id, bool value);

    // For commits 9 & 11/24.
    bool clearInitialPasswordExpiry(std::int64_t user_id);

    // commit 11/24 — задать absolute unix-ts срока годности bootstrap/reset
    // пароля. Используется adminCreateUser(must_change=true) и
    // adminResetPassword. Самостоятельная смена пароля юзером (=
    // updatePasswordHash с clear_must_change=true) обнуляет этот столбец.
    bool setInitialPasswordExpiry(std::int64_t user_id, std::int64_t expires_at);

    // Sessions CRUD (commit 5/24).
    //
    // insertSession bumps s.id on success, returns false on UNIQUE jwt_id
    // collision or DB error. findSessionByJwtId / findSessionByRefreshTokenHash
    // return std::nullopt if no match. revokeSessionByJwtId is idempotent —
    // it sets revoked_at on the row but doesn't fail if it's already set
    // (caller checks the returned bool only as a "row existed" signal).
    bool insertSession(Session& s);
    std::optional<Session> findSessionByJwtId(std::string_view jwt_id);
    std::optional<Session> findSessionByRefreshTokenHash(std::string_view hash);
    bool revokeSessionByJwtId(std::string_view jwt_id, std::int64_t when);
    int  revokeAllSessionsForUser(std::int64_t user_id, std::int64_t when);

    // Revoke every active session for `user_id` EXCEPT the one keyed by
    // `keep_jwt_id`. Used by self-change-password flow (commit 8/24): we
    // want to drop other devices but keep the current session usable so
    // the user does not have to re-login on the same browser.
    int  revokeOtherSessionsForUser(std::int64_t user_id,
                                    std::string_view keep_jwt_id,
                                    std::int64_t when);

    // ── fix32 B2: own-sessions API ────────────────────────────────────
    //
    // Возвращает активные (revoked_at IS NULL AND expires_at > now)
    // сессии юзера, отсортированные по last_seen_at DESC NULLS LAST,
    // потом created_at DESC. Используется UI Profile/Sessions page.
    std::vector<Session> listActiveSessionsForUser(std::int64_t user_id,
                                                   std::int64_t now);

    // Идемпотентный revoke сессии, ОТФИЛЬТРОВАННЫЙ по user_id —
    // юзер не может отозвать чужую сессию. Возвращает true если ровно
    // одна строка перешла из non-revoked в revoked (повторный вызов
    // или чужой jwt_id → false).
    bool revokeSessionByJwtIdForUser(std::string_view jwt_id,
                                     std::int64_t user_id,
                                     std::int64_t when);

    // Обновляет sessions.last_seen_at для активной сессии (revoked_at
    // IS NULL). UPDATE по UNIQUE индексу, дёшево. Вызывается на каждом
    // успешном verifyActiveAccess; throttle на 60s выполняется в SQL —
    // запись лишь если предыдущий timestamp старше throttle_sec
    // или NULL. Возвращает true если строка реально была обновлена.
    bool touchSession(std::string_view jwt_id,
                      std::int64_t when,
                      std::int64_t throttle_sec);

    // ── Audit log (commit 12/24) ───────────────────────────────────────
    //
    // Запись событий — append-only; чтения — фильтр + пагинация для
    // GET /api/auth/audit (admin). Retention реализован через purge:
    // оператор/janitor вызывает purgeAuditOlderThan на основе политики.
    bool insertAuditEvent(const AuditEvent& e);
    std::vector<AuditEvent> listAuditEvents(const AuditFilter& f);
    int  purgeAuditOlderThan(std::int64_t cutoff_ts);

    // ── JWT secret encrypted-at-rest (commit 14/24) ────────────────────
    //
    // jwt_secret table — singleton (id=1). Хранит ciphertext, шифрованный
    // MasterKey'ом. Вызвавший передаёт raw bytes ciphertext'а — AuthDb
    // не знает про шифрование.
    //
    // hasJwtSecret(): true если строка с id=1 существует.
    // storeJwtSecret(): UPSERT — первый запуск INSERT, ротация UPDATE.
    // readJwtSecret():  возвращает blob; std::nullopt если нет.
    bool hasJwtSecret();
    bool storeJwtSecret(const std::vector<std::uint8_t>& ciphertext,
                        std::int64_t rotated_at);
    std::optional<std::vector<std::uint8_t>> readJwtSecret();

    // ── Brute-force lockout (commit 13/24) ─────────────────────────────
    //
    // recordFailedLogin: атомарно ставит users.failed_login_count = new_count
    //                    и users.locked_until = locked_until_or_null.
    //                    Передаём абсолютные значения, чтобы политику считал
    //                    AuthService (там удобнее тестировать) и AuthDb не
    //                    знал про экспоненту.
    // clearFailedLogin:  обнуляет count и locked_until. Используется И на
    //                    успешном логине, И при admin-unlock'е — поведение
    //                    одинаково: «учётка снова чистая».
    bool recordFailedLogin(std::int64_t user_id,
                           int new_count,
                           std::optional<std::int64_t> locked_until);
    bool clearFailedLogin(std::int64_t user_id);

    // ── LDAP groups cache (commit 20/24) ───────────────────────────────
    //
    // Кэшируем список групп юзера, как они пришли из директории, плюс
    // unix-sec последнего успешного LDAP-логина. Refresh для LDAP-юзера
    // использует кэш для пересчёта grants без обращения к directory —
    // это даёт нам read-only fallback, если LDAP лежит. Cache TTL
    // живёт в AuthService::LdapCachePolicy (по умолчанию 7 суток).
    //
    // groups_json — нормализованный JSON-array string (см. AuthService).
    // На cache miss на refresh AuthService возвращает LdapCacheExpired.
    bool updateLdapGroupsCache(std::int64_t user_id,
                               std::string_view groups_json,
                               std::int64_t cached_at);

    // ── channel_permissions CRUD (commit 22/24) ────────────────────────
    //
    // Per-user, per-channel ACL для local-юзеров. Используется как
    // GrantsResolver (см. AuthService) — login/refresh складывают grants
    // прямо в JWT claims. ACL для LDAP-юзеров вычисляется отдельно из
    // group_role_map / channel_acl и сюда не пишется.
    //
    // setChannelPermission — UPSERT (PRIMARY KEY (user_id, channel_id)).
    // granted_by — id админа, эмитировавшего изменение; granted_at — момент.
    // removeChannelPermission — DELETE; идемпотентен (ok если row нет).
    // listChannelGrantsForUser — выборка для GrantsResolver: возвращает
    //   только {channel_id, permission}, без audit-полей.
    // listChannelPermissionsForUser/Channel — admin-снимок с audit-полями.
    struct ChannelPermissionRow {
        std::int64_t       user_id{0};
        std::int64_t       channel_id{0};
        ChannelPermission  permission{ChannelPermission::View};
        std::int64_t       granted_at{0};
        std::int64_t       granted_by{0};
        std::string        username;        // joined для admin views
    };

    bool setChannelPermission(std::int64_t user_id,
                              std::int64_t channel_id,
                              ChannelPermission permission,
                              std::int64_t granted_by,
                              std::int64_t granted_at);

    bool removeChannelPermission(std::int64_t user_id,
                                 std::int64_t channel_id);

    int  removeAllChannelPermissionsForUser(std::int64_t user_id);

    std::vector<ChannelGrant>
        listChannelGrantsForUser(std::int64_t user_id);

    std::vector<ChannelPermissionRow>
        listChannelPermissionsForUser(std::int64_t user_id);

    std::vector<ChannelPermissionRow>
        listChannelPermissionsForChannel(std::int64_t channel_id);

    // ── LDAP config row (commit 17/24) ─────────────────────────────────
    //
    // Singleton (id=1). bind_password лежит как BLOB ciphertext, остальные
    // поля — plaintext. Шифрование/расшифровка bind_password — в
    // LdapConfigRepo (там же MasterKey'ом). AuthDb эту семантику не знает.
    //
    // group_role_map_json / channel_acl_json — нормализованные JSON-строки
    // (см. LdapConfigRepo). Пустые строки == «не задано».
    struct LdapConfigRow {
        bool         enabled{false};
        std::string  server;
        std::string  tls_mode;
        std::string  base_dn;
        std::string  bind_dn;
        std::vector<std::uint8_t> bind_password_encrypted;
        std::string  user_filter;
        std::string  group_attribute;
        std::string  group_role_map_json;
        std::string  channel_acl_json;
        bool         recheck_groups_on_refresh{true};
        int          network_timeout_sec{5};
        std::optional<std::int64_t> updated_at;
        std::optional<std::int64_t> updated_by;
    };
    std::optional<LdapConfigRow> readLdapConfigRow();
    bool writeLdapConfigRow(const LdapConfigRow& row);

    // ── System time config row (fix33) ─────────────────────────────────
    //
    // Singleton (id=1). Без секретов — шифрование не нужно.
    struct SystemTimeConfigRow {
        std::string                 source{"system_local"};   // см. TimeSource
        std::string                 server_timezone{"UTC"};
        bool                        ntp_enabled{false};
        std::string                 ntp_servers_json{"[]"};
        int                         ntp_poll_interval_s{60};
        std::optional<std::int64_t> ntp_last_offset_ms;
        std::optional<std::int64_t> ntp_last_sync_at;
        std::int64_t                manual_offset_ms{0};
        std::optional<std::int64_t> manual_set_at;
        std::optional<std::int64_t> updated_at;
    };
    std::optional<SystemTimeConfigRow> readSystemTimeConfigRow();
    bool writeSystemTimeConfigRow(const SystemTimeConfigRow& row);
    // Partial update — только NTP sync результат, без перетирания
    // остального конфига (для фонового поллера).
    bool updateSystemTimeNtpSync(std::int64_t offset_ms,
                                 std::int64_t sync_at_unix_sec);

    // ── SMTP config row (commit 23/24) ─────────────────────────────────
    //
    // Singleton (id=1). password лежит как BLOB ciphertext, остальное —
    // plaintext. Шифрование/расшифровка password — в SmtpConfigRepo
    // (там же MasterKey'ом). AuthDb эту семантику не знает.
    struct SmtpConfigRow {
        bool         enabled{false};
        std::string  server;
        int          port{587};
        std::string  security;     // "none" | "starttls" | "tls"
        std::string  username;
        std::vector<std::uint8_t> password_encrypted;
        std::string  from_email;
        std::string  from_name;
        int          timeout_sec{15};
        std::optional<std::int64_t> updated_at;
        std::optional<std::int64_t> updated_by;
    };
    std::optional<SmtpConfigRow> readSmtpConfigRow();
    bool writeSmtpConfigRow(const SmtpConfigRow& row);

private:
    std::filesystem::path path_;
    std::mutex mutex_;
    sqlite3*   db_{nullptr};

    bool runMigrations();
    bool exec(const char* sql);
};

}  // namespace liveqx::auth
