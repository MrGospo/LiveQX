#pragma once
//
// fix17 — durable per-channel state.
//
// `ChannelStatePersistence` owns one SQLite file (`channel_dir/state.db`)
// holding the cursor, paused flag and active schedule window for a
// single channel. The previous JSON-based StatePersistence was removed
// in fix17 step 1 — it didn't satisfy the atomic-on-kill-9 requirement
// from the technical spec, and nothing in the tree referenced it.
//
// This header only declares the public surface; the implementation
// lands in step 2 (sqlite open/save/load + corrupt-db rename).
//
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

struct sqlite3;

namespace liveqx::persistence {

// Snapshot of everything fix17 persists for a single channel. All fields
// are optional so an empty / fresh state.db round-trips into a default
// snapshot — caller treats every nullopt as "no recovered value, use cfg
// or runtime defaults".
struct ChannelStateSnapshot {
    // Active clip pointer. playlist_index is the index into the *current*
    // playlist snapshot at save time; clip_path is the resolved path of
    // that clip and is used as a sanity check during restore — if the
    // playlist was edited offline so the index now points at a different
    // clip, restore falls back to playlist head.
    std::optional<int>          playlist_index;
    std::optional<std::string>  clip_path;
    std::optional<double>       slot_pos_sec;

    // True if the channel was paused (stop()'d) at save time. Used by
    // the bootstrap loop in main.cpp to decide whether to auto-play.
    std::optional<bool>         paused;

    // Active schedule window (fix9). Empty in REGULAR mode.
    //   { "mode": "schedule", "entry_id": int, "window_end_ns": int64 }
    std::optional<nlohmann::json> schedule_active;

    bool empty() const noexcept {
        return !playlist_index && !clip_path && !slot_pos_sec
            && !paused && !schedule_active;
    }
};

// One-per-channel SQLite wrapper. Construction opens (and creates if
// missing) `db_path`; failure to open is logged and leaves the object
// in a "no-op" state — every save() is a no-op and load() returns an
// empty snapshot. This matches the spec line "Corrupt state.db → лог +
// skip + старт с initial state, не падать".
//
// Thread-safety: save() is called from one writer thread (the
// debouncer in ChannelStateSaver). load() is called once during
// channel build, before the saver thread exists. The two never overlap.
class ChannelStatePersistence {
public:
    explicit ChannelStatePersistence(std::filesystem::path db_path);
    ~ChannelStatePersistence();

    ChannelStatePersistence(const ChannelStatePersistence&)            = delete;
    ChannelStatePersistence& operator=(const ChannelStatePersistence&) = delete;

    // True once the SQLite file was opened and the schema applied. False
    // after a corrupt-db rename or an open failure — every save() in
    // that state is a logged no-op.
    bool ok() const noexcept { return db_ != nullptr; }

    // Path of the file we operate on. Stable across the object's lifetime.
    const std::filesystem::path& path() const noexcept { return path_; }

    // BEGIN IMMEDIATE; INSERT OR REPLACE for every populated field of
    // `snap`; COMMIT. Empty snapshot writes nothing (still returns true).
    // Returns false on any SQLite error — caller may log + retry.
    bool save(const ChannelStateSnapshot& snap);

    // Reads every key from channel_state. Missing rows surface as
    // nullopt. On any SQLite error during read, the underlying file is
    // renamed to `<path>.corrupt-<unix_ns>` and an empty snapshot is
    // returned. Subsequent save() calls reopen a fresh file.
    ChannelStateSnapshot load();

private:
    std::filesystem::path path_;
    sqlite3*              db_ = nullptr;

    // Try to (re)open `path_` and apply the schema. Returns true on
    // success and stores the handle in `db_`. On schema/open failure
    // the corrupt file is renamed and the function returns false (db_
    // remains nullptr).
    bool openAndPrepare();
};

}  // namespace liveqx::persistence
