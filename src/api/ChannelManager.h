#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

#include "metrics/ChannelProfiler.h"

class ChannelInstance;
class Watchdog;
namespace liveqx::logging { class SqlitePlaybackSink; }
namespace liveqx::events  { class EventBus; }
namespace liveqx::preview { class PreviewManager; }
namespace liveqx::profiler {
    class ProfileSampler;  // ChannelProfiler is defined in the included header.
}

// Owns the live set of channels. CRUD + transport methods are thread-safe and
// return result codes (no throws on bad ids — REST handlers translate to 4xx).
class ChannelManager {
public:
    enum class Result {
        Ok,
        NotFound,
        AlreadyExists,
        AlreadyRunning,
        AlreadyStopped,
        BuildFailed,
        StartFailed,
        BadPatch,
        // Playlist-specific
        ManagedByContentSync,   // 409 — share-driven channel
        BadJson,                // 400 — body shape wrong
        ItemBuildFailed,        // 400 — at least one clip failed prepare
        IndexOutOfRange,        // 404 — bad playlist idx
        PathNotFound,           // 404 — notify-deleted path absent
        // Outputs-specific (fix12 c4-c6)
        OutputIdConflict,       // 409 — outputs[].id already used
        OutputBuildFailed,      // 400 — driver instantiation/parse threw
        OutputStartFailed,      // 500 — driver->start() returned false
        OutputNotFound,         // 404 — referenced output id absent
    };

    // channel_root: optional directory under which per-channel folders
    // (fix7) are created. When empty, channels are built without a
    // per-channel dir (legacy paths). main.cpp passes the value of
    // global config's "channel_root_dir" (default "channels").
    explicit ChannelManager(Watchdog* watchdog = nullptr,
                            std::filesystem::path channel_root = {});
    ~ChannelManager();

    ChannelManager(const ChannelManager&)            = delete;
    ChannelManager& operator=(const ChannelManager&) = delete;

    // Build the channel from cfg and insert it. If cfg has no "id", picks
    // max(existing)+1 (or 1 if empty). On success, *out_id receives the chosen
    // id. Channel starts in stopped state — call play() separately.
    Result create(const nlohmann::json& cfg, int* out_id = nullptr);

    // Auto-played path: same as create() + play() under one lock; used by the
    // config bootstrap so default.json channels come up running. Errors during
    // play are logged but do not roll back the create.
    Result createAndPlay(const nlohmann::json& cfg, int* out_id = nullptr);

    Result remove(int id);
    Result play(int id);
    Result stop(int id);
    Result next(int id);
    Result updateConfig(int id, const nlohmann::json& patch);

    // ── Playlist API (delegated to ChannelInstance) ─────────────────────────
    nlohmann::json playlistJson(int id) const;          // null if channel not found
    Result replacePlaylist(int id, const nlohmann::json& items);
    Result appendPlaylist(int id, const nlohmann::json& items, int* out_first_idx = nullptr);
    Result removeAt(int id, int idx, bool* out_was_active = nullptr);
    Result clearPlaylist(int id);
    Result notifyDeleted(int id, const std::string& path);

    // ── Playback log (fix8) ─────────────────────────────────────────────────
    // Inject the process-wide SQLite sink. Forwarded to every existing channel
    // immediately and to every channel created afterwards. May be called once
    // at startup; passing nullptr clears the injection (then any "db" channels
    // fall back to NullPlaybackSink + logged error).
    void setSqlitePlaybackSink(liveqx::logging::SqlitePlaybackSink* sink);

    // fix23: inject the process-wide event bus. Forwarded to every existing
    // channel immediately and to every channel created afterwards. nullptr
    // disables event publishing (used by tests and by builds without the
    // SSE endpoint enabled).
    void setEventBus(liveqx::events::EventBus* bus);

    // fix34 D2.8: inject the process-wide PreviewManager. Wires the render-
    // loop frame tap into preview->onChannelFrame for every existing and
    // future ChannelInstance. nullptr detaches (sessions stop receiving
    // frames; the preview encoder is torn down on next stop).
    void setPreviewManager(liveqx::preview::PreviewManager* pv);

    // fix26: inject the global profile sampler. Already-running channels are
    // (un)registered on the spot; channels created after this call are
    // wired automatically. Pass nullptr to detach.
    void setProfileSampler(liveqx::profiler::ProfileSampler* sampler);

    // fix33 C — server TZ provider for channel TZ inheritance.
    // Колл-бэк должен возвращать актуальную серверную IANA TZ (например,
    // из TimeConfigRepo). Используется ChannelInstance, когда канал не
    // задал собственный channel_timezone. notifyServerTimezoneChanged()
    // обходит inherit-каналы и хот-свопит Scheduler::channel_tz_.
    using ServerTimezoneGetter = std::function<std::string()>;
    void setServerTimezoneGetter(ServerTimezoneGetter g);
    void notifyServerTimezoneChanged();

    // fix26: lookup a channel's profiler by id. Returns nullptr if the
    // channel is unknown or stopped (no RenderLoop yet). Caller must NOT
    // hold the result past the next remove(id) — the lifetime is tied to
    // the channel.
    liveqx::profiler::ChannelProfiler* profilerFor(int id) noexcept;

    // fix26 c5: REST handlers. profilerStart switches a *running* channel's
    // profiler to the given mode (Sampling or Instrumentation). reset=true
    // also clears accumulated counters. profilerStop returns it to Off and
    // PRESERVES accumulators so callers can fetch the last snapshot via
    // profilerSnapshotJson. Both return AlreadyStopped if the channel is
    // not running (no RenderLoop ⇒ no profiler), NotFound if the id is
    // unknown, and BadPatch if mode is Off (use stop instead).
    Result profilerStart(int id, liveqx::profiler::Mode mode,
                         bool reset);
    Result profilerStop(int id);
    nlohmann::json profilerSnapshotJson(int id) const;

    // Sink status snapshot for one channel. Returns null if id unknown.
    nlohmann::json playbackLogStatusJson(int id) const;

    // Query proof-of-play rows. Returns null if id unknown, otherwise the
    // sink's response shape ({events:[...], next_after_ns}). limit is clamped
    // to [1, 1000] inside the sink.
    nlohmann::json queryPlaybackLog(int id,
                                    const std::optional<int64_t>& from_ns,
                                    const std::optional<int64_t>& to_ns,
                                    const std::optional<int64_t>& after_ns,
                                    int limit, int offset) const;

    // Purge proof-of-play rows. Returns null if id unknown, otherwise
    // {"deleted_rows":N, "removed_files":M}. Range — закрытый интервал
    // [from_ns, to_ns]; nullopt с обеих сторон = удалить весь лог канала.
    nlohmann::json purgePlaybackLog(int id,
                                    const std::optional<int64_t>& from_ns,
                                    const std::optional<int64_t>& to_ns);

    // ── Outputs API (fix12 c4-c6) ───────────────────────────────────────────
    // List outputs for a channel. Returns null if id unknown, otherwise an
    // array (possibly empty). When the channel is running, each entry
    // includes per-driver runtime fields (queue_drops, packets_sent, …).
    nlohmann::json outputsJson(int id) const;

    // Single-output status (fix12 c9). Returns null if either the channel
    // or the output id is unknown — REST handler maps both to 404.
    nlohmann::json outputStatusJson(int id, const std::string& output_id) const;

    // ── Live inputs (fix13 c8) ──────────────────────────────────────────────
    // Returns null on unknown id; otherwise a (possibly empty) JSON array
    // of per-LiveClip status objects. Surfaced by REST as
    // GET /api/channels/{id}/live-status.
    nlohmann::json liveStatusJson(int id) const;

    // Append a new output to the channel. Body must be {id, type, …}.
    // Result mapping: BadJson / OutputIdConflict / OutputBuildFailed /
    // OutputStartFailed / NotFound.
    Result addOutput(int id, const nlohmann::json& body);

    // Remove an output from the channel (fix12 c5).
    // Result mapping: Ok / NotFound (channel) / OutputNotFound (output id).
    Result removeOutput(int id, const std::string& output_id);

    // Patch (replace) an output (fix12 c6). Body is the new full output cfg.
    // Result mapping: Ok / NotFound (channel) / OutputNotFound /
    // BadJson / OutputBuildFailed / OutputStartFailed.
    Result patchOutput(int id, const std::string& output_id,
                       const nlohmann::json& body);

    // ── Watcher (ContentSync) proxies ────────────────────────────────────────
    // Returns null if channel not found OR has no ContentSync.
    nlohmann::json watcherStatus(int id) const;
    // Result::Ok if rescan scheduled, NotFound if channel/watcher absent.
    Result requestRescan(int id);

    // ── Schedule (fix9 step 6) ──────────────────────────────────────────────
    // Returns null on unknown id; otherwise the JSON array of entries
    // (possibly empty).
    nlohmann::json scheduleJson(int id) const;
    // Returns null on unknown id; otherwise {mode, entry_id, window_end_ns}.
    nlohmann::json scheduleActiveJson(int id) const;
    // Full replacement of the schedule. Returns BadPatch on parse/validation
    // failure, NotFound if the channel does not exist.
    Result replaceSchedule(int id, const nlohmann::json& items);
    // Returns null on unknown id; otherwise the JSON array (possibly empty)
    // of upcoming activations within `within_sec` seconds of now.
    nlohmann::json scheduleUpcomingJson(int id, int64_t within_sec) const;

    nlohmann::json listJson() const;
    nlohmann::json statusJson(int id) const;   // returns null if not found
    nlohmann::json healthJson() const;         // /api/health response body

    // fix21: aggregated counters for sd_notify STATUS=… keep-alive line.
    // Cheap O(channels) walk under shared_lock — designed to be polled
    // every WATCHDOG_USEC/2 from the SystemdNotifier loop.
    struct StatusSnapshot {
        int channels      = 0;   // total registered channels
        int running       = 0;   // ChannelHealth::Running
        int degraded      = 0;   // ChannelHealth::Degraded
        int failed        = 0;   // ChannelHealth::Failed
        int outputs_ok    = 0;   // sum of OutputHealthSummary.healthy
        int outputs_total = 0;   // sum of OutputHealthSummary.total
    };
    StatusSnapshot snapshotForStatus() const;

    // Free-standing for testability — formats the line systemd-status will
    // surface via `systemctl status liveqx`.
    static std::string formatStatusLine(const StatusSnapshot& s);

    // Iterate registered channels under a shared_lock. fn must not call
    // mutating ChannelManager methods (would deadlock). Used by metrics /
    // status collectors that need to read multiple per-channel attributes
    // without committing to a heavy listJson / statusJson roundtrip per id.
    void forEachChannel(
        const std::function<void(const ChannelInstance&)>& fn) const;

    // fix7: scan channel_root_/ for ch{id}-*/config.json and bootstrap
    // each as a stopped channel. Broken or unparseable folders are
    // skipped and logged — the rest still come up. Returns the number
    // of successfully loaded channels. No-op when channel_root_ is
    // empty (legacy mode). Caller decides whether to play() afterwards.
    std::size_t loadFromRoot();

    // Stop every channel. Used during shutdown.
    void stopAll();

    // For tests: total channel count.
    std::size_t size() const;

    // ── fix16 readiness signals ─────────────────────────────────────────────
    // True after loadFromRoot() has finished its disk scan (regardless of
    // how many channels were actually loaded — an empty channel root is a
    // valid "ready" state). /readyz reads this lock-free.
    bool isLoaded() const noexcept {
        return loaded_.load(std::memory_order_acquire);
    }

    // fix17: flipped to false at process start. Bootstrap calls
    // setStatePersistenceLoaded(true) once every per-channel state.db has
    // been opened and the cursor anchors restored. /readyz blocks until then
    // so a Kubernetes rolling restart does not shift traffic to a pod that
    // hasn't replayed its persisted state yet.
    bool isStatePersistenceLoaded() const noexcept {
        return state_persistence_loaded_.load(std::memory_order_acquire);
    }
    void setStatePersistenceLoaded(bool v) noexcept {
        state_persistence_loaded_.store(v, std::memory_order_release);
    }

    // fix22 (auth.db) toggles this analogously. Default true.
    bool isAuthLoaded() const noexcept {
        return auth_loaded_.load(std::memory_order_acquire);
    }
    void setAuthLoaded(bool v) noexcept {
        auth_loaded_.store(v, std::memory_order_release);
    }

private:
    int chooseIdLocked(const nlohmann::json& cfg) const;
    ChannelInstance* findLocked(int id) const;

    // Compute the per-channel directory for the given (id, name) using the
    // sanitized "ch{id}-{name}" pattern. Returns empty path when
    // channel_root_ is empty (legacy mode).
    std::filesystem::path channelDirFor(int id, const std::string& name) const;

    mutable std::shared_mutex                          mu_;
    std::map<int, std::unique_ptr<ChannelInstance>>    channels_;
    Watchdog*                                          watchdog_;
    std::filesystem::path                              channel_root_;
    liveqx::logging::SqlitePlaybackSink*       sqlite_sink_ = nullptr;
    liveqx::events::EventBus*                  event_bus_   = nullptr;
    liveqx::preview::PreviewManager*           preview_mgr_ = nullptr;
    liveqx::profiler::ProfileSampler*          sampler_     = nullptr;
    ServerTimezoneGetter                               server_tz_getter_;

    // fix16 readiness flags. loaded_ flips once at end of loadFromRoot();
    // the two persistence flags default to true and are toggled by future
    // subsystems (fix17 / fix22) before they unblock readiness.
    std::atomic<bool> loaded_                  {false};
    std::atomic<bool> state_persistence_loaded_{false};
    std::atomic<bool> auth_loaded_             {true};
};

const char* channelManagerResultName(ChannelManager::Result r) noexcept;
