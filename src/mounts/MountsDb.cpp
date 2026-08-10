#include "mounts/MountsDb.h"

#include <chrono>
#include <cstring>
#include <system_error>

#include <sqlite3.h>

#include "utils/Log.h"

namespace liveqx::mounts {
namespace {

constexpr int kSchemaVersion = 1;

constexpr const char* kSchemaSqlV1 = R"sql(
CREATE TABLE IF NOT EXISTS mounts (
  id                       INTEGER PRIMARY KEY AUTOINCREMENT,
  fs_type                  TEXT NOT NULL CHECK (fs_type IN ('cifs','nfs')),
  source                   TEXT NOT NULL,
  target                   TEXT NOT NULL,
  options                  TEXT NOT NULL DEFAULT '',
  ro                       INTEGER NOT NULL DEFAULT 1,
  cifs_username            TEXT NOT NULL DEFAULT '',
  cifs_password_encrypted  BLOB,
  cifs_domain              TEXT NOT NULL DEFAULT '',
  enabled                  INTEGER NOT NULL DEFAULT 1,
  created_at               INTEGER NOT NULL,
  updated_at               INTEGER NOT NULL,
  last_active_state        TEXT NOT NULL DEFAULT '',
  last_status_at           INTEGER NOT NULL DEFAULT 0
);

CREATE UNIQUE INDEX IF NOT EXISTS mounts_target_uniq ON mounts(target);
)sql";

bool readUserVersion(sqlite3* db, int& out) {
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, "PRAGMA user_version;", -1, &st, nullptr) != SQLITE_OK) {
        return false;
    }
    bool ok = false;
    if (sqlite3_step(st) == SQLITE_ROW) {
        out = sqlite3_column_int(st, 0);
        ok = true;
    }
    sqlite3_finalize(st);
    return ok;
}

std::int64_t nowSec() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string colText(sqlite3_stmt* st, int idx) {
    const auto* p = reinterpret_cast<const char*>(sqlite3_column_text(st, idx));
    return p ? std::string(p) : std::string();
}

std::vector<std::uint8_t> colBlob(sqlite3_stmt* st, int idx) {
    if (sqlite3_column_type(st, idx) == SQLITE_NULL) return {};
    const auto* p = static_cast<const std::uint8_t*>(sqlite3_column_blob(st, idx));
    const int n = sqlite3_column_bytes(st, idx);
    if (!p || n <= 0) return {};
    return std::vector<std::uint8_t>(p, p + n);
}

constexpr const char* kSelectColumns =
    "id, fs_type, source, target, options, ro, "
    "cifs_username, cifs_password_encrypted, cifs_domain, "
    "enabled, created_at, updated_at, "
    "last_active_state, last_status_at";

MountRow rowFrom(sqlite3_stmt* st) {
    MountRow r;
    r.id            = sqlite3_column_int64(st, 0);
    r.fs_type       = colText(st, 1);
    r.source        = colText(st, 2);
    r.target        = colText(st, 3);
    r.options       = colText(st, 4);
    r.ro            = sqlite3_column_int(st, 5) != 0;
    r.cifs_username = colText(st, 6);
    r.cifs_password_blob = colBlob(st, 7);
    r.cifs_domain   = colText(st, 8);
    r.enabled       = sqlite3_column_int(st, 9) != 0;
    r.created_at    = sqlite3_column_int64(st, 10);
    r.updated_at    = sqlite3_column_int64(st, 11);
    r.last_active_state = colText(st, 12);
    r.last_status_at    = sqlite3_column_int64(st, 13);
    return r;
}

}  // namespace

MountsDb::MountsDb(std::filesystem::path path) : path_(std::move(path)) {}
MountsDb::~MountsDb() {
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

bool MountsDb::exec(const char* sql) {
    char* err = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        LOG_ERROR("MountsDb: exec failed: {} ({})",
                  err ? err : "(null)", sqlite3_errmsg(db_));
        sqlite3_free(err);
        return false;
    }
    return true;
}

bool MountsDb::runMigrations() {
    int existing = 0;
    if (!readUserVersion(db_, existing)) {
        LOG_ERROR("MountsDb: cannot read user_version: {}", sqlite3_errmsg(db_));
        return false;
    }
    if (existing > kSchemaVersion) {
        LOG_ERROR("MountsDb: schema v{} newer than supported v{} — refusing {}",
                  existing, kSchemaVersion, path_.string());
        return false;
    }

    if (!exec("PRAGMA journal_mode=WAL;")
        || !exec("PRAGMA synchronous=NORMAL;")
        || !exec("PRAGMA foreign_keys=ON;")) {
        return false;
    }

    if (existing < 1) {
        if (!exec(kSchemaSqlV1)) return false;
        if (!exec(("PRAGMA user_version = "
                   + std::to_string(kSchemaVersion) + ";").c_str())) {
            return false;
        }
    }
    return true;
}

bool MountsDb::open() {
    std::lock_guard<std::mutex> lk(mu_);
    if (db_) return true;
    {
        std::error_code ec;
        std::filesystem::create_directories(path_.parent_path(), ec);
        // Если родительский каталог уже есть — ec.ok(); если нет прав —
        // sqlite_open ниже всё равно даст осмысленную ошибку.
    }
    if (sqlite3_open(path_.c_str(), &db_) != SQLITE_OK) {
        LOG_ERROR("MountsDb: cannot open {} ({})",
                  path_.string(), db_ ? sqlite3_errmsg(db_) : "null");
        if (db_) { sqlite3_close(db_); db_ = nullptr; }
        return false;
    }
    sqlite3_busy_timeout(db_, 5000);
    if (!runMigrations()) {
        sqlite3_close(db_);
        db_ = nullptr;
        return false;
    }
    return true;
}

std::vector<MountRow> MountsDb::listAll() {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<MountRow> out;
    if (!db_) return out;
    const std::string sql = std::string("SELECT ") + kSelectColumns
        + " FROM mounts ORDER BY id";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) return out;
    while (sqlite3_step(st) == SQLITE_ROW) {
        out.push_back(rowFrom(st));
    }
    sqlite3_finalize(st);
    return out;
}

std::optional<MountRow> MountsDb::findById(std::int64_t id) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!db_) return std::nullopt;
    const std::string sql = std::string("SELECT ") + kSelectColumns
        + " FROM mounts WHERE id = ?";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_int64(st, 1, id);
    std::optional<MountRow> r;
    if (sqlite3_step(st) == SQLITE_ROW) r = rowFrom(st);
    sqlite3_finalize(st);
    return r;
}

std::optional<MountRow> MountsDb::findByTarget(std::string_view target) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!db_) return std::nullopt;
    const std::string sql = std::string("SELECT ") + kSelectColumns
        + " FROM mounts WHERE target = ?";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_text(st, 1, target.data(),
                      static_cast<int>(target.size()), SQLITE_TRANSIENT);
    std::optional<MountRow> r;
    if (sqlite3_step(st) == SQLITE_ROW) r = rowFrom(st);
    sqlite3_finalize(st);
    return r;
}

std::optional<std::int64_t> MountsDb::insert(const MountRow& row) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!db_) return std::nullopt;
    const auto now = nowSec();

    const char* sql =
        "INSERT INTO mounts ("
        "  fs_type, source, target, options, ro,"
        "  cifs_username, cifs_password_encrypted, cifs_domain,"
        "  enabled, created_at, updated_at"
        ") VALUES (?,?,?,?,?, ?,?,?, ?,?,?)";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
        LOG_ERROR("MountsDb::insert prepare: {}", sqlite3_errmsg(db_));
        return std::nullopt;
    }
    sqlite3_bind_text (st, 1, row.fs_type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (st, 2, row.source.c_str(),  -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (st, 3, row.target.c_str(),  -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (st, 4, row.options.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int  (st, 5, row.ro ? 1 : 0);
    sqlite3_bind_text (st, 6, row.cifs_username.c_str(), -1, SQLITE_TRANSIENT);
    if (row.cifs_password_blob.empty()) {
        sqlite3_bind_null(st, 7);
    } else {
        sqlite3_bind_blob(st, 7, row.cifs_password_blob.data(),
                          static_cast<int>(row.cifs_password_blob.size()),
                          SQLITE_TRANSIENT);
    }
    sqlite3_bind_text (st, 8, row.cifs_domain.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int  (st, 9, row.enabled ? 1 : 0);
    sqlite3_bind_int64(st, 10, now);
    sqlite3_bind_int64(st, 11, now);

    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        LOG_WARN("MountsDb::insert: {}", sqlite3_errmsg(db_));
        return std::nullopt;
    }
    return sqlite3_last_insert_rowid(db_);
}

bool MountsDb::update(const MountRow& row) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!db_) return false;
    const auto now = nowSec();
    const char* sql =
        "UPDATE mounts SET "
        "  fs_type=?, source=?, target=?, options=?, ro=?,"
        "  cifs_username=?, cifs_password_encrypted=?, cifs_domain=?,"
        "  enabled=?, updated_at=? "
        "WHERE id=?";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text (st, 1, row.fs_type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (st, 2, row.source.c_str(),  -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (st, 3, row.target.c_str(),  -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (st, 4, row.options.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int  (st, 5, row.ro ? 1 : 0);
    sqlite3_bind_text (st, 6, row.cifs_username.c_str(), -1, SQLITE_TRANSIENT);
    if (row.cifs_password_blob.empty()) {
        sqlite3_bind_null(st, 7);
    } else {
        sqlite3_bind_blob(st, 7, row.cifs_password_blob.data(),
                          static_cast<int>(row.cifs_password_blob.size()),
                          SQLITE_TRANSIENT);
    }
    sqlite3_bind_text (st, 8, row.cifs_domain.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int  (st, 9, row.enabled ? 1 : 0);
    sqlite3_bind_int64(st, 10, now);
    sqlite3_bind_int64(st, 11, row.id);

    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE;
}

bool MountsDb::deleteById(std::int64_t id) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!db_) return false;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "DELETE FROM mounts WHERE id=?",
                           -1, &st, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_int64(st, 1, id);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE;
}

bool MountsDb::updateStatus(std::int64_t id,
                            std::string_view state,
                            std::int64_t ts) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!db_) return false;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_,
            "UPDATE mounts SET last_active_state=?, last_status_at=? WHERE id=?",
            -1, &st, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text (st, 1, state.data(),
                       static_cast<int>(state.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, ts);
    sqlite3_bind_int64(st, 3, id);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE;
}

}  // namespace liveqx::mounts
