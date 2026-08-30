// Enterprise audit trail — SQLite backing with HMAC-chained rows.
//
// Chain formula:
//   mac[0] = HMAC-SHA256(master_key, canonical(row[0]))
//   mac[N] = HMAC-SHA256(master_key, prev_mac || canonical(row[N]))
//
// Every row stores prev_mac + mac + key_fingerprint. verifyChain rescans
// the table and re-derives each mac; a mismatch flags tampering.
// Key rotation is expected: rows with a fingerprint different from the
// current key are trusted (we cannot verify MACs signed by a lost key).
// Purge does not rewrite the chain — a hole before cutoff is fine, the
// verification cursor treats the first surviving row as chain head.

#include "audit/AuditDb.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <sstream>

#include <sqlite3.h>
#include <nlohmann/json.hpp>

#include "auth/MasterKey.h"
#include "utils/Log.h"

namespace liveqx::audit {
namespace {

using json = nlohmann::json;

constexpr int  kSchemaVersion   = 1;
constexpr int  kMaxListLimit    = 1000;
constexpr auto kMetaLastMacKey  = "last_mac";  // hex-encoded

constexpr const char* kSchemaSqlV1 = R"sql(
CREATE TABLE IF NOT EXISTS audit_events (
    id                 INTEGER PRIMARY KEY AUTOINCREMENT,
    ts_unix_ms         INTEGER NOT NULL,
    category           TEXT    NOT NULL,
    action             TEXT    NOT NULL,
    actor_user_id      INTEGER,
    actor_username     TEXT,
    actor_role         TEXT,
    actor_ip           TEXT,
    target_type        TEXT,
    target_id          TEXT,
    http_method        TEXT,
    http_path          TEXT,
    http_status        INTEGER NOT NULL DEFAULT 0,
    elapsed_ms         INTEGER NOT NULL DEFAULT 0,
    summary            TEXT,
    details_json       TEXT    NOT NULL DEFAULT '{}',
    request_id         TEXT,
    prev_mac           BLOB,
    mac                BLOB    NOT NULL,
    key_fingerprint    TEXT    NOT NULL,
    legacy             INTEGER NOT NULL DEFAULT 0
);

CREATE INDEX IF NOT EXISTS idx_audit_events_ts
    ON audit_events(ts_unix_ms DESC);

CREATE INDEX IF NOT EXISTS idx_audit_events_actor
    ON audit_events(actor_user_id, ts_unix_ms DESC);

CREATE INDEX IF NOT EXISTS idx_audit_events_target
    ON audit_events(target_type, target_id, ts_unix_ms DESC);

CREATE INDEX IF NOT EXISTS idx_audit_events_category_ts
    ON audit_events(category, ts_unix_ms DESC);

CREATE INDEX IF NOT EXISTS idx_audit_events_action_ts
    ON audit_events(action, ts_unix_ms DESC);

CREATE TABLE IF NOT EXISTS audit_meta (
    key    TEXT PRIMARY KEY,
    value  TEXT NOT NULL
);
)sql";

std::int64_t nowUnixMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string toHex(const std::vector<std::uint8_t>& b) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string s;
    s.reserve(b.size() * 2);
    for (auto x : b) {
        s.push_back(kHex[(x >> 4) & 0xF]);
        s.push_back(kHex[x & 0xF]);
    }
    return s;
}

std::vector<std::uint8_t> fromHex(std::string_view s) {
    auto v = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::vector<std::uint8_t> out;
    if (s.size() % 2) return out;
    out.reserve(s.size() / 2);
    for (std::size_t i = 0; i + 1 < s.size(); i += 2) {
        int hi = v(s[i]);
        int lo = v(s[i + 1]);
        if (hi < 0 || lo < 0) { out.clear(); return out; }
        out.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
    }
    return out;
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
        LOG_ERROR("AuditDb: cannot rename corrupt {} -> {}: {}",
                  p.string(), target.string(), ec.message());
    } else {
        LOG_WARN("AuditDb: corrupt db moved to {}", target.string());
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

std::vector<std::uint8_t> colBlob(sqlite3_stmt* st, int idx) {
    if (sqlite3_column_type(st, idx) == SQLITE_NULL) return {};
    const auto* p = static_cast<const std::uint8_t*>(sqlite3_column_blob(st, idx));
    const int n   = sqlite3_column_bytes(st, idx);
    if (!p || n <= 0) return {};
    return std::vector<std::uint8_t>(p, p + n);
}

}  // namespace

AuditDb::AuditDb(std::filesystem::path db_path,
                 const liveqx::auth::MasterKey* key)
    : path_(std::move(db_path)), key_(key) {}

AuditDb::~AuditDb() {
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

bool AuditDb::exec(const char* sql) {
    char* err = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        LOG_ERROR("AuditDb: exec failed: {} ({})",
                  err ? err : "(null)", sqlite3_errmsg(db_));
        sqlite3_free(err);
        return false;
    }
    return true;
}

bool AuditDb::runMigrations() {
    int existing = 0;
    if (!readUserVersion(db_, existing)) {
        LOG_ERROR("AuditDb: cannot read user_version: {}", sqlite3_errmsg(db_));
        return false;
    }
    if (existing > kSchemaVersion) {
        LOG_ERROR("AuditDb: schema v{} newer than supported v{} — refusing {}",
                  existing, kSchemaVersion, path_.string());
        return false;
    }

    if (!exec("PRAGMA journal_mode=WAL;") ||
        !exec("PRAGMA synchronous=NORMAL;")) {
        return false;
    }

    if (existing < 1) {
        if (!exec(kSchemaSqlV1)) return false;
    }
    if (existing < kSchemaVersion) {
        const std::string set_pragma =
            "PRAGMA user_version=" + std::to_string(kSchemaVersion) + ";";
        if (!exec(set_pragma.c_str())) return false;
    }
    return true;
}

bool AuditDb::loadChainTail() {
    last_mac_.clear();
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_,
            "SELECT value FROM audit_meta WHERE key=?", -1, &st, nullptr)
            != SQLITE_OK) return false;
    sqlite3_bind_text(st, 1, kMetaLastMacKey, -1, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW) {
        last_mac_ = fromHex(colText(st, 0));
    }
    sqlite3_finalize(st);

    // Fallback: cursor missing (fresh install or lost). Recover from
    // the newest row so the chain does not restart mid-life.
    if (last_mac_.empty()) {
        sqlite3_stmt* s2 = nullptr;
        if (sqlite3_prepare_v2(db_,
                "SELECT mac FROM audit_events ORDER BY id DESC LIMIT 1",
                -1, &s2, nullptr) == SQLITE_OK) {
            if (sqlite3_step(s2) == SQLITE_ROW) {
                last_mac_ = colBlob(s2, 0);
            }
            sqlite3_finalize(s2);
        }
    }
    return true;
}

bool AuditDb::saveChainTail() {
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_,
            "INSERT INTO audit_meta(key,value) VALUES(?,?) "
            "ON CONFLICT(key) DO UPDATE SET value=excluded.value",
            -1, &st, nullptr) != SQLITE_OK) return false;
    const std::string hex = toHex(last_mac_);
    sqlite3_bind_text(st, 1, kMetaLastMacKey, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, hex.c_str(), -1, SQLITE_TRANSIENT);
    const int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE;
}

bool AuditDb::open() {
    std::lock_guard<std::mutex> lk(mutex_);
    if (db_) return true;

    std::error_code ec;
    if (path_.has_parent_path())
        std::filesystem::create_directories(path_.parent_path(), ec);

    if (sqlite3_open(path_.string().c_str(), &db_) != SQLITE_OK) {
        LOG_ERROR("AuditDb: open {} failed: {}",
                  path_.string(), db_ ? sqlite3_errmsg(db_) : "(no db)");
        if (db_) { sqlite3_close(db_); db_ = nullptr; }
        renameCorrupt(path_);
        return false;
    }
    if (!runMigrations()) {
        sqlite3_close(db_); db_ = nullptr;
        renameCorrupt(path_);
        return false;
    }
    loadChainTail();
    LOG_INFO("AuditDb opened at {} (chain tail={} bytes)",
             path_.string(), last_mac_.size());
    return true;
}

std::string AuditDb::canonicalize(const AuditEvent& e) {
    // Ordered JSON — nlohmann::json preserves insertion order and
    // serializes deterministically with default flags. We build the
    // object with a fixed field order so verifyChain reproduces bit-
    // identical bytes years later.
    json j;
    j["ts_unix_ms"]     = e.ts_unix_ms;
    j["category"]       = categoryName(e.category);
    j["action"]         = e.action;
    j["actor_user_id"]  = e.actor_user_id.has_value()
                            ? json(*e.actor_user_id) : json(nullptr);
    j["actor_username"] = e.actor_username;
    j["actor_role"]     = e.actor_role;
    j["actor_ip"]       = e.actor_ip;
    j["target_type"]    = e.target_type;
    j["target_id"]      = e.target_id;
    j["http_method"]    = e.http_method;
    j["http_path"]      = e.http_path;
    j["http_status"]    = e.http_status;
    j["elapsed_ms"]     = e.elapsed_ms;
    j["summary"]        = e.summary;
    j["details_json"]   = e.details_json;
    j["request_id"]     = e.request_id;
    return j.dump();
}

std::optional<std::int64_t> AuditDb::insert(AuditEvent& e) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!db_ || !key_ || !key_->loaded()) return std::nullopt;

    if (e.ts_unix_ms == 0) e.ts_unix_ms = nowUnixMs();
    e.prev_mac = last_mac_;

    // HMAC input = prev_mac || canonical(record)
    std::string msg;
    msg.append(reinterpret_cast<const char*>(e.prev_mac.data()),
               e.prev_mac.size());
    const std::string canon = canonicalize(e);
    msg.append(canon);
    auto mac = key_->hmacSha256(msg);
    e.mac.assign(mac.begin(), mac.end());
    e.key_fingerprint = key_->fingerprint();

    sqlite3_stmt* st = nullptr;
    constexpr const char* kSql =
        "INSERT INTO audit_events("
        "  ts_unix_ms, category, action, "
        "  actor_user_id, actor_username, actor_role, actor_ip, "
        "  target_type, target_id, "
        "  http_method, http_path, http_status, elapsed_ms, "
        "  summary, details_json, request_id, "
        "  prev_mac, mac, key_fingerprint, legacy) "
        "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) {
        LOG_ERROR("AuditDb::insert prepare failed: {}", sqlite3_errmsg(db_));
        return std::nullopt;
    }
    int i = 1;
    sqlite3_bind_int64(st, i++, e.ts_unix_ms);
    sqlite3_bind_text (st, i++, categoryName(e.category), -1, SQLITE_STATIC);
    sqlite3_bind_text (st, i++, e.action.c_str(), -1, SQLITE_TRANSIENT);
    if (e.actor_user_id) sqlite3_bind_int64(st, i++, *e.actor_user_id);
    else sqlite3_bind_null(st, i++);
    sqlite3_bind_text (st, i++, e.actor_username.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (st, i++, e.actor_role.c_str(),     -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (st, i++, e.actor_ip.c_str(),       -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (st, i++, e.target_type.c_str(),    -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (st, i++, e.target_id.c_str(),      -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (st, i++, e.http_method.c_str(),    -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (st, i++, e.http_path.c_str(),      -1, SQLITE_TRANSIENT);
    sqlite3_bind_int  (st, i++, e.http_status);
    sqlite3_bind_int64(st, i++, e.elapsed_ms);
    sqlite3_bind_text (st, i++, e.summary.c_str(),        -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (st, i++, e.details_json.c_str(),   -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (st, i++, e.request_id.c_str(),     -1, SQLITE_TRANSIENT);
    if (e.prev_mac.empty()) sqlite3_bind_null(st, i++);
    else sqlite3_bind_blob(st, i++, e.prev_mac.data(),
                           static_cast<int>(e.prev_mac.size()), SQLITE_TRANSIENT);
    sqlite3_bind_blob (st, i++, e.mac.data(),
                       static_cast<int>(e.mac.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text (st, i++, e.key_fingerprint.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int  (st, i++, e.legacy ? 1 : 0);

    const int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        LOG_ERROR("AuditDb::insert step failed: {}", sqlite3_errmsg(db_));
        return std::nullopt;
    }
    e.id = sqlite3_last_insert_rowid(db_);
    last_mac_ = e.mac;
    saveChainTail();
    return e.id;
}

std::size_t AuditDb::insertBatch(std::vector<AuditEvent>& batch) {
    if (batch.empty()) return 0;
    std::lock_guard<std::mutex> lk(mutex_);
    if (!db_ || !key_ || !key_->loaded()) return 0;

    if (sqlite3_exec(db_, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr)
            != SQLITE_OK) {
        LOG_ERROR("AuditDb::insertBatch BEGIN failed: {}", sqlite3_errmsg(db_));
        return 0;
    }

    sqlite3_stmt* st = nullptr;
    constexpr const char* kSql =
        "INSERT INTO audit_events("
        "  ts_unix_ms, category, action, "
        "  actor_user_id, actor_username, actor_role, actor_ip, "
        "  target_type, target_id, "
        "  http_method, http_path, http_status, elapsed_ms, "
        "  summary, details_json, request_id, "
        "  prev_mac, mac, key_fingerprint, legacy) "
        "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) {
        LOG_ERROR("AuditDb::insertBatch prepare failed: {}", sqlite3_errmsg(db_));
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        return 0;
    }

    const std::string fp = key_->fingerprint();
    std::vector<std::uint8_t> chain = last_mac_;

    std::size_t written = 0;
    for (auto& e : batch) {
        if (e.ts_unix_ms == 0) e.ts_unix_ms = nowUnixMs();
        e.prev_mac = chain;

        std::string msg;
        msg.append(reinterpret_cast<const char*>(e.prev_mac.data()),
                   e.prev_mac.size());
        msg.append(canonicalize(e));
        auto mac = key_->hmacSha256(msg);
        e.mac.assign(mac.begin(), mac.end());
        e.key_fingerprint = fp;

        sqlite3_reset(st);
        sqlite3_clear_bindings(st);
        int i = 1;
        sqlite3_bind_int64(st, i++, e.ts_unix_ms);
        sqlite3_bind_text (st, i++, categoryName(e.category), -1, SQLITE_STATIC);
        sqlite3_bind_text (st, i++, e.action.c_str(), -1, SQLITE_TRANSIENT);
        if (e.actor_user_id) sqlite3_bind_int64(st, i++, *e.actor_user_id);
        else sqlite3_bind_null(st, i++);
        sqlite3_bind_text (st, i++, e.actor_username.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text (st, i++, e.actor_role.c_str(),     -1, SQLITE_TRANSIENT);
        sqlite3_bind_text (st, i++, e.actor_ip.c_str(),       -1, SQLITE_TRANSIENT);
        sqlite3_bind_text (st, i++, e.target_type.c_str(),    -1, SQLITE_TRANSIENT);
        sqlite3_bind_text (st, i++, e.target_id.c_str(),      -1, SQLITE_TRANSIENT);
        sqlite3_bind_text (st, i++, e.http_method.c_str(),    -1, SQLITE_TRANSIENT);
        sqlite3_bind_text (st, i++, e.http_path.c_str(),      -1, SQLITE_TRANSIENT);
        sqlite3_bind_int  (st, i++, e.http_status);
        sqlite3_bind_int64(st, i++, e.elapsed_ms);
        sqlite3_bind_text (st, i++, e.summary.c_str(),        -1, SQLITE_TRANSIENT);
        sqlite3_bind_text (st, i++, e.details_json.c_str(),   -1, SQLITE_TRANSIENT);
        sqlite3_bind_text (st, i++, e.request_id.c_str(),     -1, SQLITE_TRANSIENT);
        if (e.prev_mac.empty()) sqlite3_bind_null(st, i++);
        else sqlite3_bind_blob(st, i++, e.prev_mac.data(),
                               static_cast<int>(e.prev_mac.size()),
                               SQLITE_TRANSIENT);
        sqlite3_bind_blob (st, i++, e.mac.data(),
                           static_cast<int>(e.mac.size()), SQLITE_TRANSIENT);
        sqlite3_bind_text (st, i++, e.key_fingerprint.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int  (st, i++, e.legacy ? 1 : 0);

        if (sqlite3_step(st) != SQLITE_DONE) {
            LOG_ERROR("AuditDb::insertBatch row {} step failed: {}",
                      written, sqlite3_errmsg(db_));
            sqlite3_finalize(st);
            sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
            return 0;
        }
        e.id = sqlite3_last_insert_rowid(db_);
        chain = e.mac;
        ++written;
    }
    sqlite3_finalize(st);

    if (sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK) {
        LOG_ERROR("AuditDb::insertBatch COMMIT failed: {}", sqlite3_errmsg(db_));
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        return 0;
    }
    last_mac_ = chain;
    saveChainTail();
    return written;
}

namespace {

struct Query {
    std::string sql;
    // Bindings applied in order; kept out-of-band to survive std::string
    // copies in the where-builder.
    std::vector<std::pair<int, std::int64_t>> ints;   // (index, val)
    std::vector<std::pair<int, std::string>>  texts;
};

Query buildListQuery(const AuditFilter& f, bool count_only) {
    Query q;
    std::ostringstream sql;
    if (count_only) {
        sql << "SELECT COUNT(*) FROM audit_events";
    } else {
        sql << "SELECT id, ts_unix_ms, category, action, "
               "actor_user_id, actor_username, actor_role, actor_ip, "
               "target_type, target_id, "
               "http_method, http_path, http_status, elapsed_ms, "
               "summary, details_json, request_id, "
               "prev_mac, mac, key_fingerprint, legacy "
               "FROM audit_events";
    }

    std::vector<std::string> where;
    int idx = 1;

    if (f.from_ts_ms) {
        where.push_back("ts_unix_ms >= ?");
        q.ints.emplace_back(idx++, *f.from_ts_ms);
    }
    if (f.to_ts_ms) {
        where.push_back("ts_unix_ms < ?");
        q.ints.emplace_back(idx++, *f.to_ts_ms);
    }
    if (f.category) {
        where.push_back("category = ?");
        q.texts.emplace_back(idx++, categoryName(*f.category));
    }
    if (!f.action.empty()) {
        where.push_back("action = ?");
        q.texts.emplace_back(idx++, f.action);
    }
    if (f.actor_user_id) {
        where.push_back("actor_user_id = ?");
        q.ints.emplace_back(idx++, *f.actor_user_id);
    }
    if (!f.actor_username.empty()) {
        where.push_back("actor_username = ?");
        q.texts.emplace_back(idx++, f.actor_username);
    }
    if (!f.target_type.empty()) {
        where.push_back("target_type = ?");
        q.texts.emplace_back(idx++, f.target_type);
    }
    if (!f.target_id.empty()) {
        where.push_back("target_id = ?");
        q.texts.emplace_back(idx++, f.target_id);
    }
    if (!f.request_id.empty()) {
        where.push_back("request_id = ?");
        q.texts.emplace_back(idx++, f.request_id);
    }

    if (!where.empty()) {
        sql << " WHERE " << where[0];
        for (std::size_t i = 1; i < where.size(); ++i) sql << " AND " << where[i];
    }
    if (!count_only) {
        int limit = std::clamp(f.limit, 1, kMaxListLimit);
        int offset = std::max(0, f.offset);
        sql << " ORDER BY ts_unix_ms DESC, id DESC LIMIT " << limit
            << " OFFSET " << offset;
    }
    q.sql = sql.str();
    return q;
}

}  // namespace

std::vector<AuditEvent> AuditDb::list(const AuditFilter& f) {
    std::vector<AuditEvent> out;
    std::lock_guard<std::mutex> lk(mutex_);
    if (!db_) return out;

    const Query q = buildListQuery(f, /*count_only=*/false);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, q.sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
        LOG_ERROR("AuditDb::list prepare failed: {} sql={}",
                  sqlite3_errmsg(db_), q.sql);
        return out;
    }
    for (const auto& [i, v] : q.ints)   sqlite3_bind_int64(st, i, v);
    for (const auto& [i, v] : q.texts)
        sqlite3_bind_text(st, i, v.c_str(), -1, SQLITE_TRANSIENT);

    while (sqlite3_step(st) == SQLITE_ROW) {
        AuditEvent e;
        int c = 0;
        e.id             = sqlite3_column_int64(st, c++);
        e.ts_unix_ms     = sqlite3_column_int64(st, c++);
        e.category       = categoryFromString(colText(st, c++))
                              .value_or(Category::System);
        e.action         = colText(st, c++);
        e.actor_user_id  = colInt64Opt(st, c++);
        e.actor_username = colText(st, c++);
        e.actor_role     = colText(st, c++);
        e.actor_ip       = colText(st, c++);
        e.target_type    = colText(st, c++);
        e.target_id      = colText(st, c++);
        e.http_method    = colText(st, c++);
        e.http_path      = colText(st, c++);
        e.http_status    = sqlite3_column_int(st, c++);
        e.elapsed_ms     = sqlite3_column_int64(st, c++);
        e.summary        = colText(st, c++);
        e.details_json   = colText(st, c++);
        e.request_id     = colText(st, c++);
        e.prev_mac       = colBlob(st, c++);
        e.mac            = colBlob(st, c++);
        e.key_fingerprint= colText(st, c++);
        e.legacy         = sqlite3_column_int(st, c++) != 0;
        out.push_back(std::move(e));
    }
    sqlite3_finalize(st);
    return out;
}

std::int64_t AuditDb::count(const AuditFilter& f) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!db_) return 0;

    const Query q = buildListQuery(f, /*count_only=*/true);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, q.sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
        return 0;
    }
    for (const auto& [i, v] : q.ints)   sqlite3_bind_int64(st, i, v);
    for (const auto& [i, v] : q.texts)
        sqlite3_bind_text(st, i, v.c_str(), -1, SQLITE_TRANSIENT);

    std::int64_t n = 0;
    if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return n;
}

int AuditDb::purgeOlderThan(Category cat, std::int64_t cutoff_ms) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!db_) return 0;

    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_,
            "DELETE FROM audit_events WHERE category=? AND ts_unix_ms < ?",
            -1, &st, nullptr) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_text (st, 1, categoryName(cat), -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 2, cutoff_ms);
    const int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) return 0;
    return sqlite3_changes(db_);
}

ChainVerification AuditDb::verifyChain() {
    ChainVerification v;
    std::lock_guard<std::mutex> lk(mutex_);
    if (!db_ || !key_ || !key_->loaded()) {
        v.first_bad_reason = "no_key";
        return v;
    }

    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_,
            "SELECT id, category, action, "
            "  ts_unix_ms, actor_user_id, actor_username, actor_role, actor_ip, "
            "  target_type, target_id, http_method, http_path, http_status, "
            "  elapsed_ms, summary, details_json, request_id, "
            "  prev_mac, mac, key_fingerprint "
            "FROM audit_events ORDER BY id ASC",
            -1, &st, nullptr) != SQLITE_OK) {
        v.first_bad_reason = "prepare_failed";
        return v;
    }

    const std::string current_fp = key_->fingerprint();
    std::vector<std::uint8_t> chain;   // rolling prev_mac we expect
    bool chain_initialised = false;

    while (sqlite3_step(st) == SQLITE_ROW) {
        v.scanned++;
        AuditEvent e;
        int c = 0;
        e.id             = sqlite3_column_int64(st, c++);
        e.category       = categoryFromString(colText(st, c++))
                              .value_or(Category::System);
        e.action         = colText(st, c++);
        e.ts_unix_ms     = sqlite3_column_int64(st, c++);
        e.actor_user_id  = colInt64Opt(st, c++);
        e.actor_username = colText(st, c++);
        e.actor_role     = colText(st, c++);
        e.actor_ip       = colText(st, c++);
        e.target_type    = colText(st, c++);
        e.target_id      = colText(st, c++);
        e.http_method    = colText(st, c++);
        e.http_path      = colText(st, c++);
        e.http_status    = sqlite3_column_int(st, c++);
        e.elapsed_ms     = sqlite3_column_int64(st, c++);
        e.summary        = colText(st, c++);
        e.details_json   = colText(st, c++);
        e.request_id     = colText(st, c++);
        e.prev_mac       = colBlob(st, c++);
        e.mac            = colBlob(st, c++);
        e.key_fingerprint= colText(st, c++);

        // Key rotation — rows signed under a different key cannot be
        // verified with the current one. Reset chain expectation to the
        // stored mac; we trust rows within the same fingerprint band.
        if (e.key_fingerprint != current_fp) {
            chain = e.mac;
            chain_initialised = true;
            continue;
        }

        if (chain_initialised && e.prev_mac != chain) {
            v.first_bad_id = e.id;
            v.first_bad_reason = "prev_mac_mismatch";
            break;
        }

        std::string msg;
        msg.append(reinterpret_cast<const char*>(e.prev_mac.data()),
                   e.prev_mac.size());
        msg.append(canonicalize(e));
        auto expect = key_->hmacSha256(msg);
        std::vector<std::uint8_t> expect_v(expect.begin(), expect.end());
        if (expect_v != e.mac) {
            v.first_bad_id = e.id;
            v.first_bad_reason = "mac_mismatch";
            break;
        }
        chain = e.mac;
        chain_initialised = true;
    }
    sqlite3_finalize(st);
    return v;
}

}  // namespace liveqx::audit
