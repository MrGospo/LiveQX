// state/players.db — see players/PlayersDb.h.

#include "players/PlayersDb.h"

#include "utils/Log.h"

#include <nlohmann/json.hpp>
#include <sodium.h>
#include <sqlite3.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>

namespace fs = std::filesystem;

namespace liveqx::players {
namespace {

std::int64_t wallNow() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::int64_t nowOr(std::int64_t now) { return now != 0 ? now : wallNow(); }

bool sodiumReady() {
    static const bool ok = (sodium_init() >= 0);
    return ok;
}

// ── prepared statement, RAII ────────────────────────────────────────────────
class Stmt {
public:
    Stmt(sqlite3* db, const char* sql) : db_(db) {
        if (sqlite3_prepare_v2(db, sql, -1, &st_, nullptr) != SQLITE_OK) {
            LOG_ERROR("PlayersDb: prepare failed: {} [{}]", sqlite3_errmsg(db), sql);
            st_ = nullptr;
        }
    }
    ~Stmt() { if (st_) sqlite3_finalize(st_); }
    Stmt(const Stmt&)            = delete;
    Stmt& operator=(const Stmt&) = delete;

    bool valid() const { return st_ != nullptr; }
    Stmt& text(int i, const std::string& v) {
        if (st_) sqlite3_bind_text(st_, i, v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
        return *this;
    }
    Stmt& i64(int i, std::int64_t v) {
        if (st_) sqlite3_bind_int64(st_, i, v);
        return *this;
    }
    Stmt& opt(int i, const std::optional<std::int64_t>& v) {
        if (!st_) return *this;
        if (v) sqlite3_bind_int64(st_, i, *v); else sqlite3_bind_null(st_, i);
        return *this;
    }
    Stmt& blob(int i, const unsigned char* data, std::size_t n) {
        if (st_) sqlite3_bind_blob(st_, i, data, static_cast<int>(n), SQLITE_TRANSIENT);
        return *this;
    }
    int step() { return st_ ? sqlite3_step(st_) : SQLITE_MISUSE; }
    // Runs a statement that returns no rows.
    bool exec() { return step() == SQLITE_DONE; }
    int  changes() const { return sqlite3_changes(db_); }

    std::string  str(int c) const {
        const auto* p = sqlite3_column_text(st_, c);
        return p ? std::string(reinterpret_cast<const char*>(p),
                               static_cast<std::size_t>(sqlite3_column_bytes(st_, c)))
                 : std::string();
    }
    std::int64_t num(int c) const { return sqlite3_column_int64(st_, c); }
    bool         isNull(int c) const { return sqlite3_column_type(st_, c) == SQLITE_NULL; }
    std::optional<std::int64_t> optNum(int c) const {
        return isNull(c) ? std::nullopt : std::optional<std::int64_t>(num(c));
    }
    const unsigned char* bytes(int c) const {
        return static_cast<const unsigned char*>(sqlite3_column_blob(st_, c));
    }
    int size(int c) const { return sqlite3_column_bytes(st_, c); }

private:
    sqlite3*      db_;
    sqlite3_stmt* st_{nullptr};
};

// BEGIN IMMEDIATE ... COMMIT, rolled back unless commit() was called.
class Tx {
public:
    explicit Tx(sqlite3* db) : db_(db) {
        char* err = nullptr;
        ok_ = sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, &err) == SQLITE_OK;
        if (!ok_) {
            LOG_ERROR("PlayersDb: BEGIN failed: {}", err ? err : "?");
            sqlite3_free(err);
        }
    }
    ~Tx() { if (ok_ && !done_) sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr); }
    Tx(const Tx&)            = delete;
    Tx& operator=(const Tx&) = delete;
    bool ok() const { return ok_; }
    bool commit() {
        if (!ok_ || done_) return false;
        done_ = true;
        return sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) == SQLITE_OK;
    }

private:
    sqlite3* db_;
    bool     ok_{false};
    bool     done_{false};
};

// ── identifiers and tokens ──────────────────────────────────────────────────
std::string newUuidV4() {
    unsigned char b[16];
    randombytes_buf(b, sizeof b);
    b[6] = static_cast<unsigned char>((b[6] & 0x0f) | 0x40);
    b[8] = static_cast<unsigned char>((b[8] & 0x3f) | 0x80);
    static const char* d = "0123456789abcdef";
    std::string s;
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) s.push_back('-');
        s.push_back(d[b[i] >> 4]);
        s.push_back(d[b[i] & 15]);
    }
    return s;
}

std::string newTokenHex() {
    unsigned char b[32];
    randombytes_buf(b, sizeof b);
    char hex[65];
    sodium_bin2hex(hex, sizeof hex, b, sizeof b);
    sodium_memzero(b, sizeof b);
    return hex;
}

bool isHexToken(const std::string& t) {
    return t.size() == 64 &&
           std::all_of(t.begin(), t.end(), [](unsigned char c) { return std::isxdigit(c) != 0; });
}

void hashToken(const std::string& token, unsigned char out[crypto_hash_sha256_BYTES]) {
    crypto_hash_sha256(out, reinterpret_cast<const unsigned char*>(token.data()), token.size());
}

std::optional<std::string> cleanName(const std::string& raw) {
    std::size_t a = 0, b = raw.size();
    while (a < b && raw[a] == ' ') ++a;
    while (b > a && raw[b - 1] == ' ') --b;
    std::string s = raw.substr(a, b - a);
    if (s.empty() || s.size() > PlayersDb::kMaxNameBytes) return std::nullopt;
    for (unsigned char c : s)
        if (c < 0x20 || c == 0x7f) return std::nullopt;
    return s;
}

PlayerRow readPlayer(const Stmt& s) {
    PlayerRow p;
    p.id            = s.str(0);
    p.name          = s.str(1);
    p.state         = parsePlayerState(s.str(2)).value_or(PlayerState::Revoked);
    p.clock_policy  = s.str(3);
    p.created_at    = s.num(4);
    p.created_by    = s.optNum(5);
    p.activated_at  = s.optNum(6);
    p.revoked_at    = s.optNum(7);
    p.revoke_reason = s.str(8);
    p.group_id      = s.optNum(9);
    return p;
}

constexpr const char* kPlayerCols =
    "id,name,state,clock_policy,created_at,created_by,activated_at,revoked_at,revoke_reason,group_id";

CertRow readCert(const Stmt& s) {
    CertRow c;
    c.generation    = static_cast<int>(s.num(0));
    c.serial_hex    = s.str(1);
    c.player_id     = s.str(2);
    c.issued_at     = s.num(3);
    c.not_before    = s.num(4);
    c.not_after     = s.num(5);
    c.revoked_at    = s.optNum(6);
    c.revoke_reason = s.str(7);
    return c;
}

constexpr const char* kCertCols =
    "generation,serial_hex,player_id,issued_at,not_before,not_after,revoked_at,revoke_reason";

StatusRow readStatus(const Stmt& s) {
    StatusRow r;
    r.player_id    = s.str(0);
    r.updated_at   = s.num(1);
    r.online       = s.num(2) != 0;
    r.version      = s.str(3);
    r.payload_json = s.str(4);
    return r;
}

bool execSql(sqlite3* db, const char* sql) {
    char* err = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        LOG_ERROR("PlayersDb: {} [{}]", err ? err : "?", sql);
        sqlite3_free(err);
        return false;
    }
    return true;
}

// Version 0 -> 1: the whole schema.
constexpr const char* kSchemaV1 = R"SQL(
CREATE TABLE players (
    id            TEXT PRIMARY KEY,
    name          TEXT NOT NULL,
    state         TEXT NOT NULL CHECK (state IN ('pending','active','revoked')),
    clock_policy  TEXT NOT NULL CHECK (clock_policy IN ('strict','relaxed')),
    created_at    INTEGER NOT NULL,
    created_by    INTEGER,
    activated_at  INTEGER,
    revoked_at    INTEGER,
    revoke_reason TEXT NOT NULL DEFAULT ''
);
CREATE INDEX idx_players_state ON players(state, created_at);

CREATE TABLE enroll_tokens (
    id         INTEGER PRIMARY KEY AUTOINCREMENT,
    player_id  TEXT NOT NULL REFERENCES players(id) ON DELETE CASCADE,
    token_hash BLOB NOT NULL,
    created_at INTEGER NOT NULL,
    expires_at INTEGER NOT NULL,
    used_at    INTEGER
);
CREATE INDEX idx_tokens_player ON enroll_tokens(player_id);

-- No foreign key to players: the record of a certificate must outlive the
-- player, or a revoked certificate would vanish from the CRL.
CREATE TABLE player_certs (
    generation    INTEGER NOT NULL,
    serial_hex    TEXT NOT NULL,
    player_id     TEXT NOT NULL,
    issued_at     INTEGER NOT NULL,
    not_before    INTEGER NOT NULL,
    not_after     INTEGER NOT NULL,
    revoked_at    INTEGER,
    revoke_reason TEXT NOT NULL DEFAULT '',
    PRIMARY KEY (generation, serial_hex)
);
CREATE INDEX idx_certs_player  ON player_certs(player_id);
CREATE INDEX idx_certs_revoked ON player_certs(generation, revoked_at) WHERE revoked_at IS NOT NULL;

CREATE TABLE player_status (
    player_id    TEXT PRIMARY KEY REFERENCES players(id) ON DELETE CASCADE,
    updated_at   INTEGER NOT NULL,
    online       INTEGER NOT NULL,
    version      TEXT NOT NULL DEFAULT '',
    payload_json TEXT NOT NULL DEFAULT ''
);

-- No foreign key either: the history stays until retention removes it.
CREATE TABLE player_events (
    id        INTEGER PRIMARY KEY AUTOINCREMENT,
    player_id TEXT NOT NULL,
    ts        INTEGER NOT NULL,
    kind      TEXT NOT NULL,
    details   TEXT NOT NULL DEFAULT ''
);
CREATE INDEX idx_events_player ON player_events(player_id, ts);
CREATE INDEX idx_events_ts     ON player_events(ts);

CREATE TABLE crl_state (
    generation  INTEGER PRIMARY KEY,
    last_number INTEGER NOT NULL
);
)SQL";

// Version 2 -> 3: the hierarchy all -> object -> group. AUTOINCREMENT, so the
// id of a removed object or group is never handed out again.
constexpr const char* kSchemaV3 = R"SQL(
CREATE TABLE player_objects (
    id         INTEGER PRIMARY KEY AUTOINCREMENT,
    name       TEXT    NOT NULL UNIQUE,
    created_at INTEGER NOT NULL
);
CREATE TABLE player_groups (
    id         INTEGER PRIMARY KEY AUTOINCREMENT,
    object_id  INTEGER NOT NULL REFERENCES player_objects(id),
    name       TEXT    NOT NULL,
    created_at INTEGER NOT NULL,
    UNIQUE(object_id, name)
);
ALTER TABLE players ADD COLUMN group_id INTEGER REFERENCES player_groups(id);
CREATE INDEX idx_players_group ON players(group_id);

-- Players that exist from before groups get a place; an empty installation
-- gets nothing invented.
INSERT INTO player_objects(name, created_at)
    SELECT 'Unassigned', CAST(strftime('%s','now') AS INTEGER) WHERE EXISTS (SELECT 1 FROM players);
INSERT INTO player_groups(object_id, name, created_at)
    SELECT id, 'Unassigned', CAST(strftime('%s','now') AS INTEGER) FROM player_objects WHERE name='Unassigned';
UPDATE players SET group_id = (SELECT g.id FROM player_groups g
                               JOIN player_objects o ON o.id = g.object_id
                               WHERE o.name='Unassigned' AND g.name='Unassigned')
    WHERE group_id IS NULL AND EXISTS (SELECT 1 FROM player_objects WHERE name='Unassigned');
)SQL";

// Version 1 -> 2: the module's settings.
constexpr const char* kSchemaV2 = R"SQL(
CREATE TABLE players_settings (
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL
);
)SQL";

// Version 3 -> 4: the log of commands sent to players.
constexpr const char* kSchemaV4 = R"SQL(
CREATE TABLE player_commands (
    id          TEXT PRIMARY KEY,
    player_id   TEXT NOT NULL,
    type        TEXT NOT NULL,
    state       TEXT NOT NULL,
    result      TEXT NOT NULL DEFAULT '',
    issued_at   INTEGER NOT NULL,
    ttl_seconds INTEGER NOT NULL,
    deadline_at INTEGER NOT NULL,
    issued_by   INTEGER,
    finished_at INTEGER
);
CREATE INDEX idx_commands_player ON player_commands(player_id, issued_at);
CREATE INDEX idx_commands_pending ON player_commands(deadline_at) WHERE state = 'pending';
-- One unsettled command per screen and type: the block against a second click.
CREATE UNIQUE INDEX ux_commands_pending ON player_commands(player_id, type) WHERE state = 'pending';
)SQL";

// Version 4 -> 5: jobs that send a command to many screens in batches.
constexpr const char* kSchemaV5 = R"SQL(
CREATE TABLE command_jobs (
    id              TEXT PRIMARY KEY,
    type            TEXT NOT NULL,
    target_kind     TEXT NOT NULL,
    target_id       INTEGER NOT NULL DEFAULT 0,
    target_name     TEXT NOT NULL,
    state           TEXT NOT NULL,
    stop_reason     TEXT NOT NULL DEFAULT '',
    batch_size      INTEGER NOT NULL,
    stop_percent    INTEGER NOT NULL,
    next_batch      INTEGER NOT NULL DEFAULT 0,
    created_at      INTEGER NOT NULL,
    created_by      INTEGER,
    created_by_name TEXT NOT NULL DEFAULT '',
    created_by_role TEXT NOT NULL DEFAULT '',
    finished_at     INTEGER
);
CREATE INDEX idx_jobs_state ON command_jobs(state, created_at);
CREATE TABLE command_job_items (
    job_id     TEXT NOT NULL REFERENCES command_jobs(id) ON DELETE CASCADE,
    player_id  TEXT NOT NULL,
    batch_no   INTEGER NOT NULL,
    state      TEXT NOT NULL,
    command_id TEXT NOT NULL DEFAULT '',
    error      TEXT NOT NULL DEFAULT '',
    PRIMARY KEY (job_id, player_id)
);
CREATE INDEX idx_job_items_batch ON command_job_items(job_id, batch_no, state);
)SQL";

}  // namespace

const char* toString(PlayerState s) noexcept {
    switch (s) {
        case PlayerState::Pending: return "pending";
        case PlayerState::Active:  return "active";
        case PlayerState::Revoked: return "revoked";
    }
    return "revoked";
}

std::optional<PlayerState> parsePlayerState(const std::string& s) noexcept {
    if (s == "pending") return PlayerState::Pending;
    if (s == "active")  return PlayerState::Active;
    if (s == "revoked") return PlayerState::Revoked;
    return std::nullopt;
}

// ─── lifecycle ──────────────────────────────────────────────────────────────
PlayersDb::PlayersDb(fs::path path) : path_(std::move(path)) {}

PlayersDb::~PlayersDb() {
    if (db_) sqlite3_close(db_);
}

bool PlayersDb::open() {
    std::lock_guard<std::mutex> lk(mu_);
    if (db_) return true;
    if (!sodiumReady()) return false;

    std::error_code ec;
    if (path_.has_parent_path()) fs::create_directories(path_.parent_path(), ec);
    // Created private from the start: token hashes and certificate records.
    if (!fs::exists(path_, ec)) {
        const int fd = ::open(path_.c_str(), O_CREAT | O_WRONLY | O_CLOEXEC, 0600);
        if (fd >= 0) ::close(fd);
    }

    sqlite3* raw = nullptr;
    if (sqlite3_open_v2(path_.c_str(), &raw, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                        nullptr) != SQLITE_OK) {
        LOG_ERROR("PlayersDb: cannot open {}: {}", path_.string(), raw ? sqlite3_errmsg(raw) : "?");
        if (raw) sqlite3_close(raw);
        return false;
    }
    sqlite3_busy_timeout(raw, 5000);

    // Read the version BEFORE any pragma that writes: a database from a newer
    // version must be left exactly as it is.
    int version = 0;
    {
        Stmt s(raw, "PRAGMA user_version;");
        if (s.step() == SQLITE_ROW) version = static_cast<int>(s.num(0));
    }
    if (version > kSchemaVersion) {
        LOG_ERROR("PlayersDb: {} has schema version {}, this build understands up to {}; "
                  "refusing to open it", path_.string(), version, kSchemaVersion);
        sqlite3_close(raw);
        return false;
    }

    if (!execSql(raw, "PRAGMA journal_mode=WAL;") ||
        !execSql(raw, "PRAGMA synchronous=NORMAL;") ||
        !execSql(raw, "PRAGMA foreign_keys=ON;")) {
        sqlite3_close(raw);
        return false;
    }

    if (version < kSchemaVersion) {
        Tx tx(raw);
        if (!tx.ok()) { sqlite3_close(raw); return false; }
        bool ok = true;
        if (version < 1) ok = ok && execSql(raw, kSchemaV1);
        if (version < 2) ok = ok && execSql(raw, kSchemaV2);
        if (version < 3) ok = ok && execSql(raw, kSchemaV3);
        if (version < 4) ok = ok && execSql(raw, kSchemaV4);
        if (version < 5) ok = ok && execSql(raw, kSchemaV5);
        // Future versions: add `if (version < 4) ...` steps here, in order.
        ok = ok && execSql(raw, ("PRAGMA user_version = " + std::to_string(kSchemaVersion) + ";").c_str());
        if (!ok || !tx.commit()) {
            LOG_ERROR("PlayersDb: migration {} -> {} failed", version, kSchemaVersion);
            sqlite3_close(raw);
            return false;
        }
    }
    db_ = raw;
    return true;
}

int PlayersDb::schemaVersion() {
    std::lock_guard<std::mutex> lk(mu_);
    if (!db_) return 0;
    Stmt s(db_, "PRAGMA user_version;");
    return s.step() == SQLITE_ROW ? static_cast<int>(s.num(0)) : 0;
}

// ─── players ────────────────────────────────────────────────────────────────
std::optional<CreatedPlayer> PlayersDb::createPlayer(const std::string& name,
                                                     std::optional<std::int64_t> created_by,
                                                     const std::string& clock_policy,
                                                     std::int64_t token_ttl_seconds,
                                                     std::int64_t now,
                                                     std::optional<std::int64_t> group_id) {
    auto clean = cleanName(name);
    if (!clean) return std::nullopt;
    if (clock_policy != "strict" && clock_policy != "relaxed") return std::nullopt;
    if (token_ttl_seconds < 1 || token_ttl_seconds > kMaxTokenTtlSeconds) return std::nullopt;

    std::lock_guard<std::mutex> lk(mu_);
    if (!db_) return std::nullopt;
    const std::int64_t t = nowOr(now);

    CreatedPlayer out;
    out.player.id           = newUuidV4();
    out.player.name         = *clean;
    out.player.state        = PlayerState::Pending;
    out.player.clock_policy = clock_policy;
    out.player.created_at   = t;
    out.player.created_by   = created_by;
    out.token               = newTokenHex();
    out.token_expires_at    = t + token_ttl_seconds;

    unsigned char hash[crypto_hash_sha256_BYTES];
    hashToken(out.token, hash);

    Tx tx(db_);
    if (!tx.ok()) return std::nullopt;
    if (group_id) {
        Stmt s(db_, "SELECT 1 FROM player_groups WHERE id=?");
        s.i64(1, *group_id);
        if (s.step() != SQLITE_ROW) return std::nullopt;
    }
    out.player.group_id = group_id;
    {
        Stmt s(db_, "INSERT INTO players(id,name,state,clock_policy,created_at,created_by,group_id) "
                    "VALUES(?,?,'pending',?,?,?,?)");
        s.text(1, out.player.id).text(2, out.player.name).text(3, clock_policy)
         .i64(4, t).opt(5, created_by).opt(6, group_id);
        if (!s.exec()) return std::nullopt;
    }
    {
        Stmt s(db_, "INSERT INTO enroll_tokens(player_id,token_hash,created_at,expires_at) "
                    "VALUES(?,?,?,?)");
        s.text(1, out.player.id).blob(2, hash, sizeof hash).i64(3, t).i64(4, out.token_expires_at);
        if (!s.exec()) return std::nullopt;
    }
    if (!tx.commit()) return std::nullopt;
    return out;
}

std::optional<PlayerRow> PlayersDb::findPlayer(const std::string& id) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!db_) return std::nullopt;
    Stmt s(db_, (std::string("SELECT ") + kPlayerCols + " FROM players WHERE id=?").c_str());
    s.text(1, id);
    if (s.step() != SQLITE_ROW) return std::nullopt;
    return readPlayer(s);
}

std::vector<PlayerRow> PlayersDb::listPlayers(const ListFilter& f) {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<PlayerRow> out;
    if (!db_) return out;
    std::string sql = std::string("SELECT ") + kPlayerCols + " FROM players";
    std::string where;
    auto add = [&](const char* cond) { where += where.empty() ? " WHERE " : " AND "; where += cond; };
    if (f.state)     add("state=?");
    if (f.group_id)  add("group_id=?");
    if (f.object_id) add("group_id IN (SELECT id FROM player_groups WHERE object_id=?)");
    sql += where;
    sql += " ORDER BY created_at, id";
    if (f.limit > 0) sql += " LIMIT ? OFFSET ?";
    Stmt s(db_, sql.c_str());
    int i = 1;
    if (f.state)     s.text(i++, toString(*f.state));
    if (f.group_id)  s.i64(i++, *f.group_id);
    if (f.object_id) s.i64(i++, *f.object_id);
    if (f.limit > 0) { s.i64(i++, f.limit); s.i64(i++, std::max<std::int64_t>(f.offset, 0)); }
    while (s.step() == SQLITE_ROW) out.push_back(readPlayer(s));
    return out;
}

bool PlayersDb::deletePlayer(const std::string& id, const std::string& reason, std::int64_t now) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!db_) return false;
    const std::int64_t t = nowOr(now);
    Tx tx(db_);
    if (!tx.ok()) return false;
    {
        Stmt s(db_, "SELECT 1 FROM players WHERE id=?");
        s.text(1, id);
        if (s.step() != SQLITE_ROW) return false;
    }
    {   // Live certificates are revoked, not forgotten: the CRL needs them.
        Stmt s(db_, "UPDATE player_certs SET revoked_at=?, revoke_reason=? "
                    "WHERE player_id=? AND revoked_at IS NULL");
        s.i64(1, t).text(2, reason).text(3, id);
        if (!s.exec()) return false;
    }
    {
        Stmt s(db_, "DELETE FROM players WHERE id=?");   // tokens and status cascade
        s.text(1, id);
        if (!s.exec()) return false;
    }
    {
        Stmt s(db_, "INSERT INTO player_events(player_id,ts,kind,details) VALUES(?,?,'deleted',?)");
        s.text(1, id).i64(2, t).text(3, reason);
        if (!s.exec()) return false;
    }
    return tx.commit();
}

std::optional<std::vector<CertRow>> PlayersDb::revokePlayer(const std::string& id,
                                                            const std::string& reason,
                                                            std::int64_t now) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!db_) return std::nullopt;
    const std::int64_t t = nowOr(now);
    Tx tx(db_);
    if (!tx.ok()) return std::nullopt;

    PlayerState state;
    {
        Stmt s(db_, "SELECT state FROM players WHERE id=?");
        s.text(1, id);
        if (s.step() != SQLITE_ROW) return std::nullopt;
        state = parsePlayerState(s.str(0)).value_or(PlayerState::Revoked);
    }
    std::vector<CertRow> revoked;
    if (state == PlayerState::Revoked) {
        if (!tx.commit()) return std::nullopt;
        return revoked;   // already done; the first reason stays
    }
    {
        Stmt s(db_, (std::string("SELECT ") + kCertCols +
                     " FROM player_certs WHERE player_id=? AND revoked_at IS NULL "
                     "ORDER BY issued_at, serial_hex").c_str());
        s.text(1, id);
        while (s.step() == SQLITE_ROW) {
            CertRow c = readCert(s);
            c.revoked_at = t;
            c.revoke_reason = reason;
            revoked.push_back(std::move(c));
        }
    }
    {
        Stmt s(db_, "UPDATE player_certs SET revoked_at=?, revoke_reason=? "
                    "WHERE player_id=? AND revoked_at IS NULL");
        s.i64(1, t).text(2, reason).text(3, id);
        if (!s.exec()) return std::nullopt;
    }
    {
        Stmt s(db_, "UPDATE players SET state='revoked', revoked_at=?, revoke_reason=? WHERE id=?");
        s.i64(1, t).text(2, reason).text(3, id);
        if (!s.exec()) return std::nullopt;
    }
    {   // A revoked player's pairing tokens are worthless.
        Stmt s(db_, "DELETE FROM enroll_tokens WHERE player_id=? AND used_at IS NULL");
        s.text(1, id);
        if (!s.exec()) return std::nullopt;
    }
    {
        Stmt s(db_, "INSERT INTO player_events(player_id,ts,kind,details) VALUES(?,?,'revoked',?)");
        s.text(1, id).i64(2, t).text(3, reason);
        if (!s.exec()) return std::nullopt;
    }
    if (!tx.commit()) return std::nullopt;
    return revoked;
}

// ─── pairing ────────────────────────────────────────────────────────────────
std::optional<ReEnrollment> PlayersDb::reEnrollPlayer(const std::string& id, const std::string& reason,
                                                      std::int64_t token_ttl_seconds, std::int64_t now) {
    if (token_ttl_seconds < 1 || token_ttl_seconds > kMaxTokenTtlSeconds) return std::nullopt;
    std::lock_guard<std::mutex> lk(mu_);
    if (!db_) return std::nullopt;
    const std::int64_t t = nowOr(now);
    Tx tx(db_);
    if (!tx.ok()) return std::nullopt;

    ReEnrollment out;
    {
        Stmt s(db_, "SELECT state FROM players WHERE id=?");
        s.text(1, id);
        if (s.step() != SQLITE_ROW) return std::nullopt;
        out.previous = parsePlayerState(s.str(0)).value_or(PlayerState::Revoked);
        if (out.previous == PlayerState::Pending) return std::nullopt;
    }
    {   // Certificates still alive: they stop working now. Already revoked ones keep their first reason.
        Stmt s(db_, (std::string("SELECT ") + kCertCols +
                     " FROM player_certs WHERE player_id=? AND revoked_at IS NULL "
                     "ORDER BY issued_at, serial_hex").c_str());
        s.text(1, id);
        while (s.step() == SQLITE_ROW) {
            CertRow c = readCert(s);
            c.revoked_at = t;
            c.revoke_reason = reason;
            out.revoked.push_back(std::move(c));
        }
    }
    {
        Stmt s(db_, "UPDATE player_certs SET revoked_at=?, revoke_reason=? WHERE player_id=? AND revoked_at IS NULL");
        s.i64(1, t).text(2, reason).text(3, id);
        if (!s.exec()) return std::nullopt;
    }
    {
        Stmt s(db_, "UPDATE players SET state='pending', activated_at=NULL, revoked_at=NULL, revoke_reason='' WHERE id=?");
        s.text(1, id);
        if (!s.exec() || s.changes() != 1) return std::nullopt;
    }
    {   // What the old device last said is not a fact about the screen that will be bound next.
        Stmt s(db_, "DELETE FROM player_status WHERE player_id=?");
        s.text(1, id);
        if (!s.exec()) return std::nullopt;
    }
    {   // A command waiting for the old device would block the same command to the new one.
        Stmt s(db_, "UPDATE player_commands SET state='released', result='re_enrolled', finished_at=? "
                    "WHERE player_id=? AND state='pending'");
        s.i64(1, t).text(2, id);
        if (!s.exec()) return std::nullopt;
    }
    out.token.token = newTokenHex();
    out.token.token_expires_at = t + token_ttl_seconds;
    unsigned char hash[crypto_hash_sha256_BYTES];
    hashToken(out.token.token, hash);
    {
        Stmt s(db_, "DELETE FROM enroll_tokens WHERE player_id=?");
        s.text(1, id);
        if (!s.exec()) return std::nullopt;
    }
    {
        Stmt s(db_, "INSERT INTO enroll_tokens(player_id,token_hash,created_at,expires_at) VALUES(?,?,?,?)");
        s.text(1, id).blob(2, hash, sizeof hash).i64(3, t).i64(4, out.token.token_expires_at);
        if (!s.exec()) return std::nullopt;
    }
    {
        Stmt s(db_, "INSERT INTO player_events(player_id,ts,kind,details) VALUES(?,?,'re_enrolled',?)");
        s.text(1, id).i64(2, t).text(3, nlohmann::json{{"reason", reason},
                                                       {"previous_state", toString(out.previous)},
                                                       {"revoked_certs", out.revoked.size()}}.dump());
        if (!s.exec()) return std::nullopt;
    }
    if (!tx.commit()) return std::nullopt;
    return out;
}

std::optional<TokenIssue> PlayersDb::reissueToken(const std::string& player_id,
                                                  std::int64_t token_ttl_seconds,
                                                  std::int64_t now) {
    if (token_ttl_seconds < 1 || token_ttl_seconds > kMaxTokenTtlSeconds) return std::nullopt;
    std::lock_guard<std::mutex> lk(mu_);
    if (!db_) return std::nullopt;
    const std::int64_t t = nowOr(now);
    Tx tx(db_);
    if (!tx.ok()) return std::nullopt;
    {
        Stmt s(db_, "SELECT state FROM players WHERE id=?");
        s.text(1, player_id);
        if (s.step() != SQLITE_ROW || s.str(0) != "pending") return std::nullopt;
    }
    TokenIssue out;
    out.token = newTokenHex();
    out.token_expires_at = t + token_ttl_seconds;
    unsigned char hash[crypto_hash_sha256_BYTES];
    hashToken(out.token, hash);
    {
        Stmt s(db_, "DELETE FROM enroll_tokens WHERE player_id=?");
        s.text(1, player_id);
        if (!s.exec()) return std::nullopt;
    }
    {
        Stmt s(db_, "INSERT INTO enroll_tokens(player_id,token_hash,created_at,expires_at) VALUES(?,?,?,?)");
        s.text(1, player_id).blob(2, hash, sizeof hash).i64(3, t).i64(4, out.token_expires_at);
        if (!s.exec()) return std::nullopt;
    }
    if (!tx.commit()) return std::nullopt;
    return out;
}

ClaimResult PlayersDb::claimToken(const std::string& player_id, const std::string& token,
                                  std::int64_t now) {
    ClaimResult res;
    std::lock_guard<std::mutex> lk(mu_);
    if (!db_) { res.reason = ClaimFailure::NoMatch; return res; }
    const std::int64_t t = nowOr(now);
    Tx tx(db_);
    if (!tx.ok()) { res.reason = ClaimFailure::NoMatch; return res; }

    PlayerState state;
    {
        Stmt s(db_, "SELECT state FROM players WHERE id=?");
        s.text(1, player_id);
        if (s.step() != SQLITE_ROW) { res.reason = ClaimFailure::UnknownPlayer; return res; }
        state = parsePlayerState(s.str(0)).value_or(PlayerState::Revoked);
    }

    // Compare against EVERY token of this player without stopping at the first
    // match, so the time taken does not depend on which one (or none) matched.
    const bool well_formed = isHexToken(token);
    unsigned char want[crypto_hash_sha256_BYTES] = {0};
    if (well_formed) hashToken(token, want);

    std::int64_t match_id = 0, match_expires = 0;
    bool match_used = false;
    {
        Stmt s(db_, "SELECT id,token_hash,expires_at,used_at FROM enroll_tokens WHERE player_id=?");
        s.text(1, player_id);
        while (s.step() == SQLITE_ROW) {
            const bool same = well_formed && s.size(1) == static_cast<int>(sizeof want) &&
                              sodium_memcmp(want, s.bytes(1), sizeof want) == 0;
            if (same) {
                match_id = s.num(0);
                match_expires = s.num(2);
                match_used = !s.isNull(3);
            }
        }
    }
    sodium_memzero(want, sizeof want);

    if (match_id == 0)          { res.reason = ClaimFailure::NoMatch;     return res; }
    if (match_used)             { res.reason = ClaimFailure::AlreadyUsed; return res; }
    if (t > match_expires)      { res.reason = ClaimFailure::Expired;     return res; }
    if (state != PlayerState::Pending) { res.reason = ClaimFailure::WrongState; return res; }

    {
        Stmt s(db_, "UPDATE enroll_tokens SET used_at=? WHERE id=? AND used_at IS NULL");
        s.i64(1, t).i64(2, match_id);
        if (!s.exec() || s.changes() != 1) { res.reason = ClaimFailure::AlreadyUsed; return res; }
    }
    if (!tx.commit()) { res.reason = ClaimFailure::NoMatch; return res; }
    res.ok = true;
    res.token_id = match_id;
    return res;
}

bool PlayersDb::releaseClaim(std::int64_t token_id) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!db_) return false;
    Stmt s(db_, "UPDATE enroll_tokens SET used_at=NULL WHERE id=? AND used_at IS NOT NULL "
                "AND player_id IN (SELECT id FROM players WHERE state='pending')");
    s.i64(1, token_id);
    return s.exec() && s.changes() == 1;
}

bool PlayersDb::finishEnrollment(std::int64_t token_id, const CertRow& cert, std::int64_t now) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!db_) return false;
    const std::int64_t t = nowOr(now);
    Tx tx(db_);
    if (!tx.ok()) return false;
    {   // The claim must be this player's, and still held.
        Stmt s(db_, "SELECT t.player_id, p.state FROM enroll_tokens t "
                    "JOIN players p ON p.id = t.player_id "
                    "WHERE t.id=? AND t.used_at IS NOT NULL");
        s.i64(1, token_id);
        if (s.step() != SQLITE_ROW) return false;
        if (s.str(0) != cert.player_id || s.str(1) != "pending") return false;
    }
    {
        Stmt s(db_, "INSERT INTO player_certs(generation,serial_hex,player_id,issued_at,not_before,not_after) "
                    "VALUES(?,?,?,?,?,?)");
        s.i64(1, cert.generation).text(2, cert.serial_hex).text(3, cert.player_id)
         .i64(4, cert.issued_at != 0 ? cert.issued_at : t).i64(5, cert.not_before).i64(6, cert.not_after);
        if (!s.exec()) return false;   // duplicate serial in a generation: rolled back
    }
    {
        Stmt s(db_, "UPDATE players SET state='active', activated_at=? WHERE id=? AND state='pending'");
        s.i64(1, t).text(2, cert.player_id);
        if (!s.exec() || s.changes() != 1) return false;
    }
    {
        Stmt s(db_, "INSERT INTO player_events(player_id,ts,kind,details) VALUES(?,?,'enrolled',?)");
        s.text(1, cert.player_id).i64(2, t).text(3, "generation " + std::to_string(cert.generation));
        if (!s.exec()) return false;
    }
    return tx.commit();
}

bool PlayersDb::recordRenewal(const CertRow& cert, std::int64_t now) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!db_) return false;
    const std::int64_t t = nowOr(now);
    Tx tx(db_);
    if (!tx.ok()) return false;
    {
        Stmt s(db_, "SELECT state FROM players WHERE id=?");
        s.text(1, cert.player_id);
        if (s.step() != SQLITE_ROW || s.str(0) != "active") return false;
    }
    {
        Stmt s(db_, "INSERT INTO player_certs(generation,serial_hex,player_id,issued_at,not_before,not_after) "
                    "VALUES(?,?,?,?,?,?)");
        s.i64(1, cert.generation).text(2, cert.serial_hex).text(3, cert.player_id)
         .i64(4, cert.issued_at != 0 ? cert.issued_at : t).i64(5, cert.not_before).i64(6, cert.not_after);
        if (!s.exec()) return false;
    }
    {
        Stmt s(db_, "INSERT INTO player_events(player_id,ts,kind,details) VALUES(?,?,'renewed',?)");
        s.text(1, cert.player_id).i64(2, t).text(3, "generation " + std::to_string(cert.generation));
        if (!s.exec()) return false;
    }
    return tx.commit();
}

// ─── certificates and CRLs ──────────────────────────────────────────────────
std::vector<CertRow> PlayersDb::certsOf(const std::string& player_id) {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<CertRow> out;
    if (!db_) return out;
    Stmt s(db_, (std::string("SELECT ") + kCertCols +
                 " FROM player_certs WHERE player_id=? ORDER BY issued_at, serial_hex").c_str());
    s.text(1, player_id);
    while (s.step() == SQLITE_ROW) out.push_back(readCert(s));
    return out;
}

bool PlayersDb::revokeCert(int generation, const std::string& serial_hex,
                           const std::string& reason, std::int64_t now) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!db_) return false;
    Stmt s(db_, "UPDATE player_certs SET revoked_at=?, revoke_reason=? "
                "WHERE generation=? AND serial_hex=? AND revoked_at IS NULL");
    s.i64(1, nowOr(now)).text(2, reason).i64(3, generation).text(4, serial_hex);
    return s.exec() && s.changes() == 1;
}

std::vector<CertRow> PlayersDb::revokedSerials(int generation, std::int64_t now) {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<CertRow> out;
    if (!db_) return out;
    Stmt s(db_, (std::string("SELECT ") + kCertCols +
                 " FROM player_certs WHERE generation=? AND revoked_at IS NOT NULL AND not_after>=? "
                 "ORDER BY revoked_at, serial_hex").c_str());
    s.i64(1, generation).i64(2, nowOr(now));
    while (s.step() == SQLITE_ROW) out.push_back(readCert(s));
    return out;
}

std::vector<CertRow> PlayersDb::revokedCerts(std::int64_t now) {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<CertRow> out;
    if (!db_) return out;
    Stmt s(db_, (std::string("SELECT ") + kCertCols +
                 " FROM player_certs WHERE revoked_at IS NOT NULL AND not_after>=? "
                 "ORDER BY generation, serial_hex").c_str());
    s.i64(1, nowOr(now));
    while (s.step() == SQLITE_ROW) out.push_back(readCert(s));
    return out;
}

std::int64_t PlayersDb::nextCrlNumber(int generation) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!db_) return 0;
    Tx tx(db_);
    if (!tx.ok()) return 0;
    {
        Stmt s(db_, "INSERT OR IGNORE INTO crl_state(generation,last_number) VALUES(?,0)");
        s.i64(1, generation);
        if (!s.exec()) return 0;
    }
    {
        Stmt s(db_, "UPDATE crl_state SET last_number=last_number+1 WHERE generation=?");
        s.i64(1, generation);
        if (!s.exec()) return 0;
    }
    std::int64_t n = 0;
    {
        Stmt s(db_, "SELECT last_number FROM crl_state WHERE generation=?");
        s.i64(1, generation);
        if (s.step() != SQLITE_ROW) return 0;
        n = s.num(0);
    }
    return tx.commit() ? n : 0;
}

std::size_t PlayersDb::purgeExpiredCerts(std::int64_t now) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!db_) return 0;
    Stmt s(db_, "DELETE FROM player_certs WHERE not_after < ?");
    s.i64(1, nowOr(now));
    return s.exec() ? static_cast<std::size_t>(s.changes()) : 0;
}

// ─── objects and groups ─────────────────────────────────────────────────────
namespace {
void setResult(GroupResult* out, GroupResult r) { if (out) *out = r; }
ObjectRow readObject(const Stmt& s) { return {s.num(0), s.str(1), s.num(2)}; }
GroupRow  readGroup(const Stmt& s)  { return {s.num(0), s.num(1), s.str(2), s.num(3)}; }
}  // namespace

std::optional<ObjectRow> PlayersDb::createObject(const std::string& name, GroupResult* result,
                                                 std::int64_t now) {
    auto clean = cleanName(name);
    if (!clean) { setResult(result, GroupResult::InvalidName); return std::nullopt; }
    std::lock_guard<std::mutex> lk(mu_);
    if (!db_) { setResult(result, GroupResult::Storage); return std::nullopt; }
    Tx tx(db_);
    if (!tx.ok()) { setResult(result, GroupResult::Storage); return std::nullopt; }
    {
        Stmt s(db_, "SELECT 1 FROM player_objects WHERE name=?");
        s.text(1, *clean);
        if (s.step() == SQLITE_ROW) { setResult(result, GroupResult::NameTaken); return std::nullopt; }
    }
    const std::int64_t t = nowOr(now);
    {
        Stmt s(db_, "INSERT INTO player_objects(name,created_at) VALUES(?,?)");
        s.text(1, *clean).i64(2, t);
        if (!s.exec()) { setResult(result, GroupResult::Storage); return std::nullopt; }
    }
    const std::int64_t id = sqlite3_last_insert_rowid(db_);
    if (!tx.commit()) { setResult(result, GroupResult::Storage); return std::nullopt; }
    setResult(result, GroupResult::Ok);
    return ObjectRow{id, *clean, t};
}

GroupResult PlayersDb::renameObject(std::int64_t id, const std::string& name) {
    auto clean = cleanName(name);
    if (!clean) return GroupResult::InvalidName;
    std::lock_guard<std::mutex> lk(mu_);
    if (!db_) return GroupResult::Storage;
    Tx tx(db_);
    if (!tx.ok()) return GroupResult::Storage;
    {
        Stmt s(db_, "SELECT 1 FROM player_objects WHERE id=?");
        s.i64(1, id);
        if (s.step() != SQLITE_ROW) return GroupResult::NotFound;
    }
    {
        Stmt s(db_, "SELECT 1 FROM player_objects WHERE name=? AND id<>?");
        s.text(1, *clean).i64(2, id);
        if (s.step() == SQLITE_ROW) return GroupResult::NameTaken;
    }
    {
        Stmt s(db_, "UPDATE player_objects SET name=? WHERE id=?");
        s.text(1, *clean).i64(2, id);
        if (!s.exec()) return GroupResult::Storage;
    }
    return tx.commit() ? GroupResult::Ok : GroupResult::Storage;
}

GroupResult PlayersDb::deleteObject(std::int64_t id) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!db_) return GroupResult::Storage;
    Tx tx(db_);
    if (!tx.ok()) return GroupResult::Storage;
    {
        Stmt s(db_, "SELECT 1 FROM player_objects WHERE id=?");
        s.i64(1, id);
        if (s.step() != SQLITE_ROW) return GroupResult::NotFound;
    }
    {
        Stmt s(db_, "SELECT 1 FROM player_groups WHERE object_id=?");
        s.i64(1, id);
        if (s.step() == SQLITE_ROW) return GroupResult::NotEmpty;
    }
    {
        Stmt s(db_, "DELETE FROM player_objects WHERE id=?");
        s.i64(1, id);
        if (!s.exec()) return GroupResult::Storage;
    }
    return tx.commit() ? GroupResult::Ok : GroupResult::Storage;
}

std::optional<ObjectRow> PlayersDb::findObject(std::int64_t id) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!db_) return std::nullopt;
    Stmt s(db_, "SELECT id,name,created_at FROM player_objects WHERE id=?");
    s.i64(1, id);
    if (s.step() != SQLITE_ROW) return std::nullopt;
    return readObject(s);
}

std::vector<ObjectRow> PlayersDb::listObjects() {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<ObjectRow> out;
    if (!db_) return out;
    Stmt s(db_, "SELECT id,name,created_at FROM player_objects ORDER BY name, id");
    while (s.step() == SQLITE_ROW) out.push_back(readObject(s));
    return out;
}

std::optional<GroupRow> PlayersDb::createGroup(std::int64_t object_id, const std::string& name,
                                               GroupResult* result, std::int64_t now) {
    auto clean = cleanName(name);
    if (!clean) { setResult(result, GroupResult::InvalidName); return std::nullopt; }
    std::lock_guard<std::mutex> lk(mu_);
    if (!db_) { setResult(result, GroupResult::Storage); return std::nullopt; }
    Tx tx(db_);
    if (!tx.ok()) { setResult(result, GroupResult::Storage); return std::nullopt; }
    {
        Stmt s(db_, "SELECT 1 FROM player_objects WHERE id=?");
        s.i64(1, object_id);
        if (s.step() != SQLITE_ROW) { setResult(result, GroupResult::NotFound); return std::nullopt; }
    }
    {
        Stmt s(db_, "SELECT 1 FROM player_groups WHERE object_id=? AND name=?");
        s.i64(1, object_id).text(2, *clean);
        if (s.step() == SQLITE_ROW) { setResult(result, GroupResult::NameTaken); return std::nullopt; }
    }
    const std::int64_t t = nowOr(now);
    {
        Stmt s(db_, "INSERT INTO player_groups(object_id,name,created_at) VALUES(?,?,?)");
        s.i64(1, object_id).text(2, *clean).i64(3, t);
        if (!s.exec()) { setResult(result, GroupResult::Storage); return std::nullopt; }
    }
    const std::int64_t id = sqlite3_last_insert_rowid(db_);
    if (!tx.commit()) { setResult(result, GroupResult::Storage); return std::nullopt; }
    setResult(result, GroupResult::Ok);
    return GroupRow{id, object_id, *clean, t};
}

GroupResult PlayersDb::renameGroup(std::int64_t id, const std::string& name) {
    auto clean = cleanName(name);
    if (!clean) return GroupResult::InvalidName;
    std::lock_guard<std::mutex> lk(mu_);
    if (!db_) return GroupResult::Storage;
    Tx tx(db_);
    if (!tx.ok()) return GroupResult::Storage;
    std::int64_t object_id = 0;
    {
        Stmt s(db_, "SELECT object_id FROM player_groups WHERE id=?");
        s.i64(1, id);
        if (s.step() != SQLITE_ROW) return GroupResult::NotFound;
        object_id = s.num(0);
    }
    {
        Stmt s(db_, "SELECT 1 FROM player_groups WHERE object_id=? AND name=? AND id<>?");
        s.i64(1, object_id).text(2, *clean).i64(3, id);
        if (s.step() == SQLITE_ROW) return GroupResult::NameTaken;
    }
    {
        Stmt s(db_, "UPDATE player_groups SET name=? WHERE id=?");
        s.text(1, *clean).i64(2, id);
        if (!s.exec()) return GroupResult::Storage;
    }
    return tx.commit() ? GroupResult::Ok : GroupResult::Storage;
}

GroupResult PlayersDb::deleteGroup(std::int64_t id) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!db_) return GroupResult::Storage;
    Tx tx(db_);
    if (!tx.ok()) return GroupResult::Storage;
    {
        Stmt s(db_, "SELECT 1 FROM player_groups WHERE id=?");
        s.i64(1, id);
        if (s.step() != SQLITE_ROW) return GroupResult::NotFound;
    }
    {
        Stmt s(db_, "SELECT 1 FROM players WHERE group_id=?");
        s.i64(1, id);
        if (s.step() == SQLITE_ROW) return GroupResult::NotEmpty;
    }
    {
        Stmt s(db_, "DELETE FROM player_groups WHERE id=?");
        s.i64(1, id);
        if (!s.exec()) return GroupResult::Storage;
    }
    return tx.commit() ? GroupResult::Ok : GroupResult::Storage;
}

std::optional<GroupRow> PlayersDb::findGroup(std::int64_t id) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!db_) return std::nullopt;
    Stmt s(db_, "SELECT id,object_id,name,created_at FROM player_groups WHERE id=?");
    s.i64(1, id);
    if (s.step() != SQLITE_ROW) return std::nullopt;
    return readGroup(s);
}

std::vector<GroupRow> PlayersDb::listGroups(std::optional<std::int64_t> object_id) {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<GroupRow> out;
    if (!db_) return out;
    Stmt s(db_, object_id ? "SELECT id,object_id,name,created_at FROM player_groups WHERE object_id=? ORDER BY name, id"
                          : "SELECT id,object_id,name,created_at FROM player_groups ORDER BY object_id, name, id");
    if (object_id) s.i64(1, *object_id);
    while (s.step() == SQLITE_ROW) out.push_back(readGroup(s));
    return out;
}

GroupResult PlayersDb::movePlayer(const std::string& player_id, std::int64_t group_id, std::int64_t now) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!db_) return GroupResult::Storage;
    Tx tx(db_);
    if (!tx.ok()) return GroupResult::Storage;
    std::optional<std::int64_t> from;
    {
        Stmt s(db_, "SELECT group_id FROM players WHERE id=?");
        s.text(1, player_id);
        if (s.step() != SQLITE_ROW) return GroupResult::NotFound;
        from = s.optNum(0);
    }
    {
        Stmt s(db_, "SELECT 1 FROM player_groups WHERE id=?");
        s.i64(1, group_id);
        if (s.step() != SQLITE_ROW) return GroupResult::NotFound;
    }
    if (from && *from == group_id) return GroupResult::Ok;           // nothing to do, nothing to record
    {
        Stmt s(db_, "UPDATE players SET group_id=? WHERE id=?");
        s.i64(1, group_id).text(2, player_id);
        if (!s.exec()) return GroupResult::Storage;
    }
    {
        Stmt s(db_, "INSERT INTO player_events(player_id,ts,kind,details) VALUES(?,?,'moved',?)");
        s.text(1, player_id).i64(2, nowOr(now))
         .text(3, "{\"from\":" + (from ? std::to_string(*from) : std::string("null")) +
                  ",\"to\":" + std::to_string(group_id) + "}");
        if (!s.exec()) return GroupResult::Storage;
    }
    return tx.commit() ? GroupResult::Ok : GroupResult::Storage;
}

std::optional<PlayerPlace> PlayersDb::placeOfPlayer(const std::string& player_id) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!db_) return std::nullopt;
    Stmt s(db_, "SELECT g.id, g.object_id FROM players p JOIN player_groups g ON g.id = p.group_id WHERE p.id=?");
    s.text(1, player_id);
    if (s.step() != SQLITE_ROW) return std::nullopt;
    return PlayerPlace{s.num(0), s.num(1)};
}

std::map<std::int64_t, std::int64_t> PlayersDb::playerCountByGroup() {
    std::lock_guard<std::mutex> lk(mu_);
    std::map<std::int64_t, std::int64_t> out;
    if (!db_) return out;
    Stmt s(db_, "SELECT group_id, COUNT(*) FROM players WHERE group_id IS NOT NULL GROUP BY group_id");
    while (s.step() == SQLITE_ROW) out[s.num(0)] = s.num(1);
    return out;
}

// ─── settings ───────────────────────────────────────────────────────────────
std::map<std::string, std::string> PlayersDb::settings() {
    std::lock_guard<std::mutex> lk(mu_);
    std::map<std::string, std::string> out;
    if (!db_) return out;
    Stmt s(db_, "SELECT key,value FROM players_settings");
    while (s.step() == SQLITE_ROW) out[s.str(0)] = s.str(1);
    return out;
}

bool PlayersDb::setSettings(const std::map<std::string, std::string>& values) {
    for (const auto& [k, v] : values)
        if (k.empty() || k.size() > 128 || v.size() > kMaxSettingBytes) return false;
    std::lock_guard<std::mutex> lk(mu_);
    if (!db_) return false;
    Tx tx(db_);
    if (!tx.ok()) return false;
    for (const auto& [k, v] : values) {
        if (v.empty()) {
            Stmt s(db_, "DELETE FROM players_settings WHERE key=?");
            s.text(1, k);
            if (!s.exec()) return false;
        } else {
            Stmt s(db_, "INSERT INTO players_settings(key,value) VALUES(?,?) "
                        "ON CONFLICT(key) DO UPDATE SET value=excluded.value");
            s.text(1, k).text(2, v);
            if (!s.exec()) return false;
        }
    }
    return tx.commit();
}

// ─── last known state and events ────────────────────────────────────────────
bool PlayersDb::applyStatuses(const std::vector<StatusRow>& rows) {
    for (const auto& r : rows)
        if (r.payload_json.size() > kMaxPayloadBytes) return false;   // nothing written
    std::lock_guard<std::mutex> lk(mu_);
    if (!db_) return false;
    Tx tx(db_);
    if (!tx.ok()) return false;
    for (const auto& r : rows) {
        // The WHERE EXISTS skips players that were deleted in the meantime.
        Stmt s(db_, "INSERT INTO player_status(player_id,updated_at,online,version,payload_json) "
                    "SELECT ?,?,?,?,? WHERE EXISTS (SELECT 1 FROM players WHERE id=?) "
                    "ON CONFLICT(player_id) DO UPDATE SET updated_at=excluded.updated_at, "
                    "online=excluded.online, version=excluded.version, payload_json=excluded.payload_json");
        s.text(1, r.player_id).i64(2, r.updated_at).i64(3, r.online ? 1 : 0)
         .text(4, r.version).text(5, r.payload_json).text(6, r.player_id);
        if (!s.exec()) return false;
    }
    return tx.commit();
}

std::optional<StatusRow> PlayersDb::status(const std::string& player_id) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!db_) return std::nullopt;
    Stmt s(db_, "SELECT player_id,updated_at,online,version,payload_json FROM player_status WHERE player_id=?");
    s.text(1, player_id);
    if (s.step() != SQLITE_ROW) return std::nullopt;
    return readStatus(s);
}

std::vector<StatusRow> PlayersDb::allStatuses() {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<StatusRow> out;
    if (!db_) return out;
    Stmt s(db_, "SELECT player_id,updated_at,online,version,payload_json FROM player_status ORDER BY player_id");
    while (s.step() == SQLITE_ROW) out.push_back(readStatus(s));
    return out;
}

bool PlayersDb::addEvent(const std::string& player_id, const std::string& kind,
                         const std::string& details, std::int64_t now) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!db_) return false;
    Stmt s(db_, "INSERT INTO player_events(player_id,ts,kind,details) VALUES(?,?,?,?)");
    s.text(1, player_id).i64(2, nowOr(now)).text(3, kind).text(4, details);
    return s.exec();
}

std::vector<EventRow> PlayersDb::events(const std::string& player_id, std::size_t limit) {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<EventRow> out;
    if (!db_ || limit == 0) return out;
    Stmt s(db_, "SELECT id,player_id,ts,kind,details FROM player_events WHERE player_id=? "
                "ORDER BY ts DESC, id DESC LIMIT ?");
    s.text(1, player_id).i64(2, static_cast<std::int64_t>(limit));
    while (s.step() == SQLITE_ROW) {
        EventRow e;
        e.id = s.num(0); e.player_id = s.str(1); e.ts = s.num(2);
        e.kind = s.str(3); e.details = s.str(4);
        out.push_back(std::move(e));
    }
    return out;
}

namespace {
constexpr const char* kCommandCols =
    "id,player_id,type,state,result,issued_at,ttl_seconds,deadline_at,issued_by,finished_at";
CommandRow readCommand(const Stmt& s) {
    CommandRow c;
    c.id = s.str(0); c.player_id = s.str(1); c.type = s.str(2); c.state = s.str(3); c.result = s.str(4);
    c.issued_at = s.num(5); c.ttl_seconds = s.num(6); c.deadline_at = s.num(7);
    c.issued_by = s.optNum(8); c.finished_at = s.optNum(9);
    return c;
}
}  // namespace

CommandCreate PlayersDb::createCommand(const CommandRow& row, std::string* busy_id) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!db_) return CommandCreate::Storage;
    Stmt s(db_, "INSERT INTO player_commands(id,player_id,type,state,result,issued_at,ttl_seconds,"
                "deadline_at,issued_by) VALUES(?,?,?,'pending','',?,?,?,?)");
    s.text(1, row.id).text(2, row.player_id).text(3, row.type).i64(4, row.issued_at)
     .i64(5, row.ttl_seconds).i64(6, row.deadline_at).opt(7, row.issued_by);
    if (s.exec()) return CommandCreate::Ok;
    if (sqlite3_extended_errcode(db_) == SQLITE_CONSTRAINT_UNIQUE) {
        if (busy_id) {
            Stmt q(db_, "SELECT id FROM player_commands WHERE player_id=? AND type=? AND state='pending'");
            q.text(1, row.player_id).text(2, row.type);
            if (q.step() == SQLITE_ROW) *busy_id = q.str(0);
        }
        return CommandCreate::Busy;
    }
    return CommandCreate::Storage;
}

namespace {
constexpr const char* kJobCols =
    "id,type,target_kind,target_id,target_name,state,stop_reason,batch_size,stop_percent,next_batch,"
    "created_at,created_by,created_by_name,created_by_role,finished_at";
JobRow readJob(const Stmt& s) {
    JobRow j;
    j.id = s.str(0); j.type = s.str(1); j.target_kind = s.str(2); j.target_id = s.num(3);
    j.target_name = s.str(4); j.state = s.str(5); j.stop_reason = s.str(6);
    j.batch_size = s.num(7); j.stop_percent = s.num(8); j.next_batch = s.num(9);
    j.created_at = s.num(10); j.created_by = s.optNum(11); j.created_by_name = s.str(12);
    j.created_by_role = s.str(13); j.finished_at = s.optNum(14);
    return j;
}
}  // namespace

bool PlayersDb::createJob(const JobRow& job, const std::vector<JobItem>& items) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!db_) return false;
    Tx tx(db_);
    if (!tx.ok()) return false;
    {
        Stmt s(db_, "INSERT INTO command_jobs(id,type,target_kind,target_id,target_name,state,batch_size,"
                    "stop_percent,next_batch,created_at,created_by,created_by_name,created_by_role) "
                    "VALUES(?,?,?,?,?,'running',?,?,0,?,?,?,?)");
        s.text(1, job.id).text(2, job.type).text(3, job.target_kind).i64(4, job.target_id)
         .text(5, job.target_name).i64(6, job.batch_size).i64(7, job.stop_percent).i64(8, job.created_at)
         .opt(9, job.created_by).text(10, job.created_by_name).text(11, job.created_by_role);
        if (!s.exec()) return false;
    }
    for (const auto& it : items) {
        Stmt s(db_, "INSERT INTO command_job_items(job_id,player_id,batch_no,state) VALUES(?,?,?,?)");
        s.text(1, job.id).text(2, it.player_id).i64(3, it.batch_no).text(4, it.state);
        if (!s.exec()) return false;
    }
    return tx.commit();
}

std::optional<JobRow> PlayersDb::findJob(const std::string& id) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!db_) return std::nullopt;
    Stmt s(db_, (std::string("SELECT ") + kJobCols + " FROM command_jobs WHERE id=?").c_str());
    s.text(1, id);
    if (s.step() != SQLITE_ROW) return std::nullopt;
    return readJob(s);
}

std::vector<JobRow> PlayersDb::runningJobs() {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<JobRow> out;
    if (!db_) return out;
    Stmt s(db_, (std::string("SELECT ") + kJobCols + " FROM command_jobs WHERE state='running' ORDER BY created_at").c_str());
    while (s.step() == SQLITE_ROW) out.push_back(readJob(s));
    return out;
}

std::vector<JobRow> PlayersDb::listJobs(std::optional<std::int64_t> created_by, std::size_t limit) {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<JobRow> out;
    if (!db_ || limit == 0) return out;
    std::string sql = std::string("SELECT ") + kJobCols + " FROM command_jobs";
    if (created_by) sql += " WHERE created_by=?";
    sql += " ORDER BY created_at DESC, rowid DESC LIMIT ?";
    Stmt s(db_, sql.c_str());
    int i = 1;
    if (created_by) s.i64(i++, *created_by);
    s.i64(i, static_cast<std::int64_t>(limit));
    while (s.step() == SQLITE_ROW) out.push_back(readJob(s));
    return out;
}

std::vector<JobItem> PlayersDb::jobItems(const std::string& job_id) {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<JobItem> out;
    if (!db_) return out;
    Stmt s(db_, "SELECT job_id,player_id,batch_no,state,command_id,error FROM command_job_items "
                "WHERE job_id=? ORDER BY batch_no, rowid");
    s.text(1, job_id);
    while (s.step() == SQLITE_ROW) {
        JobItem it;
        it.job_id = s.str(0); it.player_id = s.str(1); it.batch_no = s.num(2);
        it.state = s.str(3); it.command_id = s.str(4); it.error = s.str(5);
        out.push_back(std::move(it));
    }
    return out;
}

std::vector<PlayersDb::JobItemCount> PlayersDb::jobItemCounts(const std::string& job_id) {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<JobItemCount> out;
    if (!db_) return out;
    Stmt s(db_, "SELECT ji.state, COALESCE(pc.state,''), pc.id IS NOT NULL, COUNT(*) "
                "FROM command_job_items ji LEFT JOIN player_commands pc ON pc.id = ji.command_id AND ji.command_id <> '' "
                "WHERE ji.job_id=? GROUP BY ji.state, pc.state, pc.id IS NOT NULL");
    s.text(1, job_id);
    while (s.step() == SQLITE_ROW) {
        JobItemCount c;
        c.item_state = s.str(0); c.command_state = s.str(1); c.has_command = s.num(2) != 0; c.n = s.num(3);
        out.push_back(std::move(c));
    }
    return out;
}

bool PlayersDb::updateJobItem(const std::string& job_id, const std::string& player_id,
                              const std::string& state, const std::string& command_id,
                              const std::string& error) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!db_) return false;
    Stmt s(db_, "UPDATE command_job_items SET state=?, command_id=?, error=? WHERE job_id=? AND player_id=?");
    s.text(1, state).text(2, command_id).text(3, error).text(4, job_id).text(5, player_id);
    return s.exec() && s.changes() == 1;
}

bool PlayersDb::setNextBatch(const std::string& job_id, std::int64_t next_batch) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!db_) return false;
    Stmt s(db_, "UPDATE command_jobs SET next_batch=? WHERE id=? AND state='running'");
    s.i64(1, next_batch).text(2, job_id);
    return s.exec() && s.changes() == 1;
}

bool PlayersDb::finishJob(const std::string& job_id, const std::string& state,
                          const std::string& reason, std::int64_t now) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!db_) return false;
    Tx tx(db_);
    if (!tx.ok()) return false;
    {
        Stmt s(db_, "UPDATE command_jobs SET state=?, stop_reason=?, finished_at=? WHERE id=? AND state='running'");
        s.text(1, state).text(2, reason).i64(3, nowOr(now)).text(4, job_id);
        if (!s.exec() || s.changes() != 1) return false;
    }
    {
        Stmt s(db_, "UPDATE command_job_items SET state='cancelled' WHERE job_id=? AND state='queued'");
        s.text(1, job_id);
        if (!s.exec()) return false;
    }
    return tx.commit();
}

std::size_t PlayersDb::purgeSettledCommandsOlderThan(std::int64_t ts) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!db_) return 0;
    Stmt s(db_, "DELETE FROM player_commands WHERE state <> 'pending' AND finished_at IS NOT NULL AND finished_at < ?");
    s.i64(1, ts);
    return s.exec() ? static_cast<std::size_t>(s.changes()) : 0;
}

std::size_t PlayersDb::purgeFinishedJobsOlderThan(std::int64_t ts) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!db_) return 0;
    Stmt s(db_, "DELETE FROM command_jobs WHERE state <> 'running' AND finished_at IS NOT NULL AND finished_at < ?");
    s.i64(1, ts);
    return s.exec() ? static_cast<std::size_t>(s.changes()) : 0;       // command_job_items follow (ON DELETE CASCADE)
}

std::string PlayersDb::newId() { return newUuidV4(); }

std::optional<CommandRow> PlayersDb::findCommand(const std::string& id) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!db_) return std::nullopt;
    Stmt s(db_, (std::string("SELECT ") + kCommandCols + " FROM player_commands WHERE id=?").c_str());
    s.text(1, id);
    if (s.step() != SQLITE_ROW) return std::nullopt;
    return readCommand(s);
}

std::vector<CommandRow> PlayersDb::commandsOf(const std::string& player_id, std::size_t limit) {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<CommandRow> out;
    if (!db_ || limit == 0) return out;
    Stmt s(db_, (std::string("SELECT ") + kCommandCols +
                 " FROM player_commands WHERE player_id=? ORDER BY issued_at DESC, rowid DESC LIMIT ?").c_str());
    s.text(1, player_id).i64(2, static_cast<std::int64_t>(limit));
    while (s.step() == SQLITE_ROW) out.push_back(readCommand(s));
    return out;
}

std::vector<CommandRow> PlayersDb::pendingCommandsOf(const std::string& player_id) {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<CommandRow> out;
    if (!db_) return out;
    Stmt s(db_, (std::string("SELECT ") + kCommandCols +
                 " FROM player_commands WHERE player_id=? AND state='pending' ORDER BY issued_at").c_str());
    s.text(1, player_id);
    while (s.step() == SQLITE_ROW) out.push_back(readCommand(s));
    return out;
}

std::optional<CommandRow> PlayersDb::settleCommand(const std::string& id, const std::string& state,
                                                   const std::string& result, std::int64_t now) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!db_) return std::nullopt;
    {
        Stmt s(db_, "UPDATE player_commands SET state=?, result=?, finished_at=? WHERE id=? AND state='pending'");
        s.text(1, state).text(2, result).i64(3, nowOr(now)).text(4, id);
        if (!s.exec() || s.changes() == 0) return std::nullopt;
    }
    Stmt q(db_, (std::string("SELECT ") + kCommandCols + " FROM player_commands WHERE id=?").c_str());
    q.text(1, id);
    if (q.step() != SQLITE_ROW) return std::nullopt;
    return readCommand(q);
}

std::vector<CommandRow> PlayersDb::timeoutCommands(std::int64_t now) {
    const std::int64_t t = nowOr(now);
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<CommandRow> out;
    if (!db_) return out;
    {
        Stmt s(db_, (std::string("SELECT ") + kCommandCols +
                     " FROM player_commands WHERE state='pending' AND deadline_at <= ?").c_str());
        s.i64(1, t);
        while (s.step() == SQLITE_ROW) out.push_back(readCommand(s));
    }
    for (auto& c : out) {
        Stmt u(db_, "UPDATE player_commands SET state='timeout', result='no_confirmation', finished_at=? "
                    "WHERE id=? AND state='pending'");
        u.i64(1, t).text(2, c.id);
        u.exec();
        c.state = "timeout";
        c.result = "no_confirmation";
        c.finished_at = t;
    }
    return out;
}

std::size_t PlayersDb::purgeEventsOlderThan(std::int64_t ts) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!db_) return 0;
    Stmt s(db_, "DELETE FROM player_events WHERE ts < ?");
    s.i64(1, ts);
    return s.exec() ? static_cast<std::size_t>(s.changes()) : 0;
}

}  // namespace liveqx::players
