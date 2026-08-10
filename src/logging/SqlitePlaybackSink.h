#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <list>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include "IPlaybackSink.h"
#include "utils/SpscQueue.h"

struct sqlite3;
struct sqlite3_stmt;

namespace liveqx::logging {

// Single-process owner of all per-channel proof-of-play SQLite files.
// Files are weekly-partitioned per channel:
//
//   {channel_dir}/playback/db-YYYY-Www.db   // ISO 8601 week, UTC
//
// Channels must be registered with their on-disk dir before log() is called
// for them — that's how the sink knows where to put the files.
//
// Threading:
//   - log() is non-blocking: pushes onto a 32K SPSC queue, drops on overflow,
//     bumps a counter visible in statusJson().
//   - One writer-thread drains the queue, opens DB handles lazily and keeps
//     up to 64 in an LRU cache (evicted handles are closed cleanly).
//   - Same thread runs a periodic retention sweep (every 60s) and unlinks
//     week-files whose mtime is older than channel.retention_days.
//   - query() opens fresh read-only handles per call — multiple readers do
//     not contend with the writer thanks to WAL mode.
class SqlitePlaybackSink final : public IPlaybackSink {
public:
    explicit SqlitePlaybackSink(int default_retention_days = 90);
    ~SqlitePlaybackSink() override;

    SqlitePlaybackSink(const SqlitePlaybackSink&) = delete;
    SqlitePlaybackSink& operator=(const SqlitePlaybackSink&) = delete;

    // Register / update / remove channel before/after its lifecycle.
    // retention_days == 0 → use default_retention_days_.
    void registerChannel(int channel_id, std::filesystem::path channel_dir,
                         int retention_days = 0);
    void unregisterChannel(int channel_id);

    void log(const PlaybackEvent& ev) override;
    nlohmann::json query(const QueryParams& params) override;
    nlohmann::json purge(const PurgeParams& params) override;
    nlohmann::json statusJson() const override;

    // ISO 8601 week label "YYYY-Www" computed from ns since epoch (UTC).
    static std::string weekString(int64_t ns);

    // Границы ISO-недели в ns (UTC), [start, end] включая последнюю
    // наносекунду перед следующим понедельником 00:00.
    // Возвращает false если строка нераспарсилась.
    static bool weekRangeNs(const std::string& week,
                            int64_t* start_ns, int64_t* end_ns);

private:
    static constexpr size_t kQueueSize        = 32768;
    static constexpr size_t kHandleCacheSize  = 64;
    static constexpr int    kSchemaVersion    = 1;
    static constexpr auto   kRetentionInterval = std::chrono::minutes(1);

    struct ChannelInfo {
        std::filesystem::path channel_dir;
        int                   retention_days;
    };

    struct HandleEntry {
        int           channel_id;
        std::string   week;
        sqlite3*      db          = nullptr;
        sqlite3_stmt* insert_stmt = nullptr;
    };

    void writerLoop();
    void runRetentionSweep();

    // Writer-thread only.
    sqlite3* writerHandle(int channel_id, const std::string& week,
                          sqlite3_stmt** insert_stmt);
    static std::string handleKey(int channel_id, const std::string& week);
    void closeAllHandles();

    // Static helpers — open db, ensure schema, etc. Throw on hard failure.
    static void prepareSchema(sqlite3* db);
    static void execOrThrow(sqlite3* db, const char* sql);

    std::filesystem::path dbPath(int channel_id,
                                 const std::filesystem::path& channel_dir,
                                 const std::string& week) const;
    std::filesystem::path playbackDir(
        const std::filesystem::path& channel_dir) const;

    int                                  default_retention_days_;
    std::unordered_map<int, ChannelInfo> channels_;
    mutable std::shared_mutex            channels_mu_;

    SpscQueue<PlaybackEvent, kQueueSize> queue_;
    std::atomic<uint64_t>                dropped_{0};
    std::atomic<uint64_t>                schema_errors_{0};
    std::atomic<int64_t>                 last_write_ns_{0};
    std::atomic<bool>                    running_{true};

    // LRU кэш writer-handle'ов. Обычно трогает только writer-thread, но
    // purge() (REST-thread) тоже эвиктит запись, прежде чем unlink файла —
    // иначе sqlite держит fd и место освободится только после закрытия
    // последнего connection'а. Доступ синхронизирован handles_mu_.
    std::mutex                                                      handles_mu_;
    std::list<HandleEntry>                                          handles_;
    std::unordered_map<std::string, std::list<HandleEntry>::iterator> handle_index_;
    std::chrono::steady_clock::time_point next_retention_at_;

    std::thread writer_thread_;
};

}  // namespace liveqx::logging
