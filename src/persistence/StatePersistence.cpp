// fix17 step 2 — ChannelStatePersistence implementation.
//
// Layout:
//   PRAGMA user_version=1; journal_mode=WAL; synchronous=NORMAL;
//   CREATE TABLE channel_state (
//       k             TEXT PRIMARY KEY,
//       v             TEXT NOT NULL,
//       updated_at_ns INTEGER NOT NULL);
//
// Keys: "cursor", "paused", "schedule_active".
// Values are JSON blobs so the schema stays stable as new sub-fields
// (e.g. additional cursor invariants) get added.
//
// On any SQLite error during open / schema / load / save we rename the
// underlying file to "<path>.corrupt-<unix_ns>" so the canonical path is
// free for a fresh start, and degrade to ok()==false until the next
// save() reopens it. This matches the spec's "не падать на corrupt".

#include "persistence/StatePersistence.h"

#include <chrono>
#include <stdexcept>
#include <system_error>

#include <sqlite3.h>
#include <nlohmann/json.hpp>

#include "utils/Log.h"

namespace liveqx::persistence {
namespace {

constexpr int kSchemaVersion = 1;

constexpr const char* kCreateSql =
    "CREATE TABLE IF NOT EXISTS channel_state ("
    "  k             TEXT PRIMARY KEY,"
    "  v             TEXT NOT NULL,"
    "  updated_at_ns INTEGER NOT NULL"
    ")";

std::int64_t nowUnixNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// Move file to "<path>.corrupt-<unix_ns>". Best effort — if the rename
// itself fails (read-only fs, missing parent) we just log and leave the
// file in place; the open path will keep failing but won't crash.
void renameCorrupt(const std::filesystem::path& p) {
    if (!std::filesystem::exists(p)) return;
    auto target = p;
    target += ".corrupt-" + std::to_string(nowUnixNs());
    std::error_code ec;
    std::filesystem::rename(p, target, ec);
    if (ec) {
        LOG_ERROR("ChannelStatePersistence: cannot rename corrupt {} -> {}: {}",
                  p.string(), target.string(), ec.message());
    } else {
        LOG_WARN("ChannelStatePersistence: corrupt db moved to {}",
                 target.string());
    }
}

bool execOk(sqlite3* db, const char* sql) {
    char* err = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        LOG_ERROR("ChannelStatePersistence: exec {} failed: {}",
                  sql, err ? err : "(null)");
        sqlite3_free(err);
        return false;
    }
    return true;
}

bool readUserVersion(sqlite3* db, int& out) {
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, "PRAGMA user_version;", -1, &st, nullptr)
            != SQLITE_OK)
        return false;
    bool ok = false;
    if (sqlite3_step(st) == SQLITE_ROW) {
        out = sqlite3_column_int(st, 0);
        ok = true;
    }
    sqlite3_finalize(st);
    return ok;
}

bool writeRow(sqlite3* db,
              const char* key,
              const std::string& value_json,
              std::int64_t ts_ns) {
    static constexpr const char* kSql =
        "INSERT INTO channel_state(k,v,updated_at_ns) VALUES(?,?,?) "
        "ON CONFLICT(k) DO UPDATE SET v=excluded.v, "
        "                              updated_at_ns=excluded.updated_at_ns";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, kSql, -1, &st, nullptr) != SQLITE_OK) {
        LOG_ERROR("ChannelStatePersistence: prepare write failed: {}",
                  sqlite3_errmsg(db));
        return false;
    }
    sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, value_json.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 3, ts_ns);
    const int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        LOG_ERROR("ChannelStatePersistence: step write k={} failed: {}",
                  key, sqlite3_errmsg(db));
        return false;
    }
    return true;
}

}  // namespace

ChannelStatePersistence::ChannelStatePersistence(std::filesystem::path db_path)
    : path_(std::move(db_path)) {
    std::error_code ec;
    if (path_.has_parent_path())
        std::filesystem::create_directories(path_.parent_path(), ec);
    openAndPrepare();
}

ChannelStatePersistence::~ChannelStatePersistence() {
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

bool ChannelStatePersistence::openAndPrepare() {
    if (db_) return true;
    sqlite3* db = nullptr;
    if (sqlite3_open(path_.string().c_str(), &db) != SQLITE_OK) {
        LOG_ERROR("ChannelStatePersistence: open {} failed: {}",
                  path_.string(), db ? sqlite3_errmsg(db) : "(no db)");
        if (db) sqlite3_close(db);
        renameCorrupt(path_);
        return false;
    }

    int existing_version = 0;
    if (!readUserVersion(db, existing_version)) {
        LOG_ERROR("ChannelStatePersistence: PRAGMA user_version read failed: {}",
                  sqlite3_errmsg(db));
        sqlite3_close(db);
        renameCorrupt(path_);
        return false;
    }
    if (existing_version > kSchemaVersion) {
        LOG_ERROR("ChannelStatePersistence: schema v{} newer than supported v{} "
                  "— refusing {}",
                  existing_version, kSchemaVersion, path_.string());
        sqlite3_close(db);
        // Do *not* rename — operator might be downgrading; preserve forensics.
        return false;
    }

    if (!execOk(db, "PRAGMA journal_mode=WAL;") ||
        !execOk(db, "PRAGMA synchronous=NORMAL;") ||
        !execOk(db, kCreateSql)) {
        sqlite3_close(db);
        renameCorrupt(path_);
        return false;
    }
    if (existing_version < kSchemaVersion) {
        const std::string set_pragma =
            "PRAGMA user_version=" + std::to_string(kSchemaVersion) + ";";
        if (!execOk(db, set_pragma.c_str())) {
            sqlite3_close(db);
            renameCorrupt(path_);
            return false;
        }
    }
    db_ = db;
    return true;
}

bool ChannelStatePersistence::save(const ChannelStateSnapshot& snap) {
    if (snap.empty()) return true;
    if (!db_ && !openAndPrepare()) return false;

    if (!execOk(db_, "BEGIN IMMEDIATE;")) return false;

    bool ok = true;
    const std::int64_t ts_ns = nowUnixNs();

    if (snap.playlist_index || snap.clip_path || snap.slot_pos_sec) {
        nlohmann::json j = nlohmann::json::object();
        if (snap.playlist_index) j["playlist_index"] = *snap.playlist_index;
        if (snap.clip_path)      j["clip_path"]      = *snap.clip_path;
        if (snap.slot_pos_sec)   j["slot_pos_sec"]   = *snap.slot_pos_sec;
        ok = writeRow(db_, "cursor", j.dump(), ts_ns);
    }
    if (ok && snap.paused) {
        nlohmann::json j = *snap.paused;
        ok = writeRow(db_, "paused", j.dump(), ts_ns);
    }
    if (ok && snap.schedule_active) {
        ok = writeRow(db_, "schedule_active",
                      snap.schedule_active->dump(), ts_ns);
    }

    if (!ok) {
        execOk(db_, "ROLLBACK;");
        return false;
    }
    return execOk(db_, "COMMIT;");
}

ChannelStateSnapshot ChannelStatePersistence::load() {
    ChannelStateSnapshot out;
    if (!db_ && !openAndPrepare()) return out;

    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT k,v FROM channel_state;",
                           -1, &st, nullptr) != SQLITE_OK) {
        LOG_ERROR("ChannelStatePersistence: prepare SELECT failed: {}",
                  sqlite3_errmsg(db_));
        sqlite3_close(db_);
        db_ = nullptr;
        renameCorrupt(path_);
        return out;
    }
    int rc = SQLITE_ROW;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        const char* k = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
        const char* v = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
        if (!k || !v) continue;
        try {
            const auto j = nlohmann::json::parse(v);
            const std::string key = k;
            if (key == "cursor") {
                if (j.contains("playlist_index"))
                    out.playlist_index = j["playlist_index"].get<int>();
                if (j.contains("clip_path"))
                    out.clip_path = j["clip_path"].get<std::string>();
                if (j.contains("slot_pos_sec"))
                    out.slot_pos_sec = j["slot_pos_sec"].get<double>();
            } else if (key == "paused") {
                out.paused = j.get<bool>();
            } else if (key == "schedule_active") {
                out.schedule_active = j;
            } else {
                // Unknown key — skip; lets future schema bumps coexist.
            }
        } catch (const std::exception& e) {
            LOG_WARN("ChannelStatePersistence: bad value for k='{}': {}",
                     k ? k : "(null)", e.what());
        }
    }
    sqlite3_finalize(st);

    if (rc != SQLITE_DONE) {
        LOG_ERROR("ChannelStatePersistence: SELECT step failed (rc={}): {}",
                  rc, sqlite3_errmsg(db_));
        sqlite3_close(db_);
        db_ = nullptr;
        renameCorrupt(path_);
        return ChannelStateSnapshot{};
    }
    return out;
}

}  // namespace liveqx::persistence
