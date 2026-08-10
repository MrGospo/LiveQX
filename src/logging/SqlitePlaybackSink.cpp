#include "SqlitePlaybackSink.h"

#include <algorithm>
#include <ctime>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <vector>

#include <spdlog/spdlog.h>
#include <sqlite3.h>

namespace liveqx::logging {

namespace fs = std::filesystem;

namespace {

constexpr const char* kCreateSql =
    "CREATE TABLE IF NOT EXISTS playback ("
    " started_at_ns   INTEGER NOT NULL,"
    " ended_at_ns     INTEGER NOT NULL,"
    " clip_path       TEXT    NOT NULL,"
    " clip_type       TEXT    NOT NULL,"
    " played_sec      REAL    NOT NULL,"
    " transition_type TEXT    NOT NULL,"
    " status          TEXT    NOT NULL,"
    " error_reason    TEXT"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_started ON playback(started_at_ns);";

constexpr const char* kInsertSql =
    "INSERT INTO playback("
    " started_at_ns, ended_at_ns, clip_path, clip_type,"
    " played_sec, transition_type, status, error_reason)"
    " VALUES(?,?,?,?,?,?,?,?)";

bool fileLooksLikeWeekDb(const std::string& name, std::string* week_out) {
    // db-YYYY-Www.db  e.g. "db-2026-W18.db"  → 14 chars
    static const std::regex re(R"(^db-(\d{4}-W\d{2})\.db$)");
    std::smatch m;
    if (!std::regex_match(name, m, re)) return false;
    if (week_out) *week_out = m[1].str();
    return true;
}

}  // namespace

SqlitePlaybackSink::SqlitePlaybackSink(int default_retention_days)
    : default_retention_days_(default_retention_days),
      next_retention_at_(std::chrono::steady_clock::now() +
                         kRetentionInterval) {
    writer_thread_ = std::thread(&SqlitePlaybackSink::writerLoop, this);
}

SqlitePlaybackSink::~SqlitePlaybackSink() {
    running_.store(false, std::memory_order_release);
    queue_.notify_all();
    if (writer_thread_.joinable()) writer_thread_.join();
    closeAllHandles();
}

void SqlitePlaybackSink::registerChannel(int channel_id, fs::path channel_dir,
                                         int retention_days) {
    std::error_code ec;
    fs::create_directories(playbackDir(channel_dir), ec);
    if (ec) {
        throw std::runtime_error(
            "SqlitePlaybackSink::registerChannel: cannot create " +
            playbackDir(channel_dir).string() + ": " + ec.message());
    }
    std::unique_lock lk(channels_mu_);
    channels_[channel_id] = {std::move(channel_dir),
                             retention_days > 0 ? retention_days
                                                : default_retention_days_};
}

void SqlitePlaybackSink::unregisterChannel(int channel_id) {
    std::unique_lock lk(channels_mu_);
    channels_.erase(channel_id);
}

void SqlitePlaybackSink::log(const PlaybackEvent& ev) {
    if (!queue_.push(ev)) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    queue_.notify();
}

std::string SqlitePlaybackSink::weekString(int64_t ns) {
    const auto sec = static_cast<std::time_t>(ns / 1'000'000'000LL);
    std::tm tm{};
    gmtime_r(&sec, &tm);
    char buf[16];
    // %G = ISO 8601 year of the week, %V = ISO week (01..53). Both glibc.
    std::strftime(buf, sizeof(buf), "%G-W%V", &tm);
    return buf;
}

bool SqlitePlaybackSink::weekRangeNs(const std::string& week,
                                     int64_t* start_ns, int64_t* end_ns) {
    // "YYYY-Www" → понедельник 00:00 UTC начала ISO-недели.
    //
    // strptime("%G-W%V-%u") в glibc парсит ISO-поля, но НЕ заполняет
    // tm_year/tm_mon/tm_mday в общем случае — поэтому считаем вручную:
    // Jan 4 каждого ISO-года всегда лежит в W01. Понедельник W01 = Jan 4
    // минус (его tm_wday в ISO-нумерации). Дальше W = W01 + (n-1)*7 суток.
    static const std::regex re(R"(^(\d{4})-W(\d{2})$)");
    std::smatch m;
    if (!std::regex_match(week, m, re)) return false;
    const int year = std::stoi(m[1].str());
    const int wnum = std::stoi(m[2].str());
    if (wnum < 1 || wnum > 53) return false;

    std::tm tm_jan4{};
    tm_jan4.tm_year = year - 1900;
    tm_jan4.tm_mon  = 0;
    tm_jan4.tm_mday = 4;
    const std::time_t jan4 = timegm(&tm_jan4);
    if (jan4 == static_cast<std::time_t>(-1)) return false;
    std::tm tm_norm{};
    gmtime_r(&jan4, &tm_norm);
    // POSIX: Sun=0..Sat=6. ISO: Mon=0..Sun=6. iso_dow для Jan 4.
    const int iso_dow = (tm_norm.tm_wday + 6) % 7;
    const std::time_t monday_w01 = jan4 - iso_dow * 86400;
    const std::time_t monday_w   = monday_w01 + (wnum - 1) * 7 * 86400;

    const int64_t start = static_cast<int64_t>(monday_w) * 1'000'000'000LL;
    const int64_t end = start + (7LL * 86400LL * 1'000'000'000LL) - 1LL;
    if (start_ns) *start_ns = start;
    if (end_ns)   *end_ns   = end;
    return true;
}

fs::path SqlitePlaybackSink::playbackDir(const fs::path& channel_dir) const {
    return channel_dir / "playback";
}

fs::path SqlitePlaybackSink::dbPath(int /*channel_id*/,
                                    const fs::path& channel_dir,
                                    const std::string& week) const {
    return playbackDir(channel_dir) / ("db-" + week + ".db");
}

std::string SqlitePlaybackSink::handleKey(int channel_id,
                                          const std::string& week) {
    return std::to_string(channel_id) + "/" + week;
}

void SqlitePlaybackSink::execOrThrow(sqlite3* db, const char* sql) {
    char* err = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        const std::string msg = err ? err : "sqlite3_exec failed";
        sqlite3_free(err);
        throw std::runtime_error(msg);
    }
}

void SqlitePlaybackSink::prepareSchema(sqlite3* db) {
    int existing_version = 0;
    {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db, "PRAGMA user_version;", -1, &st, nullptr)
                == SQLITE_OK) {
            if (sqlite3_step(st) == SQLITE_ROW)
                existing_version = sqlite3_column_int(st, 0);
            sqlite3_finalize(st);
        }
    }
    if (existing_version > kSchemaVersion) {
        throw std::runtime_error(
            "playback DB schema v" + std::to_string(existing_version) +
            " is newer than supported v" + std::to_string(kSchemaVersion) +
            " — refusing to open");
    }
    execOrThrow(db, "PRAGMA journal_mode=WAL;");
    execOrThrow(db, "PRAGMA synchronous=NORMAL;");
    execOrThrow(db, kCreateSql);
    if (existing_version < kSchemaVersion) {
        // Future: run ALTER TABLE migrations between existing_version+1
        // and kSchemaVersion here, in order. v1 is the baseline.
        execOrThrow(db,
            ("PRAGMA user_version=" + std::to_string(kSchemaVersion) + ";")
                .c_str());
    }
}

sqlite3* SqlitePlaybackSink::writerHandle(int channel_id,
                                         const std::string& week,
                                         sqlite3_stmt** insert_stmt) {
    std::lock_guard handles_lk(handles_mu_);
    const auto key = handleKey(channel_id, week);
    if (auto it = handle_index_.find(key); it != handle_index_.end()) {
        // Move to front (MRU).
        handles_.splice(handles_.begin(), handles_, it->second);
        if (insert_stmt) *insert_stmt = it->second->insert_stmt;
        return it->second->db;
    }

    fs::path channel_dir;
    {
        std::shared_lock lk(channels_mu_);
        auto chit = channels_.find(channel_id);
        if (chit == channels_.end()) return nullptr;
        channel_dir = chit->second.channel_dir;
    }
    const auto path = dbPath(channel_id, channel_dir, week);
    sqlite3* db = nullptr;
    if (sqlite3_open(path.string().c_str(), &db) != SQLITE_OK) {
        spdlog::error("SqlitePlaybackSink: open {} failed: {}",
                      path.string(),
                      db ? sqlite3_errmsg(db) : "no db");
        if (db) sqlite3_close(db);
        return nullptr;
    }
    try {
        prepareSchema(db);
    } catch (const std::exception& e) {
        spdlog::error("SqlitePlaybackSink: schema {} failed: {}",
                      path.string(), e.what());
        schema_errors_.fetch_add(1, std::memory_order_relaxed);
        sqlite3_close(db);
        return nullptr;
    }
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, kInsertSql, -1, &stmt, nullptr) != SQLITE_OK) {
        spdlog::error("SqlitePlaybackSink: prepare {} failed: {}",
                      path.string(), sqlite3_errmsg(db));
        sqlite3_close(db);
        return nullptr;
    }

    // Evict LRU if cache full.
    while (handles_.size() >= kHandleCacheSize) {
        auto& back = handles_.back();
        if (back.insert_stmt) sqlite3_finalize(back.insert_stmt);
        if (back.db) sqlite3_close(back.db);
        handle_index_.erase(handleKey(back.channel_id, back.week));
        handles_.pop_back();
    }
    handles_.push_front(HandleEntry{channel_id, week, db, stmt});
    handle_index_[key] = handles_.begin();
    if (insert_stmt) *insert_stmt = stmt;
    return db;
}

void SqlitePlaybackSink::closeAllHandles() {
    std::lock_guard handles_lk(handles_mu_);
    for (auto& h : handles_) {
        if (h.insert_stmt) sqlite3_finalize(h.insert_stmt);
        if (h.db) sqlite3_close(h.db);
    }
    handles_.clear();
    handle_index_.clear();
}

void SqlitePlaybackSink::writerLoop() {
    using namespace std::chrono_literals;
    while (running_.load(std::memory_order_acquire) || !queue_.empty()) {
        if (auto opt = queue_.pop_wait(100ms)) {
            const auto& ev = *opt;
            const auto week = weekString(ev.started_at_ns);
            sqlite3_stmt* stmt = nullptr;
            sqlite3* db = writerHandle(ev.channel_id, week, &stmt);
            if (!db || !stmt) continue;
            sqlite3_reset(stmt);
            sqlite3_clear_bindings(stmt);
            sqlite3_bind_int64(stmt, 1, ev.started_at_ns);
            sqlite3_bind_int64(stmt, 2, ev.ended_at_ns);
            sqlite3_bind_text  (stmt, 3, ev.clip_path.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text  (stmt, 4, ev.clip_type.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_double(stmt, 5, ev.played_sec);
            sqlite3_bind_text  (stmt, 6, ev.transition_type.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text  (stmt, 7, ev.status.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text  (stmt, 8, ev.error_reason.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(stmt) != SQLITE_DONE) {
                spdlog::warn("SqlitePlaybackSink: insert failed: {}",
                             sqlite3_errmsg(db));
            } else {
                last_write_ns_.store(ev.started_at_ns,
                                     std::memory_order_relaxed);
            }
        }
        if (std::chrono::steady_clock::now() >= next_retention_at_) {
            runRetentionSweep();
            next_retention_at_ = std::chrono::steady_clock::now() +
                                 kRetentionInterval;
        }
    }
}

void SqlitePlaybackSink::runRetentionSweep() {
    std::vector<std::pair<int, ChannelInfo>> snapshot;
    {
        std::shared_lock lk(channels_mu_);
        snapshot.reserve(channels_.size());
        for (const auto& [id, info] : channels_) snapshot.emplace_back(id, info);
    }
    const auto now = std::chrono::system_clock::now();
    for (const auto& [channel_id, info] : snapshot) {
        const auto pdir = playbackDir(info.channel_dir);
        std::error_code ec;
        if (!fs::is_directory(pdir, ec)) continue;
        for (const auto& entry : fs::directory_iterator(pdir, ec)) {
            if (ec) break;
            if (!entry.is_regular_file()) continue;
            std::string week;
            if (!fileLooksLikeWeekDb(entry.path().filename().string(), &week))
                continue;
            const auto mtime = fs::last_write_time(entry.path(), ec);
            if (ec) continue;
            const auto sct = std::chrono::file_clock::to_sys(mtime);
            const auto age = now - sct;
            if (age > std::chrono::hours(24) * info.retention_days) {
                // Close any open handle for this file before unlinking.
                {
                    std::lock_guard handles_lk(handles_mu_);
                    const auto key = handleKey(channel_id, week);
                    if (auto it = handle_index_.find(key);
                        it != handle_index_.end()) {
                        if (it->second->insert_stmt)
                            sqlite3_finalize(it->second->insert_stmt);
                        if (it->second->db) sqlite3_close(it->second->db);
                        handles_.erase(it->second);
                        handle_index_.erase(it);
                    }
                }
                std::error_code rm_ec;
                fs::remove(entry.path(), rm_ec);
                // Also remove WAL/SHM siblings.
                fs::remove(entry.path().string() + "-wal", rm_ec);
                fs::remove(entry.path().string() + "-shm", rm_ec);
            }
        }
    }
}

nlohmann::json SqlitePlaybackSink::query(const QueryParams& params) {
    const int limit  = std::clamp(params.limit, 1, 1000);
    const int offset = std::max(0, params.offset);
    const int64_t from   = params.from_ns.value_or(INT64_MIN);
    const int64_t to     = params.to_ns.value_or(INT64_MAX);
    const int64_t cursor = params.after_ns.value_or(INT64_MIN);

    nlohmann::json events  = nlohmann::json::array();
    nlohmann::json next_after = nullptr;

    fs::path channel_dir;
    {
        std::shared_lock lk(channels_mu_);
        auto it = channels_.find(params.channel_id);
        if (it == channels_.end()) {
            return {{"events", events}, {"next_after_ns", next_after}};
        }
        channel_dir = it->second.channel_dir;
    }
    const auto pdir = playbackDir(channel_dir);
    if (!fs::is_directory(pdir)) {
        return {{"events", events}, {"next_after_ns", next_after}};
    }

    std::vector<fs::path> files;
    for (const auto& entry : fs::directory_iterator(pdir)) {
        if (!entry.is_regular_file()) continue;
        if (fileLooksLikeWeekDb(entry.path().filename().string(), nullptr))
            files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());

    std::vector<nlohmann::json> matched;
    for (const auto& fp : files) {
        sqlite3* rdb = nullptr;
        const std::string uri = "file:" + fp.string() + "?mode=ro";
        if (sqlite3_open_v2(uri.c_str(), &rdb,
                            SQLITE_OPEN_READONLY | SQLITE_OPEN_URI,
                            nullptr) != SQLITE_OK) {
            if (rdb) sqlite3_close(rdb);
            continue;
        }
        const char* sel =
            "SELECT started_at_ns, ended_at_ns, clip_path, clip_type,"
            " played_sec, transition_type, status, error_reason"
            " FROM playback WHERE started_at_ns >= ? AND started_at_ns <= ?"
            " ORDER BY started_at_ns";
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(rdb, sel, -1, &st, nullptr) != SQLITE_OK) {
            sqlite3_close(rdb);
            continue;
        }
        sqlite3_bind_int64(st, 1, from);
        sqlite3_bind_int64(st, 2, to);
        while (sqlite3_step(st) == SQLITE_ROW) {
            const int64_t st_ns = sqlite3_column_int64(st, 0);
            if (params.after_ns && st_ns <= cursor) continue;
            nlohmann::json j;
            j["channel_id"]      = params.channel_id;
            j["started_at_ns"]   = st_ns;
            j["ended_at_ns"]     = sqlite3_column_int64(st, 1);
            j["clip_path"]       = reinterpret_cast<const char*>(
                sqlite3_column_text(st, 2));
            j["clip_type"]       = reinterpret_cast<const char*>(
                sqlite3_column_text(st, 3));
            j["played_sec"]      = sqlite3_column_double(st, 4);
            j["transition_type"] = reinterpret_cast<const char*>(
                sqlite3_column_text(st, 5));
            j["status"]          = reinterpret_cast<const char*>(
                sqlite3_column_text(st, 6));
            const auto* err = sqlite3_column_text(st, 7);
            j["error_reason"]    = err ? reinterpret_cast<const char*>(err) : "";
            matched.push_back(std::move(j));
        }
        sqlite3_finalize(st);
        sqlite3_close(rdb);
    }

    const size_t start = params.after_ns
        ? 0
        : std::min(static_cast<size_t>(offset), matched.size());
    for (size_t i = start;
         i < matched.size() && static_cast<int>(events.size()) < limit; ++i) {
        events.push_back(std::move(matched[i]));
    }
    if (static_cast<int>(events.size()) == limit && !events.empty()) {
        next_after = events.back().value("started_at_ns", int64_t{0});
    }
    return {{"events", events}, {"next_after_ns", next_after}};
}

nlohmann::json SqlitePlaybackSink::purge(const PurgeParams& params) {
    const int64_t from = params.from_ns.value_or(INT64_MIN);
    const int64_t to   = params.to_ns.value_or(INT64_MAX);

    int64_t deleted_rows   = 0;
    int64_t removed_files  = 0;

    fs::path channel_dir;
    {
        std::shared_lock lk(channels_mu_);
        auto it = channels_.find(params.channel_id);
        if (it == channels_.end()) {
            return {{"deleted_rows", 0}, {"removed_files", 0}};
        }
        channel_dir = it->second.channel_dir;
    }
    const auto pdir = playbackDir(channel_dir);
    std::error_code ec;
    if (!fs::is_directory(pdir, ec)) {
        return {{"deleted_rows", 0}, {"removed_files", 0}};
    }

    auto evictHandle = [this](int channel_id, const std::string& week) {
        std::lock_guard handles_lk(handles_mu_);
        const auto key = handleKey(channel_id, week);
        if (auto it = handle_index_.find(key); it != handle_index_.end()) {
            if (it->second->insert_stmt) sqlite3_finalize(it->second->insert_stmt);
            if (it->second->db)          sqlite3_close(it->second->db);
            handles_.erase(it->second);
            handle_index_.erase(it);
        }
    };

    for (const auto& entry : fs::directory_iterator(pdir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        std::string week;
        if (!fileLooksLikeWeekDb(entry.path().filename().string(), &week))
            continue;
        int64_t week_start = 0, week_end = 0;
        if (!weekRangeNs(week, &week_start, &week_end)) continue;

        // Файл вне диапазона — пропускаем.
        if (week_end < from || week_start > to) continue;

        const bool file_fully_covered = (week_start >= from && week_end <= to);

        if (file_fully_covered) {
            // Закрыть открытый handle перед unlink, чтобы освободить fd.
            evictHandle(params.channel_id, week);
            std::error_code rm_ec;
            const auto path = entry.path();
            if (fs::remove(path, rm_ec)) {
                ++removed_files;
            }
            fs::remove(path.string() + "-wal", rm_ec);
            fs::remove(path.string() + "-shm", rm_ec);
            continue;
        }

        // Частичное пересечение — DELETE FROM playback WHERE ... Открываем
        // отдельный read-write connection. WAL разруливает конкуренцию с
        // writer-handle — busy_timeout страхует от SQLITE_BUSY при гонке.
        // Важно: не эвиктим writer-handle здесь — closing «последнего»
        // connection на WAL-файле может спровоцировать auto-checkpoint и
        // race с нашим только что открытым connection'ом.
        sqlite3* db = nullptr;
        if (sqlite3_open(entry.path().string().c_str(), &db) != SQLITE_OK) {
            if (db) sqlite3_close(db);
            continue;
        }
        sqlite3_busy_timeout(db, 5000);
        const int64_t lo = std::max(from, week_start);
        const int64_t hi = std::min(to,   week_end);
        sqlite3_stmt* st = nullptr;
        const char* del =
            "DELETE FROM playback "
            "WHERE started_at_ns >= ? AND started_at_ns <= ?";
        if (sqlite3_prepare_v2(db, del, -1, &st, nullptr) != SQLITE_OK) {
            spdlog::warn("SqlitePlaybackSink::purge: prepare DELETE failed "
                         "on {}: {}", entry.path().string(),
                         sqlite3_errmsg(db));
            sqlite3_close(db);
            continue;
        }
        sqlite3_bind_int64(st, 1, lo);
        sqlite3_bind_int64(st, 2, hi);
        const int rc = sqlite3_step(st);
        if (rc == SQLITE_DONE) {
            deleted_rows += sqlite3_changes(db);
        } else {
            spdlog::warn("SqlitePlaybackSink::purge: DELETE step rc={} on {}: "
                         "{}", rc, entry.path().string(),
                         sqlite3_errmsg(db));
        }
        sqlite3_finalize(st);
        // Принудительный checkpoint, чтобы изменения попали в main-файл и
        // были видны последующим read-only connection'ам query(). Без него
        // на маленьких WAL-объёмах SQLite может откладывать чекпоинт, и
        // следующий reader увидит устаревший снимок.
        sqlite3_exec(db, "PRAGMA wal_checkpoint(TRUNCATE);",
                     nullptr, nullptr, nullptr);
        sqlite3_close(db);
    }

    return {{"deleted_rows", deleted_rows},
            {"removed_files", removed_files}};
}

nlohmann::json SqlitePlaybackSink::statusJson() const {
    int channels = 0;
    int files = 0;
    {
        std::shared_lock lk(channels_mu_);
        channels = static_cast<int>(channels_.size());
        for (const auto& [_, info] : channels_) {
            const auto pdir = playbackDir(info.channel_dir);
            std::error_code ec;
            if (!fs::is_directory(pdir, ec)) continue;
            for (const auto& entry : fs::directory_iterator(pdir, ec)) {
                if (ec) break;
                if (entry.is_regular_file() &&
                    fileLooksLikeWeekDb(
                        entry.path().filename().string(), nullptr)) {
                    ++files;
                }
            }
        }
    }
    const auto last = last_write_ns_.load(std::memory_order_relaxed);
    return {
        {"sink_type",        "db"},
        {"queue_depth",      queue_.empty() ? 0 : 1},
        {"dropped_count",    dropped_.load(std::memory_order_relaxed)},
        {"last_write_ns",    last == 0 ? nlohmann::json(nullptr)
                                       : nlohmann::json(last)},
        {"channels_count",   channels},
        {"files_count",      files},
        {"schema_errors",    schema_errors_.load(std::memory_order_relaxed)},
        {"schema_version",   kSchemaVersion},
        {"retention_days",   default_retention_days_},
    };
}

}  // namespace liveqx::logging
