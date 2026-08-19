#pragma once
#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <nlohmann/json.hpp>
#include <spdlog/logger.h>

#include "encoding/Encoder.h"
#include "clips/IClip.h"
#include "clips/ClipGraveyard.h"
#include "metrics/ChannelHealth.h"
#include "transitions/ITransition.h"

class FramePool;
class ChannelMetrics;
class ChannelHealth;
class Timeline;
class CpuCompositor;
class OutputManager;
class SrtOutput;
class IOutput;
class RenderLoop;
class ContentSync;
struct ClipBoundaryEvent;
namespace liveqx::profiler { class ChannelProfiler; }
namespace liveqx::logging {
class IPlaybackSink;
class SqlitePlaybackSink;
}
namespace liveqx::scheduling {
class Scheduler;
class ScheduleController;
struct ScheduleEntry;
}
namespace liveqx::persistence {
class ChannelStatePersistence;
class ChannelStateSaver;
struct ChannelStateSnapshot;
}
namespace liveqx::events {
class EventBus;
}
namespace liveqx::preview {
class PreviewManager;
}

// One channel = one full streaming pipeline. Lives in two states:
//   stopped  — long-lived state (timeline, clips, metrics, health) is built,
//              but encoder/srt/loop/content_sync are not yet allocated.
//   running  — runtime objects are alive and producing frames.
//
// play()/stop() are idempotent and may be called repeatedly. Each play()
// rebuilds the runtime stack from the saved Encoder::Config + output config,
// avoiding any reopen-related lifecycle traps in ffmpeg/SRT.
//
// Thread model: build()/play()/stop()/destructor must be called from a single
// "manager" thread. status()/skipToNext()/isRunning() are safe from any thread.
class ChannelInstance {
public:
    // Build a channel.
    //   channel_dir — per-channel directory (fix7). When non-empty, logger
    //                 writes to channel_dir/logs/channel.log and the
    //                 content_source cache_path auto-resolves to
    //                 channel_dir/cache. Empty → legacy paths
    //                 (logs/ch{id}-{name}.log + cache/<leaf>).
    static std::unique_ptr<ChannelInstance> build(
        const nlohmann::json&  cfg,
        std::filesystem::path  channel_dir = {});

    // Per-channel directory the channel was built with (fix7).
    // Empty when built in legacy mode (no per-channel layout).
    const std::filesystem::path& channelDir() const noexcept { return channel_dir_; }

    // Atomically write cfg_ to channel_dir_/config.json (tmp + fsync +
    // rename + dir-fsync). No-op when channel_dir_ is empty (legacy mode).
    // Used by ChannelManager::create on first registration so a freshly
    // created channel always has its config materialised on disk.
    // Throws on I/O error.
    void persistConfig() const;

    ~ChannelInstance();
    ChannelInstance(const ChannelInstance&)            = delete;
    ChannelInstance& operator=(const ChannelInstance&) = delete;

    int                id()   const noexcept { return id_; }
    const std::string& name() const noexcept { return name_; }
    bool               isRunning() const noexcept { return running_.load(std::memory_order_acquire); }

    // Long-lived shared handles needed by Watchdog / ControlApi.
    std::shared_ptr<ChannelMetrics> metrics() const { return metrics_; }
    std::shared_ptr<ChannelHealth>  health()  const { return health_; }
    std::shared_ptr<spdlog::logger> logger()  const { return logger_; }

    // fix26: per-channel profiler. Returned only when the RenderLoop has
    // been built; nullptr while the channel is stopped between play() calls.
    liveqx::profiler::ChannelProfiler* profiler() noexcept;
    const liveqx::profiler::ChannelProfiler* profiler() const noexcept;

    // fix12 c7: per-channel output fan-out summary used by Watchdog to
    // drive the Degraded-on-partial-failure transition. Returns {0,0} when
    // the channel is stopped (no OutputManager) or has no drivers.
    OutputHealthSummary outputsHealth() const;

    // Returns true on successful start. Idempotent: calling play() on a
    // running instance returns false (caller treats as "already running").
    bool play();

    // Tears down runtime objects. Idempotent. Used by both the operator-
    // driven REST DELETE/stop and the process-wide SIGTERM shutdown — the
    // distinction is encoded by paused_intent_ (set explicitly via pause()).
    void stop();

    // fix17: REST stop path. Marks the channel as operator-paused so the
    // next bootstrap honours the explicit stop intent (paused=true persisted
    // to state.db). stopAll()/SIGTERM uses stop() directly so a graceful
    // shutdown does not flip paused — restart auto-resumes a running channel.
    void pause();

    // No-op on empty playlist or stopped channel. Safe from any thread.
    void skipToNext();

    // Apply config patch in-place (bitrate / preset / default_transition /
    // default_photo_duration). Returns false if the patch contains a field
    // that requires a full rebuild (resolution / fps / output port).
    bool updateConfig(const nlohmann::json& patch);

    // ── Playlist API (stage 3.2) ─────────────────────────────────────────────
    // Result codes for playlist mutations.
    enum class PlaylistResult {
        Ok,
        ManagedByContentSync,   // mutation refused — channel is share-driven
        BadJson,                // body has wrong shape (item not an object,…)
        ItemBuildFailed,        // ClipFactory/prepare threw
        IndexOutOfRange,        // DELETE /playlist/{idx} with bad idx
        NotFound,               // notify-deleted: path not in playlist
    };

    // Snapshot of the current playlist as JSON array. Each entry contains
    // path, duration, transition, pending_remove. Safe from any thread.
    nlohmann::json playlistJson() const;

    // Replace the entire playlist (POST /playlist). Each item:
    //   { "path": "...", "duration": 10.0?, "transition": {...}? }
    // duration falls back to default_photo_duration; transition to default.
    // Cursor jumps to the head of the new playlist (advance() will heal).
    // Synchronous: each clip's prepare() runs on the caller thread.
    PlaylistResult replacePlaylist(const nlohmann::json& items);

    // Append items (POST /playlist/append). Cursor unchanged.
    // Returns the index of the first appended item via *out_first_idx if not
    // null; on partial success the failed item is skipped and logged.
    PlaylistResult appendPlaylist(const nlohmann::json& items, int* out_first_idx = nullptr);

    // Remove single entry by index (DELETE /playlist/{idx}). If idx is the
    // active clip — pending_remove is set and Preloader drops it at slot
    // wrap (smooth crossfade to next/fallback). Otherwise the entry is
    // removed from the snapshot immediately.
    PlaylistResult removeAt(int idx, bool* out_was_active = nullptr);

    // Empty the playlist (DELETE /playlist). Timeline switches to fallback.
    PlaylistResult clearPlaylist();

    // External "file deleted on disk" signal (POST /playlist/notify-deleted).
    // Body: { "path": "..." }. Marks every matching entry pending_remove.
    // Active entries play out, non-active are removed immediately. Idempotent.
    PlaylistResult notifyDeleted(const std::string& path);

    // Snapshot suitable for REST responses.
    nlohmann::json status() const;

    // ── Outputs API (fix12 c4-c6) ─────────────────────────────────────────────
    enum class OutputResult {
        Ok,
        BadJson,         // body shape wrong, type unknown, parser failed
        DuplicateId,     // id already used by an existing output
        BuildFailed,     // numa-bound driver instantiation threw
        StartFailed,     // driver->start() returned false on a running channel
        NotFound,        // referenced output id absent (DELETE/PATCH)
    };

    // Add a new output (fix12 c4). Body must be an object {id, type, …}.
    // While the channel is stopped the entry is appended to cfg_["outputs"]
    // (will be picked up on next play); while running it also builds the
    // driver, calls start() and registers it with OutputManager so packets
    // start fanning out immediately. Persists cfg on success.
    OutputResult addOutput(const nlohmann::json& body);

    // Remove an output by id (fix12 c5). On a running channel, the driver's
    // pump thread is stopped+joined and driver->stop() releases the socket
    // before the entry is dropped from cfg_["outputs"]. Persists cfg on
    // success. NotFound when no output with that id is registered.
    OutputResult removeOutput(const std::string& output_id);

    // Replace an output by id (fix12 c6). Implemented as remove+add under
    // the same lock: the existing driver is evicted and stopped, then a new
    // driver is built from `body`. URL id wins — body["id"] may be absent
    // or must match. If the rebuild step fails (BuildFailed/StartFailed),
    // the channel is left without that output and the caller must POST a
    // fresh entry to recover.
    OutputResult patchOutput(const std::string& output_id,
                              const nlohmann::json& body);

    // Snapshot of the current outputs (status()["outputs"] subtree). Always
    // an array; empty when the channel has no outputs yet.
    nlohmann::json outputsJson() const;

    // Single-output status entry (fix12 c9). Returns null when the channel
    // is stopped or no driver with that id is registered.
    nlohmann::json outputStatusJson(const std::string& output_id) const;

    // ── Live inputs (fix13 c8) ───────────────────────────────────────────────
    // Snapshot of every LiveClip currently in this channel's playlist.
    // Returns an empty array when there are no live entries (or no
    // timeline at all). Each entry is the LiveClip's statusJson()
    // payload — id, state, loss/recovered counts, last_packet_ns,
    // duration_ns, warm_up_ns, loss_threshold_ns, plus the underlying
    // input's diagnostics under "input".
    nlohmann::json liveStatusJson() const;

    // ── Watcher (ContentSync) proxies ────────────────────────────────────────
    // Returns null if this channel has no ContentSync configured.
    nlohmann::json watcherStatus() const;
    // Returns false if no ContentSync, true if rescan was scheduled.
    bool requestRescan();

    // ── Playback log (fix8) ──────────────────────────────────────────────────
    // Inject the process-wide SQLite sink. Required for cfg.playback_log.sink
    // == "db". Lifetime owned by caller (ChannelManager / main). Must be
    // called before play().
    void setSqliteSink(liveqx::logging::SqlitePlaybackSink* sink);

    // Materialise the playback sink from cfg.playback_log without waiting for
    // play(). Called by ChannelManager after all setXxxSink() injections so
    // that /playback-log endpoints answer honestly for stopped channels too
    // (otherwise a restart hides all history until the operator hits play).
    // Idempotent — reruns are cheap when cfg didn't change.
    void initPlaybackSink();

    // fix23: inject the process-wide EventBus. Forwards to internal
    // publishers (ChannelHealth, onClipBoundary). Setting nullptr
    // disables event publishing for this channel.
    void setEventBus(liveqx::events::EventBus* bus);

    // fix34 D2.8: inject the process-wide PreviewManager. When set + the
    // channel is running, RenderLoop's frame tap dispatches each composed
    // RGBA frame into preview->onChannelFrame(id, frame). Setting nullptr
    // detaches the tap (no-op for currently-active sessions; the encoder
    // simply stops getting fed). Safe to call before play() or while
    // running — the tap is set on next play() (and cleared on stop()).
    void setPreviewManager(liveqx::preview::PreviewManager* pv);

    // Sink status snapshot (queue depth, drops, sink type, …). Returns
    // {"sink_type":"none"} if playback log is disabled.
    nlohmann::json playbackLogStatusJson() const;

    // Direct query proxy for REST GET /api/channels/{id}/playback-log.
    // Returns {"events":[...]} even for sink "none".
    nlohmann::json queryPlaybackLog(int channel_id,
                                    const std::optional<int64_t>& from_ns,
                                    const std::optional<int64_t>& to_ns,
                                    const std::optional<int64_t>& after_ns,
                                    int limit, int offset) const;

    // Direct purge proxy for REST DELETE /api/channels/{id}/playback-log.
    // Удаляет события воспроизведения этого канала в диапазоне started_at_ns
    // ∈ [from_ns, to_ns] (включительно). nullopt с обеих сторон = вычистить
    // лог канала целиком. Возвращает {"deleted_rows":N, "removed_files":M};
    // для sink "none" — нули.
    nlohmann::json purgePlaybackLog(int channel_id,
                                    const std::optional<int64_t>& from_ns,
                                    const std::optional<int64_t>& to_ns);

    // ── Schedule (fix9 step 6) ───────────────────────────────────────────────
    // GET /api/channels/{id}/schedule — current entries serialised as a JSON
    // array. Always returns an array (possibly empty); never null.
    nlohmann::json scheduleJson() const;

    // GET /api/channels/{id}/schedule/active — diagnostic snapshot of the
    // scheduler decision at now_ns ({mode, entry_id, window_end_ns}). Returns
    // {"mode":"regular", "entry_id":null, "window_end_ns":null} when no
    // schedule entries are active.
    nlohmann::json scheduleActiveJson() const;

    // PUT /api/channels/{id}/schedule — full replacement of the entry list.
    // Wrapper over updateConfig({"schedule": items}); kept as a separate
    // entry-point so REST handlers can use it without leaking the merge-patch
    // shape. Returns false on parse/validation failure (no state mutated).
    bool replaceSchedule(const nlohmann::json& items);

    // GET /api/channels/{id}/schedule/upcoming?within_sec=N — array of
    // {entry_id, starts_at_ns, ends_at_ns} for entries whose next activation
    // falls within `within_sec` seconds of now (sorted ascending). Returns
    // an empty array if no entries fire in that window. within_sec is
    // clamped to [0, 30 days] inside the implementation.
    nlohmann::json scheduleUpcomingJson(int64_t within_sec) const;

    // ── State persistence (fix17) ────────────────────────────────────────────
    // True when this channel was built with a state.db that recorded
    // paused=true at last shutdown. Bootstrap reads this to skip auto-play
    // for channels the operator had explicitly stopped before SIGTERM —
    // otherwise restart would silently undo the stop. Always false in legacy
    // mode (no state.db) and false once the channel has been play()'d.
    bool wasPausedAtLastSave() const noexcept {
        return persisted_paused_at_load_;
    }

private:
    ChannelInstance() = default;

    // Build helpers — called by build() and play() respectively.
    void buildLongLived(const nlohmann::json& cfg);
    bool buildRuntime();

    // Build a single playlist clip from JSON. Throws on ClipFactory failure;
    // caller logs and decides whether to skip or reject. Used by buildLongLived
    // and the Playlist API helpers.
    std::unique_ptr<IClip> buildClipFromItem(const nlohmann::json& item_json,
                                              std::string&         out_path,
                                              TransitionConfig&    out_tc) const;

    // True if this channel's playlist is owned by ContentSync — direct
    // playlist mutations are refused. Read from cfg_, no lock.
    bool managedByContentSync() const;

    // Lock-free internals shared by addOutput / removeOutput / patchOutput.
    // Caller must hold state_mu_.
    OutputResult addOutputLocked(const nlohmann::json& body);
    OutputResult removeOutputLocked(const std::string& output_id);

    int                   id_   = 0;
    std::string           name_;
    std::filesystem::path channel_dir_;   // empty in legacy mode

    // fix31: off-thread destructor pool. Declared FIRST among non-trivial
    // members so it destructs LAST — after Timeline / ContentSync / fallback /
    // every shared_ptr<IClip> has dropped. Every clip that may end up in
    // Timeline (and therefore observed by the render thread) is shared_ptr-
    // wrapped through wrapClip(uniq, graveyard_) so the last drop on the
    // render thread routes destruction off-thread.
    ClipGraveyard         graveyard_;

    // ── Stored config (used to recreate runtime objects on each play) ────────
    nlohmann::json   cfg_;            // full original config; updateConfig mutates
    int              width_  = 1280;
    int              height_ = 720;
    int              fps_    = 25;
    // fix12 c3: outputs are now an array. Each entry of cfg_["outputs"] is
    // an object {id, type, ...type-specific fields}. Legacy configs
    // carrying a singular "output" object are migrated to a single-element
    // outputs[] in buildLongLived (id=="default") and persisted on next
    // write — after one process start the legacy field disappears from
    // disk. Validation per entry happens at buildRuntime time.
    // NUMA node hint for this channel. -1 = no pinning. Read from
    // cfg["numa_node"] in buildLongLived; propagated to VideoClip
    // (via PlaylistItem.numa_node) and to ContentSync::Config.numa_node.
    int              numa_node_   = -1;
    Encoder::Config  enc_cfg_;
    double           preload_sec_ = 4.0;

    // ── Long-lived state (survives stop/play cycles) ─────────────────────────
    std::shared_ptr<FramePool>      decode_pool_;
    std::shared_ptr<FramePool>      render_pool_;
    std::shared_ptr<ChannelMetrics> metrics_;
    std::shared_ptr<ChannelHealth>  health_;
    std::shared_ptr<spdlog::logger> logger_;
    std::unique_ptr<Timeline>       timeline_;
    std::unique_ptr<CpuCompositor>  compositor_;

    // fix13 c9: co-owned with Timeline so OnLossProvider("fallback_clip")
    // can pull frames from the same clip the timeline shows when the
    // playlist is empty. Stays null when the channel was built without a
    // valid fallback image — LiveClip then downgrades to mode=black.
    std::shared_ptr<IClip>          fallback_clip_;

    // ── Runtime objects (alive only between play and stop) ───────────────────
    std::unique_ptr<Encoder>        encoder_;
    // fix12 c3: out_mgr_ owns every driver via shared_ptr; ChannelInstance
    // no longer holds a single out_driver_ unique_ptr. srt_out_ is kept as
    // a raw alias to the *first* SrtOutput driver (if any) so the legacy
    // status field "srt_connected" and the encoder-reset-on-reconnect hook
    // still work. With multiple SRT drivers, only the first one drives the
    // hook — they all share the same encoder so a single keyframe reset
    // covers every SRT receiver anyway.
    SrtOutput*                      srt_out_ = nullptr;
    std::unique_ptr<OutputManager>  out_mgr_;
    std::unique_ptr<RenderLoop>     loop_;
    std::unique_ptr<ContentSync>    content_sync_;

    std::atomic<bool> running_{false};
    mutable std::mutex state_mu_;     // serializes play/stop/updateConfig

    // ── Playback log (fix8) ──────────────────────────────────────────────────
    // setupPlaybackSink resolves cfg.playback_log.sink to one of:
    //   - own_sink_ = NullPlaybackSink   (sink="none" / absent)
    //   - own_sink_ = FilePlaybackSink   (sink="file")
    //   - shared sqlite_sink_ pointer    (sink="db", requires setSqliteSink)
    // effective_sink_ is the IPlaybackSink the boundary-callback writes to.
    void setupPlaybackSink();
    void onClipBoundary(const ClipBoundaryEvent& ev);

    liveqx::logging::SqlitePlaybackSink*    sqlite_sink_   = nullptr;
    liveqx::events::EventBus*               event_bus_     = nullptr;
    liveqx::preview::PreviewManager*        preview_mgr_   = nullptr;
    std::unique_ptr<liveqx::logging::IPlaybackSink> own_sink_;
    liveqx::logging::IPlaybackSink*         effective_sink_ = nullptr;
    // Tracks what setupPlaybackSink() built last so a stop→cfg-edit→play()
    // can swap to the new sink type instead of sticking with whatever was
    // bound on first play(). "" means "never built". Stays in sync with
    // effective_sink_ — both reset together when sink rebuilds.
    std::string current_sink_type_;
    int         current_retention_days_ = 0;

    // ── Schedule (fix9) ──────────────────────────────────────────────────────
    // scheduler_ holds the parsed entries; schedule_ctrl_ tracks the
    // REGULAR↔SCHEDULE state machine and tells onClipBoundary when to swap
    // Timeline's playlist. regular_playlist_items_ is the JSON snapshot of
    // the most-recent regular playlist — used to rebuild clips when a
    // schedule window ends.
    void applyScheduleBoundary(int64_t now_ns);
    void swapToSchedulePlaylist(const std::vector<std::string>& paths,
                                const TransitionConfig& tc);
    void swapToRegularPlaylist();

    // hard_switch poll: ~1Hz jthread that calls schedule_ctrl_->tryHardSwitch
    // and forces a Timeline swap mid-clip when the new window has
    // hard_switch=true. No-op for soft windows. Runs only while the channel
    // is running and only when scheduler_ is configured.
    void startHardSwitchPoll();
    void stopHardSwitchPoll();

    std::unique_ptr<liveqx::scheduling::Scheduler>          scheduler_;
    std::unique_ptr<liveqx::scheduling::ScheduleController> schedule_ctrl_;
    nlohmann::json                                                  regular_playlist_items_ =
        nlohmann::json::array();

    // schedule_swap_mu_ serialises swap operations triggered from the boundary
    // dispatcher and the hard_switch poll thread — Timeline::setPlaylist is
    // already thread-safe but we don't want two parallel clip-builds racing
    // for the same window.
    std::mutex                                                      schedule_swap_mu_;
    std::jthread                                                    hard_switch_poll_;
    std::atomic<bool>                                               hard_switch_stop_{false};

    // ── State persistence (fix17) ────────────────────────────────────────────
    // Live only when channel_dir_ is non-empty. state_persist_ owns
    // channel_dir_/state.db; state_saver_ debounces save triggers and runs
    // a worker thread that calls captureChannelState() + persist.save().
    // state_periodic_poll_ ticks every 2s while running_ to flush the
    // live cursor (kill -9 recovery). Stop order in the destructor:
    // saver→periodic poll→timeline (saver may capture timeline state).
    void                                          requestStateSave();
    void                                          startPeriodicStatePoll();
    void                                          stopPeriodicStatePoll();
    liveqx::persistence::ChannelStateSnapshot captureChannelState() const;

    std::unique_ptr<liveqx::persistence::ChannelStatePersistence> state_persist_;
    std::unique_ptr<liveqx::persistence::ChannelStateSaver>       state_saver_;
    std::jthread                                                          state_periodic_poll_;
    std::atomic<bool>                                                     state_periodic_stop_{false};

    // Pending cursor restore parsed from state.db at build(). Consumed by
    // the first play() call which forwards (idx, slot_pos_sec) to
    // Timeline::restoreCursor. Cleared after the first play() so subsequent
    // stop/play cycles don't replay an old anchor.
    std::optional<int>    pending_restore_idx_;
    std::optional<double> pending_restore_slot_pos_;

    // True when the operator explicitly stopped the channel — pause() sets
    // it, play() clears it, and captureChannelState() reads it for the
    // persisted "paused" field. Initialised from persisted_paused_at_load_
    // so a process restart honours the previous run's intent. Distinct from
    // running_ on purpose: SIGTERM stop() leaves the flag alone so a
    // running-at-shutdown channel auto-resumes on the next bootstrap.
    bool                  paused_intent_ = false;
    bool                  persisted_paused_at_load_ = false;

    // ── Per-channel timezone (fix33 C) ───────────────────────────────────────
    // channel_timezone в cfg может быть:
    //   - absent / null / ""      → inherits_server_tz_=true, eff = server_tz_getter_()
    //   - "Europe/Moscow"         → inherits_server_tz_=false, eff = это значение
    // server_tz_getter_ устанавливает ChannelManager (даёт текущую серверную TZ
    // из TimeConfigRepo). При смене серверной TZ ChannelManager обходит inherit-
    // каналы и вызывает applyServerTimezoneChange() — Scheduler хот-свопит зону.
public:
    using ServerTimezoneGetter = std::function<std::string()>;
    void setServerTimezoneGetter(ServerTimezoneGetter g);
    // Re-resolves effective TZ and pushes it into scheduler_ when the channel
    // currently inherits server TZ. No-op for explicit-override channels.
    void applyServerTimezoneChange();
    bool inheritsServerTimezone() const noexcept { return inherits_server_tz_; }
    std::string effectiveTimezone() const;
private:
    ServerTimezoneGetter  server_tz_getter_;
    bool                  inherits_server_tz_ = true;
};
