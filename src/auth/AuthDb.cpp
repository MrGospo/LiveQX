// fix22 commit 2/24 — AuthDb open + migrations + базовые user CRUD
// под нужды последующих шагов (login, bootstrap, RBAC).
//
// Дизайн:
//   - PRAGMA user_version используется как версия схемы.
//   - WAL + synchronous=NORMAL + foreign_keys=ON.
//   - Все мутации под std::mutex (низкая нагрузка auth — не bottleneck).
//   - Любая ошибка при open/migrate → rename файла в .corrupt-<ns>
//     (чтобы chmod-проблемы или partial write не блокировали startup
//     навсегда), AuthDb остаётся в not-ok состоянии до следующего open().
//
// Не реализовано в этом коммите (приходит позже):
//   - channel_permissions CRUD               (commit 22/24)
//   - ldap_config + jwt_secret encryption    (commits 14, 17/24)
//   - auth_audit insert/query                (commit 12/24)
//
// Sessions CRUD добавлен в commit 5/24 (login/logout/refresh).

#include "auth/AuthDb.h"

#include <chrono>
#include <cstring>
#include <system_error>

#include <sqlite3.h>

#include "utils/Log.h"

namespace liveqx::auth {
namespace {

// commit 13/24 — schema v2: brute-force lockout colums.
// commit 20/24 — schema v3: LDAP groups cache колонки.
// Миграция forward-only: ALTER TABLE добавляет столбцы; на fresh install
// (existing<1) сначала создаётся v1-схема без этих колонок, потом v2/v3
// ALTER их добавляют. Это позволяет держать v1-определение неизменным
// (важно для рестора DBs из старых бэкапов).
constexpr int kSchemaVersion = 7;

// Schema v1 — все таблицы из fix22 design-doc, чтобы не дёргать
// миграцию от commit к commit. Поля, реально читаемые сейчас, —
// users.* и базовые индексы. Остальное лежит до своих коммитов.
constexpr const char* kSchemaSqlV1 = R"sql(
PRAGMA foreign_keys = ON;

CREATE TABLE IF NOT EXISTS users (
  id                          INTEGER PRIMARY KEY,
  username                    TEXT UNIQUE NOT NULL,
  email                       TEXT,
  password_hash               TEXT,
  source                      TEXT NOT NULL CHECK (source IN ('local','ldap')),
  role                        TEXT NOT NULL CHECK (role IN ('admin','operator','viewer')),
  must_change_password        INTEGER NOT NULL DEFAULT 0,
  initial_password_expires_at INTEGER,
  password_changed_at         INTEGER,
  last_login_at               INTEGER,
  last_login_ip               TEXT,
  disabled                    INTEGER NOT NULL DEFAULT 0,
  created_at                  INTEGER NOT NULL,
  created_by                  INTEGER REFERENCES users(id)
);

CREATE TABLE IF NOT EXISTS sessions (
  id                  INTEGER PRIMARY KEY,
  user_id             INTEGER NOT NULL REFERENCES users(id),
  jwt_id              TEXT UNIQUE NOT NULL,
  refresh_token_hash  TEXT NOT NULL,
  ip                  TEXT,
  user_agent          TEXT,
  created_at          INTEGER NOT NULL,
  expires_at          INTEGER NOT NULL,
  revoked_at          INTEGER
);

CREATE TABLE IF NOT EXISTS channel_permissions (
  user_id     INTEGER NOT NULL REFERENCES users(id),
  channel_id  INTEGER NOT NULL,
  permission  TEXT NOT NULL CHECK (permission IN ('view','operate')),
  granted_at  INTEGER NOT NULL,
  granted_by  INTEGER REFERENCES users(id),
  PRIMARY KEY (user_id, channel_id)
);

CREATE TABLE IF NOT EXISTS ldap_config (
  id                          INTEGER PRIMARY KEY CHECK (id = 1),
  enabled                     INTEGER NOT NULL DEFAULT 0,
  server                      TEXT,
  tls_mode                    TEXT,
  base_dn                     TEXT,
  bind_dn                     TEXT,
  bind_password_encrypted     BLOB,
  user_filter                 TEXT,
  group_attribute             TEXT,
  group_role_map_json         TEXT,
  channel_acl_json            TEXT,
  recheck_groups_on_refresh   INTEGER NOT NULL DEFAULT 1,
  updated_at                  INTEGER,
  updated_by                  INTEGER REFERENCES users(id)
);

CREATE TABLE IF NOT EXISTS auth_audit (
  id            INTEGER PRIMARY KEY,
  ts            INTEGER NOT NULL,
  event         TEXT NOT NULL,
  user_id       INTEGER,
  username      TEXT,
  ip            TEXT,
  details_json  TEXT
);

CREATE TABLE IF NOT EXISTS password_resets (
  user_id     INTEGER PRIMARY KEY REFERENCES users(id),
  token_hash  TEXT NOT NULL,
  expires_at  INTEGER NOT NULL,
  used_at     INTEGER
);

CREATE TABLE IF NOT EXISTS jwt_secret (
  id                INTEGER PRIMARY KEY CHECK (id = 1),
  secret_encrypted  BLOB NOT NULL,
  rotated_at        INTEGER NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_audit_ts        ON auth_audit(ts);
CREATE INDEX IF NOT EXISTS idx_audit_user      ON auth_audit(user_id);
CREATE INDEX IF NOT EXISTS idx_sessions_user   ON sessions(user_id);
CREATE INDEX IF NOT EXISTS idx_channel_perm_ch ON channel_permissions(channel_id);
)sql";

// Schema v2: добавляет users.failed_login_count и users.locked_until.
// ALTER не идемпотентен (повторный запуск даст «duplicate column») —
// runMigrations гарантирует, что v2-блок выполняется ровно один раз
// при переходе schema_version 1→2.
constexpr const char* kSchemaSqlV2 = R"sql(
ALTER TABLE users ADD COLUMN failed_login_count INTEGER NOT NULL DEFAULT 0;
ALTER TABLE users ADD COLUMN locked_until INTEGER;
)sql";

// Schema v3 (commit 20/24): кэш LDAP-групп для downtime-read-only refresh.
// ldap_groups_json — JSON-массив имён групп (как пришли из директории),
// ldap_groups_cached_at — unix-sec последнего успешного LDAP login'а.
// Refresh для LDAP-юзера читает обе колонки и, если cached_at в пределах
// policy.max_age_sec, пересчитывает grants по cached-группам через
// LdapGrantsResolver. Иначе refresh отдаёт LdapCacheExpired (юзер
// перелогинится). Local-юзеры этими колонками не пользуются — они
// остаются NULL.
constexpr const char* kSchemaSqlV3 = R"sql(
ALTER TABLE users ADD COLUMN ldap_groups_json TEXT;
ALTER TABLE users ADD COLUMN ldap_groups_cached_at INTEGER;
)sql";

// Schema v4 (commit 23/24): smtp_config singleton row для notifications.
// Создаётся отдельным CREATE TABLE IF NOT EXISTS — старые DB (v3) её
// не имели; новые (v4) получают пустую таблицу до первого save() из
// SmtpConfigRepo.
constexpr const char* kSchemaSqlV4 = R"sql(
CREATE TABLE IF NOT EXISTS smtp_config (
  id                  INTEGER PRIMARY KEY CHECK (id = 1),
  enabled             INTEGER NOT NULL DEFAULT 0,
  server              TEXT,
  port                INTEGER NOT NULL DEFAULT 587,
  security            TEXT NOT NULL DEFAULT 'starttls',
  username            TEXT,
  password_encrypted  BLOB,
  from_email          TEXT,
  from_name           TEXT,
  timeout_sec         INTEGER NOT NULL DEFAULT 15,
  updated_at          INTEGER,
  updated_by          INTEGER REFERENCES users(id)
);
)sql";

// Schema v5 (fix32 B2): sessions.last_seen_at для UI Profile/Sessions.
// Обновляется на каждом успешном verifyActiveAccess (см. AuthService).
// NULL для существующих сессий до первого touch'а.
constexpr const char* kSchemaSqlV5 = R"sql(
ALTER TABLE sessions ADD COLUMN last_seen_at INTEGER;
)sql";

// Schema v6: ldap_config.network_timeout_sec — persistent connect/bind
// timeout (см. LdapClient::applyCommonOptions). До v6 поле существовало
// в LdapConfig в памяти, но не писалось в БД: после рестарта значение,
// выставленное оператором через UI, терялось и подменялось default'ом
// 5s, из-за чего /api/auth/ldap/test упирался в httplib read_timeout
// при unreachable host'е. DEFAULT 5 совпадает с in-memory default'ом.
constexpr const char* kSchemaSqlV6 = R"sql(
ALTER TABLE ldap_config ADD COLUMN network_timeout_sec INTEGER NOT NULL DEFAULT 5;
)sql";

// Schema v7 (fix33): system_time_config singleton row для серверной
// настройки времени (источник: system_local / ntp / manual) + IANA TZ.
// Manual.offset_ms применяется liveqx'ом логически — ОС-время
// не меняется (без CAP_SYS_TIME).
constexpr const char* kSchemaSqlV7 = R"sql(
CREATE TABLE IF NOT EXISTS system_time_config (
  id                    INTEGER PRIMARY KEY CHECK (id = 1),
  source                TEXT    NOT NULL DEFAULT 'system_local',
  server_timezone       TEXT    NOT NULL DEFAULT 'UTC',
  ntp_enabled           INTEGER NOT NULL DEFAULT 0,
  ntp_servers_json      TEXT    NOT NULL DEFAULT '[]',
  ntp_poll_interval_s   INTEGER NOT NULL DEFAULT 60,
  ntp_last_offset_ms    INTEGER,
  ntp_last_sync_at      INTEGER,
  manual_offset_ms      INTEGER NOT NULL DEFAULT 0,
  manual_set_at         INTEGER,
  updated_at            INTEGER
);
)sql";

std::int64_t nowUnixSec() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

void renameCorrupt(const std::filesystem::path& p) {
    if (!std::filesystem::exists(p)) return;
    auto target = p;
    target += ".corrupt-" + std::to_string(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    std::error_code ec;
    std::filesystem::rename(p, target, ec);
    if (ec) {
        LOG_ERROR("AuthDb: cannot rename corrupt {} -> {}: {}",
                  p.string(), target.string(), ec.message());
    } else {
        LOG_WARN("AuthDb: corrupt db moved to {}", target.string());
    }
}

bool readUserVersion(sqlite3* db, int& out) {
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, "PRAGMA user_version;", -1, &st, nullptr)
            != SQLITE_OK) return false;
    bool ok = false;
    if (sqlite3_step(st) == SQLITE_ROW) {
        out = sqlite3_column_int(st, 0);
        ok = true;
    }
    sqlite3_finalize(st);
    return ok;
}

std::string colText(sqlite3_stmt* st, int idx) {
    const auto* p = reinterpret_cast<const char*>(sqlite3_column_text(st, idx));
    return p ? std::string(p) : std::string();
}

std::optional<std::int64_t> colInt64Opt(sqlite3_stmt* st, int idx) {
    if (sqlite3_column_type(st, idx) == SQLITE_NULL) return std::nullopt;
    return sqlite3_column_int64(st, idx);
}

User userFromRow(sqlite3_stmt* st) {
    User u;
    u.id            = sqlite3_column_int64(st, 0);
    u.username      = colText(st, 1);
    u.email         = colText(st, 2);
    u.password_hash = colText(st, 3);
    auto src        = roleFromString(colText(st, 5));   // role at 5
    auto src_e      = sourceFromString(colText(st, 4)); // source at 4
    u.source        = src_e.value_or(Source::Local);
    u.role          = src.value_or(Role::Viewer);
    u.must_change_password = sqlite3_column_int(st, 6) != 0;
    u.initial_password_expires_at = colInt64Opt(st, 7);
    u.password_changed_at         = colInt64Opt(st, 8);
    u.last_login_at               = colInt64Opt(st, 9);
    u.last_login_ip               = colText(st, 10);
    u.disabled                    = sqlite3_column_int(st, 11) != 0;
    u.created_at                  = sqlite3_column_int64(st, 12);
    u.created_by                  = colInt64Opt(st, 13);
    // Schema v2 (commit 13/24).
    u.failed_login_count          = sqlite3_column_int(st, 14);
    u.locked_until                = colInt64Opt(st, 15);
    // Schema v3 (commit 20/24).
    u.ldap_groups_json            = colText(st, 16);
    u.ldap_groups_cached_at       = colInt64Opt(st, 17);
    return u;
}

constexpr const char* kUserSelectColumns =
    "id, username, email, password_hash, source, role, "
    "must_change_password, initial_password_expires_at, "
    "password_changed_at, last_login_at, last_login_ip, disabled, "
    "created_at, created_by, failed_login_count, locked_until, "
    "ldap_groups_json, ldap_groups_cached_at";

}  // namespace

AuthDb::AuthDb(std::filesystem::path db_path) : path_(std::move(db_path)) {}

AuthDb::~AuthDb() {
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

bool AuthDb::exec(const char* sql) {
    char* err = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        LOG_ERROR("AuthDb: exec failed: {} ({})",
                  err ? err : "(null)", sqlite3_errmsg(db_));
        sqlite3_free(err);
        return false;
    }
    return true;
}

bool AuthDb::runMigrations() {
    int existing = 0;
    if (!readUserVersion(db_, existing)) {
        LOG_ERROR("AuthDb: cannot read user_version: {}", sqlite3_errmsg(db_));
        return false;
    }
    if (existing > kSchemaVersion) {
        LOG_ERROR("AuthDb: schema v{} newer than supported v{} — refusing {}",
                  existing, kSchemaVersion, path_.string());
        return false;
    }

    if (!exec("PRAGMA journal_mode=WAL;") ||
        !exec("PRAGMA synchronous=NORMAL;") ||
        !exec("PRAGMA foreign_keys=ON;")) {
        return false;
    }

    if (existing < 1) {
        if (!exec(kSchemaSqlV1)) return false;
    }
    if (existing < 2) {
        if (!exec(kSchemaSqlV2)) return false;
    }
    if (existing < 3) {
        if (!exec(kSchemaSqlV3)) return false;
    }
    if (existing < 4) {
        if (!exec(kSchemaSqlV4)) return false;
    }
    if (existing < 5) {
        if (!exec(kSchemaSqlV5)) return false;
    }
    if (existing < 6) {
        if (!exec(kSchemaSqlV6)) return false;
    }
    if (existing < 7) {
        if (!exec(kSchemaSqlV7)) return false;
    }

    if (existing < kSchemaVersion) {
        const std::string set_pragma =
            "PRAGMA user_version=" + std::to_string(kSchemaVersion) + ";";
        if (!exec(set_pragma.c_str())) return false;
    }
    return true;
}

bool AuthDb::open() {
    std::lock_guard<std::mutex> lk(mutex_);
    if (db_) return true;

    std::error_code ec;
    if (path_.has_parent_path())
        std::filesystem::create_directories(path_.parent_path(), ec);

    if (sqlite3_open(path_.string().c_str(), &db_) != SQLITE_OK) {
        LOG_ERROR("AuthDb: open {} failed: {}",
                  path_.string(), db_ ? sqlite3_errmsg(db_) : "(no db)");
        if (db_) { sqlite3_close(db_); db_ = nullptr; }
        renameCorrupt(path_);
        return false;
    }

    if (!runMigrations()) {
        sqlite3_close(db_);
        db_ = nullptr;
        renameCorrupt(path_);
        return false;
    }
    LOG_INFO("AuthDb opened at {}", path_.string());
    return true;
}

bool AuthDb::hasAdminUser() {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!db_) return false;

    constexpr const char* kSql =
        "SELECT 1 FROM users WHERE role='admin' AND disabled=0 LIMIT 1";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) {
        LOG_ERROR("AuthDb::hasAdminUser prepare failed: {}", sqlite3_errmsg(db_));
        return false;
    }
    const bool any = sqlite3_step(st) == SQLITE_ROW;
    sqlite3_finalize(st);
    return any;
}

std::optional<std::int64_t> AuthDb::insertUser(const User& u) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!db_) return std::nullopt;

    constexpr const char* kSql =
        "INSERT INTO users("
        "  username, email, password_hash, source, role, "
        "  must_change_password, initial_password_expires_at, "
        "  password_changed_at, last_login_at, last_login_ip, "
        "  disabled, created_at, created_by) "
        "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";

    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) {
        LOG_ERROR("AuthDb::insertUser prepare failed: {}", sqlite3_errmsg(db_));
        return std::nullopt;
    }

    int i = 1;
    sqlite3_bind_text (st, i++, u.username.c_str(), -1, SQLITE_TRANSIENT);
    if (u.email.empty()) sqlite3_bind_null(st, i++);
    else sqlite3_bind_text(st, i++, u.email.c_str(), -1, SQLITE_TRANSIENT);
    if (u.password_hash.empty()) sqlite3_bind_null(st, i++);
    else sqlite3_bind_text(st, i++, u.password_hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (st, i++, sourceName(u.source), -1, SQLITE_STATIC);
    sqlite3_bind_text (st, i++, roleName(u.role),     -1, SQLITE_STATIC);
    sqlite3_bind_int  (st, i++, u.must_change_password ? 1 : 0);
    if (u.initial_password_expires_at)
        sqlite3_bind_int64(st, i++, *u.initial_password_expires_at);
    else sqlite3_bind_null(st, i++);
    if (u.password_changed_at) sqlite3_bind_int64(st, i++, *u.password_changed_at);
    else sqlite3_bind_null(st, i++);
    if (u.last_login_at) sqlite3_bind_int64(st, i++, *u.last_login_at);
    else sqlite3_bind_null(st, i++);
    if (u.last_login_ip.empty()) sqlite3_bind_null(st, i++);
    else sqlite3_bind_text(st, i++, u.last_login_ip.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int  (st, i++, u.disabled ? 1 : 0);
    sqlite3_bind_int64(st, i++, u.created_at ? u.created_at : nowUnixSec());
    if (u.created_by) sqlite3_bind_int64(st, i++, *u.created_by);
    else sqlite3_bind_null(st, i++);

    const int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        // UNIQUE-violation на username — типичный кейс при попытке создать
        // пользователя который уже существует. Лог уровня debug, не error.
        LOG_DEBUG("AuthDb::insertUser step rc={}: {}", rc, sqlite3_errmsg(db_));
        return std::nullopt;
    }
    return sqlite3_last_insert_rowid(db_);
}

std::optional<User> AuthDb::findUserByUsername(std::string_view username) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!db_) return std::nullopt;

    const std::string sql =
        std::string("SELECT ") + kUserSelectColumns +
        " FROM users WHERE username=? LIMIT 1";

    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
        LOG_ERROR("AuthDb::findUserByUsername prepare failed: {}", sqlite3_errmsg(db_));
        return std::nullopt;
    }
    sqlite3_bind_text(st, 1, username.data(),
                      static_cast<int>(username.size()), SQLITE_TRANSIENT);

    std::optional<User> result;
    if (sqlite3_step(st) == SQLITE_ROW) result = userFromRow(st);
    sqlite3_finalize(st);
    return result;
}

std::optional<User> AuthDb::findUserById(std::int64_t id) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!db_) return std::nullopt;

    const std::string sql =
        std::string("SELECT ") + kUserSelectColumns +
        " FROM users WHERE id=? LIMIT 1";

    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_int64(st, 1, id);

    std::optional<User> result;
    if (sqlite3_step(st) == SQLITE_ROW) result = userFromRow(st);
    sqlite3_finalize(st);
    return result;
}

bool AuthDb::updatePasswordHash(std::int64_t user_id,
                                std::string_view new_hash,
                                std::int64_t password_changed_at,
                                bool clear_must_change) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!db_) return false;

    // Если caller просит снять must_change_password — параллельно обнуляем
    // initial_password_expires_at: TTL имеет смысл только пока пароль —
    // bootstrap/reset временный. После самостоятельной смены пароля юзером
    // окно закрывается само собой и больше никогда не возвращается.
    constexpr const char* kSql =
        "UPDATE users SET "
        "  password_hash = ?, "
        "  password_changed_at = ?, "
        "  must_change_password = CASE WHEN ?=1 THEN 0 ELSE must_change_password END, "
        "  initial_password_expires_at = "
        "    CASE WHEN ?=1 THEN NULL ELSE initial_password_expires_at END "
        "WHERE id=?";

    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text (st, 1, new_hash.data(),
                       static_cast<int>(new_hash.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, password_changed_at);
    sqlite3_bind_int  (st, 3, clear_must_change ? 1 : 0);
    sqlite3_bind_int  (st, 4, clear_must_change ? 1 : 0);
    sqlite3_bind_int64(st, 5, user_id);

    const int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE && sqlite3_changes(db_) == 1;
}

bool AuthDb::setLastLogin(std::int64_t user_id,
                          std::int64_t when,
                          std::string_view ip) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!db_) return false;

    constexpr const char* kSql =
        "UPDATE users SET last_login_at=?, last_login_ip=? WHERE id=?";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_int64(st, 1, when);
    sqlite3_bind_text (st, 2, ip.data(),
                       static_cast<int>(ip.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 3, user_id);

    const int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE;
}

std::vector<User> AuthDb::listUsers() {
    std::lock_guard<std::mutex> lk(mutex_);
    std::vector<User> out;
    if (!db_) return out;

    const std::string sql =
        std::string("SELECT ") + kUserSelectColumns +
        " FROM users ORDER BY id";

    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
        return out;
    }
    while (sqlite3_step(st) == SQLITE_ROW) {
        out.push_back(userFromRow(st));
    }
    sqlite3_finalize(st);
    return out;
}

int AuthDb::countEnabledAdmins() {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!db_) return 0;
    constexpr const char* kSql =
        "SELECT COUNT(*) FROM users WHERE role='admin' AND disabled=0";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) {
        LOG_ERROR("AuthDb::countEnabledAdmins prepare failed: {}", sqlite3_errmsg(db_));
        return 0;
    }
    int n = 0;
    if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return n;
}

bool AuthDb::purgeUser(std::int64_t user_id) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!db_) return false;

    // Single transaction: child rows + self-FK NULLs + parent row.
    // Полагаемся на foreign_keys=ON в open() — при FK-violation любой
    // step() падает и весь BEGIN откатывается ROLLBACK'ом ниже.
    if (sqlite3_exec(db_, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) != SQLITE_OK) {
        LOG_ERROR("AuthDb::purgeUser BEGIN failed: {}", sqlite3_errmsg(db_));
        return false;
    }

    auto runBind = [&](const char* sql) -> bool {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
            LOG_ERROR("AuthDb::purgeUser prepare '{}' failed: {}", sql, sqlite3_errmsg(db_));
            return false;
        }
        sqlite3_bind_int64(st, 1, user_id);
        const int rc = sqlite3_step(st);
        sqlite3_finalize(st);
        if (rc != SQLITE_DONE) {
            LOG_ERROR("AuthDb::purgeUser step '{}' rc={} err={}",
                      sql, rc, sqlite3_errmsg(db_));
            return false;
        }
        return true;
    };

    const bool ok =
        runBind("DELETE FROM sessions             WHERE user_id    = ?")  &&
        runBind("DELETE FROM channel_permissions  WHERE user_id    = ?")  &&
        runBind("DELETE FROM password_resets      WHERE user_id    = ?")  &&
        runBind("UPDATE users        SET created_by = NULL WHERE created_by = ?") &&
        runBind("UPDATE ldap_config  SET updated_by = NULL WHERE updated_by = ?") &&
        runBind("UPDATE smtp_config  SET updated_by = NULL WHERE updated_by = ?") &&
        runBind("DELETE FROM users               WHERE id         = ?");

    if (!ok) {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        return false;
    }
    // Финальный DELETE FROM users должен был выкинуть ровно одну строку;
    // если 0 — юзера не было, считаем purge неуспешным (caller отдаст 404).
    const int last_changes = sqlite3_changes(db_);
    if (last_changes != 1) {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        return false;
    }
    if (sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK) {
        LOG_ERROR("AuthDb::purgeUser COMMIT failed: {}", sqlite3_errmsg(db_));
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        return false;
    }
    return true;
}

bool AuthDb::setDisabled(std::int64_t user_id, bool disabled) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!db_) return false;
    constexpr const char* kSql = "UPDATE users SET disabled=? WHERE id=?";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int  (st, 1, disabled ? 1 : 0);
    sqlite3_bind_int64(st, 2, user_id);
    const int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE && sqlite3_changes(db_) == 1;
}

namespace {

constexpr const char* kSessionSelectColumns =
    "id, user_id, jwt_id, refresh_token_hash, ip, user_agent, "
    "created_at, expires_at, revoked_at, last_seen_at";

Session sessionFromRow(sqlite3_stmt* st) {
    Session s;
    s.id                 = sqlite3_column_int64(st, 0);
    s.user_id            = sqlite3_column_int64(st, 1);
    s.jwt_id             = colText(st, 2);
    s.refresh_token_hash = colText(st, 3);
    s.ip                 = colText(st, 4);
    s.user_agent         = colText(st, 5);
    s.created_at         = sqlite3_column_int64(st, 6);
    s.expires_at         = sqlite3_column_int64(st, 7);
    s.revoked_at         = colInt64Opt(st, 8);
    s.last_seen_at       = colInt64Opt(st, 9);
    return s;
}

}  // namespace

bool AuthDb::insertSession(Session& s) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!db_) return false;

    constexpr const char* kSql =
        "INSERT INTO sessions("
        "  user_id, jwt_id, refresh_token_hash, ip, user_agent, "
        "  created_at, expires_at, revoked_at) "
        "VALUES(?, ?, ?, ?, ?, ?, ?, ?)";

    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) {
        LOG_ERROR("AuthDb::insertSession prepare failed: {}", sqlite3_errmsg(db_));
        return false;
    }
    int i = 1;
    sqlite3_bind_int64(st, i++, s.user_id);
    sqlite3_bind_text (st, i++, s.jwt_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (st, i++, s.refresh_token_hash.c_str(), -1, SQLITE_TRANSIENT);
    if (s.ip.empty()) sqlite3_bind_null(st, i++);
    else sqlite3_bind_text(st, i++, s.ip.c_str(), -1, SQLITE_TRANSIENT);
    if (s.user_agent.empty()) sqlite3_bind_null(st, i++);
    else sqlite3_bind_text(st, i++, s.user_agent.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, i++, s.created_at ? s.created_at : nowUnixSec());
    sqlite3_bind_int64(st, i++, s.expires_at);
    if (s.revoked_at) sqlite3_bind_int64(st, i++, *s.revoked_at);
    else sqlite3_bind_null(st, i++);

    const int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        LOG_DEBUG("AuthDb::insertSession step rc={}: {}", rc, sqlite3_errmsg(db_));
        return false;
    }
    s.id = sqlite3_last_insert_rowid(db_);
    return true;
}

std::optional<Session> AuthDb::findSessionByJwtId(std::string_view jwt_id) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!db_) return std::nullopt;

    const std::string sql =
        std::string("SELECT ") + kSessionSelectColumns +
        " FROM sessions WHERE jwt_id=? LIMIT 1";

    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_text(st, 1, jwt_id.data(),
                      static_cast<int>(jwt_id.size()), SQLITE_TRANSIENT);

    std::optional<Session> result;
    if (sqlite3_step(st) == SQLITE_ROW) result = sessionFromRow(st);
    sqlite3_finalize(st);
    return result;
}

std::optional<Session> AuthDb::findSessionByRefreshTokenHash(std::string_view hash) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!db_) return std::nullopt;

    const std::string sql =
        std::string("SELECT ") + kSessionSelectColumns +
        " FROM sessions WHERE refresh_token_hash=? LIMIT 1";

    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_text(st, 1, hash.data(),
                      static_cast<int>(hash.size()), SQLITE_TRANSIENT);

    std::optional<Session> result;
    if (sqlite3_step(st) == SQLITE_ROW) result = sessionFromRow(st);
    sqlite3_finalize(st);
    return result;
}

bool AuthDb::revokeSessionByJwtId(std::string_view jwt_id, std::int64_t when) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!db_) return false;

    // COALESCE: оставляем самый ранний revoked_at — повторный logout
    // не сдвигает timestamp вперёд (форенсика чище).
    constexpr const char* kSql =
        "UPDATE sessions SET revoked_at=COALESCE(revoked_at, ?) WHERE jwt_id=?";

    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(st, 1, when);
    sqlite3_bind_text (st, 2, jwt_id.data(),
                       static_cast<int>(jwt_id.size()), SQLITE_TRANSIENT);
    const int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE && sqlite3_changes(db_) == 1;
}

int AuthDb::revokeAllSessionsForUser(std::int64_t user_id, std::int64_t when) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!db_) return 0;

    constexpr const char* kSql =
        "UPDATE sessions SET revoked_at=COALESCE(revoked_at, ?) "
        "WHERE user_id=? AND revoked_at IS NULL";

    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) return 0;
    sqlite3_bind_int64(st, 1, when);
    sqlite3_bind_int64(st, 2, user_id);
    const int rc = sqlite3_step(st);
    const int n  = sqlite3_changes(db_);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? n : 0;
}

int AuthDb::revokeOtherSessionsForUser(std::int64_t user_id,
                                       std::string_view keep_jwt_id,
                                       std::int64_t when) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!db_) return 0;

    constexpr const char* kSql =
        "UPDATE sessions SET revoked_at=COALESCE(revoked_at, ?) "
        "WHERE user_id=? AND jwt_id<>? AND revoked_at IS NULL";

    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) return 0;
    sqlite3_bind_int64(st, 1, when);
    sqlite3_bind_int64(st, 2, user_id);
    sqlite3_bind_text(st, 3, keep_jwt_id.data(),
                      static_cast<int>(keep_jwt_id.size()), SQLITE_TRANSIENT);
    const int rc = sqlite3_step(st);
    const int n  = sqlite3_changes(db_);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? n : 0;
}

// ── fix32 B2: own-sessions API ────────────────────────────────────────

std::vector<Session>
AuthDb::listActiveSessionsForUser(std::int64_t user_id, std::int64_t now) {
    std::lock_guard<std::mutex> lk(mutex_);
    std::vector<Session> out;
    if (!db_) return out;

    const std::string sql =
        std::string("SELECT ") + kSessionSelectColumns +
        " FROM sessions "
        " WHERE user_id=? AND revoked_at IS NULL AND expires_at>? "
        " ORDER BY (last_seen_at IS NULL), last_seen_at DESC, created_at DESC";

    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
        return out;
    }
    sqlite3_bind_int64(st, 1, user_id);
    sqlite3_bind_int64(st, 2, now);

    while (sqlite3_step(st) == SQLITE_ROW) out.push_back(sessionFromRow(st));
    sqlite3_finalize(st);
    return out;
}

bool AuthDb::revokeSessionByJwtIdForUser(std::string_view jwt_id,
                                         std::int64_t user_id,
                                         std::int64_t when) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!db_) return false;

    constexpr const char* kSql =
        "UPDATE sessions SET revoked_at=? "
        "WHERE jwt_id=? AND user_id=? AND revoked_at IS NULL";

    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(st, 1, when);
    sqlite3_bind_text (st, 2, jwt_id.data(),
                       static_cast<int>(jwt_id.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 3, user_id);
    const int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE && sqlite3_changes(db_) == 1;
}

bool AuthDb::touchSession(std::string_view jwt_id,
                          std::int64_t when,
                          std::int64_t throttle_sec) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!db_) return false;

    constexpr const char* kSql =
        "UPDATE sessions SET last_seen_at=? "
        "WHERE jwt_id=? AND revoked_at IS NULL "
        "  AND (last_seen_at IS NULL OR last_seen_at <= ?)";

    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(st, 1, when);
    sqlite3_bind_text (st, 2, jwt_id.data(),
                       static_cast<int>(jwt_id.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 3, when - throttle_sec);
    const int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE && sqlite3_changes(db_) == 1;
}

bool AuthDb::updateUser(std::int64_t user_id,
                        std::string_view email,
                        Role role,
                        Source source) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!db_) return false;

    constexpr const char* kSql =
        "UPDATE users SET email=?, role=?, source=? WHERE id=?";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) return false;
    if (email.empty()) sqlite3_bind_null(st, 1);
    else sqlite3_bind_text(st, 1, email.data(),
                            static_cast<int>(email.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text (st, 2, roleName(role),     -1, SQLITE_STATIC);
    sqlite3_bind_text (st, 3, sourceName(source), -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 4, user_id);
    const int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE && sqlite3_changes(db_) == 1;
}

bool AuthDb::setMustChangePassword(std::int64_t user_id, bool value) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!db_) return false;
    constexpr const char* kSql =
        "UPDATE users SET must_change_password=? WHERE id=?";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int  (st, 1, value ? 1 : 0);
    sqlite3_bind_int64(st, 2, user_id);
    const int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE && sqlite3_changes(db_) == 1;
}

bool AuthDb::clearInitialPasswordExpiry(std::int64_t user_id) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!db_) return false;
    constexpr const char* kSql =
        "UPDATE users SET initial_password_expires_at=NULL WHERE id=?";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(st, 1, user_id);
    const int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE;
}

bool AuthDb::setInitialPasswordExpiry(std::int64_t user_id,
                                      std::int64_t expires_at) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!db_) return false;
    constexpr const char* kSql =
        "UPDATE users SET initial_password_expires_at=? WHERE id=?";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(st, 1, expires_at);
    sqlite3_bind_int64(st, 2, user_id);
    const int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE && sqlite3_changes(db_) == 1;
}

// ── JWT secret encrypted-at-rest (commit 14/24) ───────────────────────

bool AuthDb::hasJwtSecret() {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!db_) return false;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT 1 FROM jwt_secret WHERE id=1",
                           -1, &st, nullptr) != SQLITE_OK) {
        return false;
    }
    const bool any = sqlite3_step(st) == SQLITE_ROW;
    sqlite3_finalize(st);
    return any;
}

bool AuthDb::storeJwtSecret(const std::vector<std::uint8_t>& ciphertext,
                            std::int64_t rotated_at) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!db_) return false;
    if (ciphertext.empty()) return false;
    constexpr const char* kSql =
        "INSERT INTO jwt_secret(id, secret_encrypted, rotated_at) "
        "VALUES(1, ?, ?) "
        "ON CONFLICT(id) DO UPDATE SET "
        "  secret_encrypted=excluded.secret_encrypted, "
        "  rotated_at=excluded.rotated_at";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) {
        LOG_ERROR("AuthDb::storeJwtSecret prepare failed: {}", sqlite3_errmsg(db_));
        return false;
    }
    sqlite3_bind_blob (st, 1, ciphertext.data(),
                       static_cast<int>(ciphertext.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, rotated_at);
    const int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE;
}

std::optional<std::vector<std::uint8_t>> AuthDb::readJwtSecret() {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!db_) return std::nullopt;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_,
            "SELECT secret_encrypted FROM jwt_secret WHERE id=1",
            -1, &st, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    std::optional<std::vector<std::uint8_t>> out;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const auto* p = static_cast<const std::uint8_t*>(sqlite3_column_blob(st, 0));
        const int n = sqlite3_column_bytes(st, 0);
        if (p && n > 0) out = std::vector<std::uint8_t>(p, p + n);
    }
    sqlite3_finalize(st);
    return out;
}

// ── Brute-force lockout (commit 13/24) ────────────────────────────────

bool AuthDb::recordFailedLogin(std::int64_t user_id,
                               int new_count,
                               std::optional<std::int64_t> locked_until) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!db_) return false;
    constexpr const char* kSql =
        "UPDATE users SET failed_login_count=?, locked_until=? WHERE id=?";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int  (st, 1, new_count);
    if (locked_until) sqlite3_bind_int64(st, 2, *locked_until);
    else              sqlite3_bind_null (st, 2);
    sqlite3_bind_int64(st, 3, user_id);
    const int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE && sqlite3_changes(db_) == 1;
}

bool AuthDb::clearFailedLogin(std::int64_t user_id) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!db_) return false;
    constexpr const char* kSql =
        "UPDATE users SET failed_login_count=0, locked_until=NULL WHERE id=?";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(st, 1, user_id);
    const int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE;
}

// ── LDAP groups cache (commit 20/24) ──────────────────────────────────

bool AuthDb::updateLdapGroupsCache(std::int64_t user_id,
                                   std::string_view groups_json,
                                   std::int64_t cached_at) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!db_) return false;
    constexpr const char* kSql =
        "UPDATE users SET ldap_groups_json=?, ldap_groups_cached_at=? "
        "WHERE id=?";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text (st, 1, groups_json.data(),
                       static_cast<int>(groups_json.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, cached_at);
    sqlite3_bind_int64(st, 3, user_id);
    const int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE && sqlite3_changes(db_) == 1;
}

// ── LDAP config row (commit 17/24) ────────────────────────────────────

std::optional<AuthDb::LdapConfigRow> AuthDb::readLdapConfigRow() {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!db_) return std::nullopt;
    constexpr const char* kSql =
        "SELECT enabled, server, tls_mode, base_dn, bind_dn, "
        "       bind_password_encrypted, user_filter, group_attribute, "
        "       group_role_map_json, channel_acl_json, "
        "       recheck_groups_on_refresh, updated_at, updated_by, "
        "       network_timeout_sec "
        "FROM ldap_config WHERE id=1";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) {
        LOG_ERROR("AuthDb::readLdapConfigRow prepare failed: {}",
                  sqlite3_errmsg(db_));
        return std::nullopt;
    }
    std::optional<LdapConfigRow> out;
    if (sqlite3_step(st) == SQLITE_ROW) {
        LdapConfigRow row;
        row.enabled = sqlite3_column_int(st, 0) != 0;
        auto col_text = [&](int i) -> std::string {
            const auto* s = reinterpret_cast<const char*>(sqlite3_column_text(st, i));
            return s ? std::string(s) : std::string();
        };
        row.server          = col_text(1);
        row.tls_mode        = col_text(2);
        row.base_dn         = col_text(3);
        row.bind_dn         = col_text(4);
        const auto* blob = static_cast<const std::uint8_t*>(
            sqlite3_column_blob(st, 5));
        const int blob_n = sqlite3_column_bytes(st, 5);
        if (blob && blob_n > 0) {
            row.bind_password_encrypted.assign(blob, blob + blob_n);
        }
        row.user_filter         = col_text(6);
        row.group_attribute     = col_text(7);
        row.group_role_map_json = col_text(8);
        row.channel_acl_json    = col_text(9);
        row.recheck_groups_on_refresh = sqlite3_column_int(st, 10) != 0;
        if (sqlite3_column_type(st, 11) != SQLITE_NULL)
            row.updated_at = sqlite3_column_int64(st, 11);
        if (sqlite3_column_type(st, 12) != SQLITE_NULL)
            row.updated_by = sqlite3_column_int64(st, 12);
        // Schema v6: NOT NULL DEFAULT 5, но на rebuilt-from-old-row
        // sqlite может вернуть NULL до первой записи — страхуемся.
        if (sqlite3_column_type(st, 13) != SQLITE_NULL)
            row.network_timeout_sec = sqlite3_column_int(st, 13);
        out = std::move(row);
    }
    sqlite3_finalize(st);
    return out;
}

bool AuthDb::writeLdapConfigRow(const LdapConfigRow& row) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!db_) return false;
    // UPSERT в singleton-row id=1.
    constexpr const char* kSql =
        "INSERT INTO ldap_config("
        "  id, enabled, server, tls_mode, base_dn, bind_dn, "
        "  bind_password_encrypted, user_filter, group_attribute, "
        "  group_role_map_json, channel_acl_json, "
        "  recheck_groups_on_refresh, updated_at, updated_by, "
        "  network_timeout_sec) "
        "VALUES(1, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(id) DO UPDATE SET "
        "  enabled=excluded.enabled, server=excluded.server, "
        "  tls_mode=excluded.tls_mode, base_dn=excluded.base_dn, "
        "  bind_dn=excluded.bind_dn, "
        "  bind_password_encrypted=excluded.bind_password_encrypted, "
        "  user_filter=excluded.user_filter, "
        "  group_attribute=excluded.group_attribute, "
        "  group_role_map_json=excluded.group_role_map_json, "
        "  channel_acl_json=excluded.channel_acl_json, "
        "  recheck_groups_on_refresh=excluded.recheck_groups_on_refresh, "
        "  updated_at=excluded.updated_at, updated_by=excluded.updated_by, "
        "  network_timeout_sec=excluded.network_timeout_sec";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) {
        LOG_ERROR("AuthDb::writeLdapConfigRow prepare failed: {}",
                  sqlite3_errmsg(db_));
        return false;
    }
    sqlite3_bind_int (st,  1, row.enabled ? 1 : 0);
    sqlite3_bind_text(st,  2, row.server.c_str(),
                      static_cast<int>(row.server.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(st,  3, row.tls_mode.c_str(),
                      static_cast<int>(row.tls_mode.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(st,  4, row.base_dn.c_str(),
                      static_cast<int>(row.base_dn.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(st,  5, row.bind_dn.c_str(),
                      static_cast<int>(row.bind_dn.size()), SQLITE_TRANSIENT);
    if (row.bind_password_encrypted.empty()) {
        sqlite3_bind_null(st, 6);
    } else {
        sqlite3_bind_blob(st, 6, row.bind_password_encrypted.data(),
                          static_cast<int>(row.bind_password_encrypted.size()),
                          SQLITE_TRANSIENT);
    }
    sqlite3_bind_text(st,  7, row.user_filter.c_str(),
                      static_cast<int>(row.user_filter.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(st,  8, row.group_attribute.c_str(),
                      static_cast<int>(row.group_attribute.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(st,  9, row.group_role_map_json.c_str(),
                      static_cast<int>(row.group_role_map_json.size()),
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 10, row.channel_acl_json.c_str(),
                      static_cast<int>(row.channel_acl_json.size()),
                      SQLITE_TRANSIENT);
    sqlite3_bind_int (st, 11, row.recheck_groups_on_refresh ? 1 : 0);
    if (row.updated_at.has_value())
        sqlite3_bind_int64(st, 12, *row.updated_at);
    else
        sqlite3_bind_null(st, 12);
    if (row.updated_by.has_value())
        sqlite3_bind_int64(st, 13, *row.updated_by);
    else
        sqlite3_bind_null(st, 13);
    sqlite3_bind_int(st, 14, row.network_timeout_sec);
    const int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        LOG_ERROR("AuthDb::writeLdapConfigRow step failed: {}",
                  sqlite3_errmsg(db_));
        return false;
    }
    return true;
}

// ── System time config row (fix33) ─────────────────────────────────────

std::optional<AuthDb::SystemTimeConfigRow> AuthDb::readSystemTimeConfigRow() {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!db_) return std::nullopt;
    constexpr const char* kSql =
        "SELECT source, server_timezone, ntp_enabled, ntp_servers_json, "
        "       ntp_poll_interval_s, ntp_last_offset_ms, ntp_last_sync_at, "
        "       manual_offset_ms, manual_set_at, updated_at "
        "FROM system_time_config WHERE id=1";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) {
        LOG_ERROR("AuthDb::readSystemTimeConfigRow prepare failed: {}",
                  sqlite3_errmsg(db_));
        return std::nullopt;
    }
    std::optional<SystemTimeConfigRow> out;
    if (sqlite3_step(st) == SQLITE_ROW) {
        SystemTimeConfigRow row;
        auto col_text = [&](int i) -> std::string {
            const auto* s = reinterpret_cast<const char*>(sqlite3_column_text(st, i));
            return s ? std::string(s) : std::string();
        };
        row.source              = col_text(0);
        row.server_timezone     = col_text(1);
        row.ntp_enabled         = sqlite3_column_int(st, 2) != 0;
        row.ntp_servers_json    = col_text(3);
        row.ntp_poll_interval_s = sqlite3_column_int(st, 4);
        if (sqlite3_column_type(st, 5) != SQLITE_NULL)
            row.ntp_last_offset_ms = sqlite3_column_int64(st, 5);
        if (sqlite3_column_type(st, 6) != SQLITE_NULL)
            row.ntp_last_sync_at = sqlite3_column_int64(st, 6);
        row.manual_offset_ms    = sqlite3_column_int64(st, 7);
        if (sqlite3_column_type(st, 8) != SQLITE_NULL)
            row.manual_set_at = sqlite3_column_int64(st, 8);
        if (sqlite3_column_type(st, 9) != SQLITE_NULL)
            row.updated_at = sqlite3_column_int64(st, 9);
        out = std::move(row);
    }
    sqlite3_finalize(st);
    return out;
}

bool AuthDb::writeSystemTimeConfigRow(const SystemTimeConfigRow& row) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!db_) return false;
    constexpr const char* kSql =
        "INSERT INTO system_time_config("
        "  id, source, server_timezone, ntp_enabled, ntp_servers_json, "
        "  ntp_poll_interval_s, ntp_last_offset_ms, ntp_last_sync_at, "
        "  manual_offset_ms, manual_set_at, updated_at) "
        "VALUES(1, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(id) DO UPDATE SET "
        "  source=excluded.source, "
        "  server_timezone=excluded.server_timezone, "
        "  ntp_enabled=excluded.ntp_enabled, "
        "  ntp_servers_json=excluded.ntp_servers_json, "
        "  ntp_poll_interval_s=excluded.ntp_poll_interval_s, "
        "  ntp_last_offset_ms=excluded.ntp_last_offset_ms, "
        "  ntp_last_sync_at=excluded.ntp_last_sync_at, "
        "  manual_offset_ms=excluded.manual_offset_ms, "
        "  manual_set_at=excluded.manual_set_at, "
        "  updated_at=excluded.updated_at";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) {
        LOG_ERROR("AuthDb::writeSystemTimeConfigRow prepare failed: {}",
                  sqlite3_errmsg(db_));
        return false;
    }
    sqlite3_bind_text(st, 1, row.source.c_str(),
                      static_cast<int>(row.source.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, row.server_timezone.c_str(),
                      static_cast<int>(row.server_timezone.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int (st, 3, row.ntp_enabled ? 1 : 0);
    sqlite3_bind_text(st, 4, row.ntp_servers_json.c_str(),
                      static_cast<int>(row.ntp_servers_json.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int (st, 5, row.ntp_poll_interval_s);
    if (row.ntp_last_offset_ms.has_value())
        sqlite3_bind_int64(st, 6, *row.ntp_last_offset_ms);
    else
        sqlite3_bind_null(st, 6);
    if (row.ntp_last_sync_at.has_value())
        sqlite3_bind_int64(st, 7, *row.ntp_last_sync_at);
    else
        sqlite3_bind_null(st, 7);
    sqlite3_bind_int64(st, 8, row.manual_offset_ms);
    if (row.manual_set_at.has_value())
        sqlite3_bind_int64(st, 9, *row.manual_set_at);
    else
        sqlite3_bind_null(st, 9);
    if (row.updated_at.has_value())
        sqlite3_bind_int64(st, 10, *row.updated_at);
    else
        sqlite3_bind_null(st, 10);
    const int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        LOG_ERROR("AuthDb::writeSystemTimeConfigRow step failed: {}",
                  sqlite3_errmsg(db_));
        return false;
    }
    return true;
}

bool AuthDb::updateSystemTimeNtpSync(std::int64_t offset_ms,
                                     std::int64_t sync_at_unix_sec) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!db_) return false;
    // Если row ещё не создан админ-правкой, поллеру нечего обновлять —
    // выходим без ошибки (без него source точно не Ntp).
    constexpr const char* kSql =
        "UPDATE system_time_config "
        "   SET ntp_last_offset_ms=?, ntp_last_sync_at=? "
        " WHERE id=1";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) {
        LOG_ERROR("AuthDb::updateSystemTimeNtpSync prepare failed: {}",
                  sqlite3_errmsg(db_));
        return false;
    }
    sqlite3_bind_int64(st, 1, offset_ms);
    sqlite3_bind_int64(st, 2, sync_at_unix_sec);
    const int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        LOG_ERROR("AuthDb::updateSystemTimeNtpSync step failed: {}",
                  sqlite3_errmsg(db_));
        return false;
    }
    return true;
}

// ── SMTP config row (commit 23/24) ─────────────────────────────────────

std::optional<AuthDb::SmtpConfigRow> AuthDb::readSmtpConfigRow() {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!db_) return std::nullopt;
    constexpr const char* kSql =
        "SELECT enabled, server, port, security, username, "
        "       password_encrypted, from_email, from_name, timeout_sec, "
        "       updated_at, updated_by "
        "FROM smtp_config WHERE id=1";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) {
        LOG_ERROR("AuthDb::readSmtpConfigRow prepare failed: {}",
                  sqlite3_errmsg(db_));
        return std::nullopt;
    }
    std::optional<SmtpConfigRow> out;
    if (sqlite3_step(st) == SQLITE_ROW) {
        SmtpConfigRow row;
        row.enabled = sqlite3_column_int(st, 0) != 0;
        auto col_text = [&](int i) -> std::string {
            const auto* s = reinterpret_cast<const char*>(sqlite3_column_text(st, i));
            return s ? std::string(s) : std::string();
        };
        row.server      = col_text(1);
        row.port        = sqlite3_column_int(st, 2);
        row.security    = col_text(3);
        row.username    = col_text(4);
        const auto* blob = static_cast<const std::uint8_t*>(
            sqlite3_column_blob(st, 5));
        const int blob_n = sqlite3_column_bytes(st, 5);
        if (blob && blob_n > 0) {
            row.password_encrypted.assign(blob, blob + blob_n);
        }
        row.from_email  = col_text(6);
        row.from_name   = col_text(7);
        row.timeout_sec = sqlite3_column_int(st, 8);
        if (sqlite3_column_type(st, 9) != SQLITE_NULL)
            row.updated_at = sqlite3_column_int64(st, 9);
        if (sqlite3_column_type(st, 10) != SQLITE_NULL)
            row.updated_by = sqlite3_column_int64(st, 10);
        out = std::move(row);
    }
    sqlite3_finalize(st);
    return out;
}

bool AuthDb::writeSmtpConfigRow(const SmtpConfigRow& row) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!db_) return false;
    constexpr const char* kSql =
        "INSERT INTO smtp_config("
        "  id, enabled, server, port, security, username, "
        "  password_encrypted, from_email, from_name, timeout_sec, "
        "  updated_at, updated_by) "
        "VALUES(1, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(id) DO UPDATE SET "
        "  enabled=excluded.enabled, server=excluded.server, "
        "  port=excluded.port, security=excluded.security, "
        "  username=excluded.username, "
        "  password_encrypted=excluded.password_encrypted, "
        "  from_email=excluded.from_email, from_name=excluded.from_name, "
        "  timeout_sec=excluded.timeout_sec, "
        "  updated_at=excluded.updated_at, updated_by=excluded.updated_by";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) {
        LOG_ERROR("AuthDb::writeSmtpConfigRow prepare failed: {}",
                  sqlite3_errmsg(db_));
        return false;
    }
    sqlite3_bind_int (st,  1, row.enabled ? 1 : 0);
    sqlite3_bind_text(st,  2, row.server.c_str(),
                      static_cast<int>(row.server.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int (st,  3, row.port);
    sqlite3_bind_text(st,  4, row.security.c_str(),
                      static_cast<int>(row.security.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(st,  5, row.username.c_str(),
                      static_cast<int>(row.username.size()), SQLITE_TRANSIENT);
    if (row.password_encrypted.empty()) {
        sqlite3_bind_null(st, 6);
    } else {
        sqlite3_bind_blob(st, 6, row.password_encrypted.data(),
                          static_cast<int>(row.password_encrypted.size()),
                          SQLITE_TRANSIENT);
    }
    sqlite3_bind_text(st,  7, row.from_email.c_str(),
                      static_cast<int>(row.from_email.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(st,  8, row.from_name.c_str(),
                      static_cast<int>(row.from_name.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int (st,  9, row.timeout_sec);
    if (row.updated_at.has_value())
        sqlite3_bind_int64(st, 10, *row.updated_at);
    else
        sqlite3_bind_null(st, 10);
    if (row.updated_by.has_value())
        sqlite3_bind_int64(st, 11, *row.updated_by);
    else
        sqlite3_bind_null(st, 11);
    const int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        LOG_ERROR("AuthDb::writeSmtpConfigRow step failed: {}",
                  sqlite3_errmsg(db_));
        return false;
    }
    return true;
}

// ── Audit log (commit 12/24) ──────────────────────────────────────────

bool AuthDb::insertAuditEvent(const AuditEvent& e) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!db_) return false;

    constexpr const char* kSql =
        "INSERT INTO auth_audit("
        "  ts, event, user_id, username, ip, details_json) "
        "VALUES(?, ?, ?, ?, ?, ?)";

    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) {
        LOG_ERROR("AuthDb::insertAuditEvent prepare failed: {}",
                  sqlite3_errmsg(db_));
        return false;
    }

    int i = 1;
    sqlite3_bind_int64(st, i++, e.ts ? e.ts : nowUnixSec());
    sqlite3_bind_text (st, i++, e.event.c_str(), -1, SQLITE_TRANSIENT);
    if (e.user_id) sqlite3_bind_int64(st, i++, *e.user_id);
    else sqlite3_bind_null(st, i++);
    if (e.username.empty()) sqlite3_bind_null(st, i++);
    else sqlite3_bind_text(st, i++, e.username.c_str(), -1, SQLITE_TRANSIENT);
    if (e.ip.empty()) sqlite3_bind_null(st, i++);
    else sqlite3_bind_text(st, i++, e.ip.c_str(), -1, SQLITE_TRANSIENT);
    if (e.details_json.empty()) sqlite3_bind_null(st, i++);
    else sqlite3_bind_text(st, i++, e.details_json.c_str(), -1, SQLITE_TRANSIENT);

    const int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        LOG_ERROR("AuthDb::insertAuditEvent step rc={}: {}", rc, sqlite3_errmsg(db_));
        return false;
    }
    return true;
}

std::vector<AuditEvent> AuthDb::listAuditEvents(const AuditFilter& f) {
    std::lock_guard<std::mutex> lk(mutex_);
    std::vector<AuditEvent> out;
    if (!db_) return out;

    // Динамический WHERE — собираем только реально заполненные фильтры,
    // чтобы не индексировать NULL-сравнения.
    std::string sql =
        "SELECT id, ts, event, user_id, username, ip, details_json "
        "FROM auth_audit WHERE 1=1";

    if (f.from_ts)        sql += " AND ts >= ?";
    if (f.to_ts)          sql += " AND ts <  ?";
    if (f.user_id)        sql += " AND user_id = ?";
    if (!f.username.empty()) sql += " AND username = ?";
    if (!f.event.empty())    sql += " AND event = ?";

    // Пагинация — клиент GET /api/auth/audit просит limit+offset.
    // Cap 1000 — защита от случайного "?limit=999999999".
    sql += " ORDER BY ts DESC, id DESC LIMIT ? OFFSET ?";

    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
        LOG_ERROR("AuthDb::listAuditEvents prepare failed: {}",
                  sqlite3_errmsg(db_));
        return out;
    }

    int i = 1;
    if (f.from_ts) sqlite3_bind_int64(st, i++, *f.from_ts);
    if (f.to_ts)   sqlite3_bind_int64(st, i++, *f.to_ts);
    if (f.user_id) sqlite3_bind_int64(st, i++, *f.user_id);
    if (!f.username.empty())
        sqlite3_bind_text(st, i++, f.username.c_str(), -1, SQLITE_TRANSIENT);
    if (!f.event.empty())
        sqlite3_bind_text(st, i++, f.event.c_str(), -1, SQLITE_TRANSIENT);

    int limit = f.limit;
    if (limit <= 0)     limit = 100;
    if (limit > 1000)   limit = 1000;
    int offset = f.offset < 0 ? 0 : f.offset;
    sqlite3_bind_int(st, i++, limit);
    sqlite3_bind_int(st, i++, offset);

    while (sqlite3_step(st) == SQLITE_ROW) {
        AuditEvent e;
        e.id           = sqlite3_column_int64(st, 0);
        e.ts           = sqlite3_column_int64(st, 1);
        e.event        = colText(st, 2);
        e.user_id      = colInt64Opt(st, 3);
        e.username     = colText(st, 4);
        e.ip           = colText(st, 5);
        e.details_json = colText(st, 6);
        out.push_back(std::move(e));
    }
    sqlite3_finalize(st);
    return out;
}

int AuthDb::purgeAuditOlderThan(std::int64_t cutoff_ts) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!db_) return 0;
    constexpr const char* kSql = "DELETE FROM auth_audit WHERE ts < ?";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) return 0;
    sqlite3_bind_int64(st, 1, cutoff_ts);
    const int rc = sqlite3_step(st);
    const int n  = sqlite3_changes(db_);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? n : 0;
}

// ── channel_permissions CRUD (commit 22/24) ───────────────────────────

bool AuthDb::setChannelPermission(std::int64_t user_id,
                                  std::int64_t channel_id,
                                  ChannelPermission permission,
                                  std::int64_t granted_by,
                                  std::int64_t granted_at) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!db_) return false;
    // UPSERT: ON CONFLICT (user_id, channel_id) DO UPDATE — обновляем
    // permission, granted_at, granted_by. Это admin-сценарий «поменял
    // permission»; запись о предыдущем состоянии живёт в auth_audit.
    constexpr const char* kSql =
        "INSERT INTO channel_permissions"
        " (user_id, channel_id, permission, granted_at, granted_by)"
        " VALUES (?, ?, ?, ?, ?)"
        " ON CONFLICT(user_id, channel_id) DO UPDATE SET"
        "   permission = excluded.permission,"
        "   granted_at = excluded.granted_at,"
        "   granted_by = excluded.granted_by";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) {
        LOG_ERROR("AuthDb: prepare setChannelPermission failed: {}",
                  sqlite3_errmsg(db_));
        return false;
    }
    sqlite3_bind_int64(st, 1, user_id);
    sqlite3_bind_int64(st, 2, channel_id);
    const auto* perm_str = channelPermissionName(permission);
    sqlite3_bind_text (st, 3, perm_str, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 4, granted_at);
    // granted_by == 0 → bind NULL (system / pre-RBAC actor, FK-friendly).
    if (granted_by > 0) sqlite3_bind_int64(st, 5, granted_by);
    else                sqlite3_bind_null (st, 5);
    const int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        LOG_ERROR("AuthDb: setChannelPermission step failed: {}",
                  sqlite3_errmsg(db_));
        return false;
    }
    return true;
}

bool AuthDb::removeChannelPermission(std::int64_t user_id,
                                     std::int64_t channel_id) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!db_) return false;
    constexpr const char* kSql =
        "DELETE FROM channel_permissions"
        " WHERE user_id = ? AND channel_id = ?";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(st, 1, user_id);
    sqlite3_bind_int64(st, 2, channel_id);
    const int rc = sqlite3_step(st);
    const int n  = sqlite3_changes(db_);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) return false;
    return n > 0;
}

int AuthDb::removeAllChannelPermissionsForUser(std::int64_t user_id) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!db_) return 0;
    constexpr const char* kSql =
        "DELETE FROM channel_permissions WHERE user_id = ?";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) return 0;
    sqlite3_bind_int64(st, 1, user_id);
    const int rc = sqlite3_step(st);
    const int n  = sqlite3_changes(db_);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? n : 0;
}

std::vector<ChannelGrant>
AuthDb::listChannelGrantsForUser(std::int64_t user_id) {
    std::lock_guard<std::mutex> lk(mutex_);
    std::vector<ChannelGrant> out;
    if (!db_) return out;
    constexpr const char* kSql =
        "SELECT channel_id, permission FROM channel_permissions"
        " WHERE user_id = ? ORDER BY channel_id";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) return out;
    sqlite3_bind_int64(st, 1, user_id);
    while (sqlite3_step(st) == SQLITE_ROW) {
        ChannelGrant g;
        g.channel_id = sqlite3_column_int64(st, 0);
        const auto perm = channelPermissionFromString(colText(st, 1));
        if (!perm.has_value()) continue;  // CHECK constraint должен спасти
        g.permission = *perm;
        out.push_back(g);
    }
    sqlite3_finalize(st);
    return out;
}

std::vector<AuthDb::ChannelPermissionRow>
AuthDb::listChannelPermissionsForUser(std::int64_t user_id) {
    std::lock_guard<std::mutex> lk(mutex_);
    std::vector<ChannelPermissionRow> out;
    if (!db_) return out;
    constexpr const char* kSql =
        "SELECT cp.user_id, cp.channel_id, cp.permission,"
        "       cp.granted_at, cp.granted_by, COALESCE(u.username, '')"
        "  FROM channel_permissions cp"
        "  LEFT JOIN users u ON u.id = cp.user_id"
        " WHERE cp.user_id = ?"
        " ORDER BY cp.channel_id";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) return out;
    sqlite3_bind_int64(st, 1, user_id);
    while (sqlite3_step(st) == SQLITE_ROW) {
        ChannelPermissionRow r;
        r.user_id    = sqlite3_column_int64(st, 0);
        r.channel_id = sqlite3_column_int64(st, 1);
        const auto perm = channelPermissionFromString(colText(st, 2));
        if (!perm.has_value()) continue;
        r.permission = *perm;
        r.granted_at = sqlite3_column_int64(st, 3);
        r.granted_by = sqlite3_column_int64(st, 4);
        r.username   = colText(st, 5);
        out.push_back(std::move(r));
    }
    sqlite3_finalize(st);
    return out;
}

std::vector<AuthDb::ChannelPermissionRow>
AuthDb::listChannelPermissionsForChannel(std::int64_t channel_id) {
    std::lock_guard<std::mutex> lk(mutex_);
    std::vector<ChannelPermissionRow> out;
    if (!db_) return out;
    constexpr const char* kSql =
        "SELECT cp.user_id, cp.channel_id, cp.permission,"
        "       cp.granted_at, cp.granted_by, COALESCE(u.username, '')"
        "  FROM channel_permissions cp"
        "  LEFT JOIN users u ON u.id = cp.user_id"
        " WHERE cp.channel_id = ?"
        " ORDER BY u.username";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) return out;
    sqlite3_bind_int64(st, 1, channel_id);
    while (sqlite3_step(st) == SQLITE_ROW) {
        ChannelPermissionRow r;
        r.user_id    = sqlite3_column_int64(st, 0);
        r.channel_id = sqlite3_column_int64(st, 1);
        const auto perm = channelPermissionFromString(colText(st, 2));
        if (!perm.has_value()) continue;
        r.permission = *perm;
        r.granted_at = sqlite3_column_int64(st, 3);
        r.granted_by = sqlite3_column_int64(st, 4);
        r.username   = colText(st, 5);
        out.push_back(std::move(r));
    }
    sqlite3_finalize(st);
    return out;
}

}  // namespace liveqx::auth
