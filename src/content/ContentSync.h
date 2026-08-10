#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <spdlog/logger.h>
#include <string>
#include <thread>

#include "clips/IClip.h"
#include "clips/ClipGraveyard.h"
#include "content/CacheManager.h"
#include "content/ShareScanner.h"
#include "core/FramePool.h"
#include "core/Timeline.h"
#include "metrics/ChannelMetrics.h"
#include "transitions/ITransition.h"

// ContentSync wires the share → cache → Timeline pipeline. It owns one
// background jthread per channel that:
//
//   1. polls the share via ShareScanner
//   2. for each newly-stable file: copies into cache, validates, builds an
//      IClip, appends to Timeline (hot mutation)
//   3. for each disappeared file: marks the matching Timeline entry for
//      removal (clip continues playing until it leaves the active slot)
//   4. reaps removable entries on every tick, passing the currently-active
//      cache path so the playing clip is preserved
//
// Threading: a single internal jthread owns CacheManager + ShareScanner.
// All other components are accessed through their thread-safe APIs.
//
// Lifetime: must be stop()'d before Timeline / RenderLoop destruction.
class ContentSync {
public:
    struct Config {
        std::filesystem::path share_dir;       // source dir (SMB or local)
        std::filesystem::path cache_dir;       // empty in passthrough mode
        std::uint64_t         max_file_size_bytes = 0;
        std::chrono::milliseconds scan_interval{2000};
        // Defaults applied to clips built from the share.
        int              video_width  = 1280;
        int              video_height = 720;
        double           default_photo_duration_sec = 10.0;
        TransitionConfig default_transition{};

        // NUMA node hint for the watcher thread and every clip built by
        // appendByPath. -1 = no pinning. Wired by ChannelInstance from the
        // channel's numa_node config.
        int              numa_node = -1;

        // Mode flag — derived from cache_dir at construction. true: copy
        // into cache_dir, Timeline gets cache paths (SMB / NFS protection).
        // false: feed source paths directly to Timeline (local folder).
        bool use_cache = true;
    };

    // active_idx_fn: returns the index of the currently-playing clip in
    // Timeline (or -1). RenderLoop's activeClipIndex() is the production
    // source; tests inject a controllable lambda.
    ContentSync(Config                          cfg,
                Timeline&                       timeline,
                ClipGraveyard&                  graveyard,
                std::function<int()>            active_idx_fn,
                std::shared_ptr<FramePool>      decode_pool,
                std::shared_ptr<ChannelMetrics> metrics,
                std::string                     channel_id);

    ~ContentSync();

    ContentSync(const ContentSync&)            = delete;
    ContentSync& operator=(const ContentSync&) = delete;

    // One-shot: scan cache_dir, build clips for each existing file, append to
    // Timeline in lexicographic order. Returns count of clips appended.
    // Call before start() so the playlist is warm at first frame.
    std::size_t restoreFromDisk();

    void start();
    void stop();

    bool isRunning() const noexcept { return running_.load(std::memory_order_relaxed); }

    // Public for tests — drives a single scan-ingest-reap cycle inline.
    // Returns true on successful share scan, false on share_unreachable.
    bool tick();

    // True when a cache directory is configured. False = passthrough mode
    // (Timeline references source paths directly, no cache copy).
    bool usesCache() const noexcept { return cfg_.use_cache; }

    // Wake the run() loop out of its sleep slice and force an immediate scan.
    // Safe from any thread; no-op if start() has not been called.
    void requestRescan();

    // Snapshot of the watcher state for /watcher/status. Safe from any thread.
    nlohmann::json statusJson() const;

    // Per-channel logger. If unset, falls back to spdlog::default_logger().
    void setLogger(std::shared_ptr<spdlog::logger> lg) { logger_ = std::move(lg); }

private:
    spdlog::logger& lg() noexcept;
    void run(std::stop_token st);

    // Append a clip to Timeline. `path` is whatever the configured mode
    // exposes — cache absolute path in cache mode, source absolute path in
    // passthrough mode. `display_name` is for logs only.
    bool appendByPath(const std::string& path, const std::string& display_name);

    // Active cache path for reapRemovable() — empty if RenderLoop hasn't
    // produced a frame yet.
    std::string activeCachePath() const;

    // Push current cache stats into ChannelMetrics.
    void publishMetrics();

    Config                            cfg_;
    Timeline&                         timeline_;
    ClipGraveyard&                    graveyard_;
    std::function<int()>              active_idx_fn_;
    std::shared_ptr<FramePool>        decode_pool_;
    std::shared_ptr<ChannelMetrics>   metrics_;
    std::string                       channel_id_;

    CacheManager  cache_;
    ShareScanner  scanner_;

    std::atomic<bool>       running_{false};
    std::jthread            thread_;

    // CV-based wakeup. requestRescan() bumps wakeup_seq_ and notifies; the
    // run loop wakes early from wait_for() and runs an extra tick().
    mutable std::mutex      wakeup_mu_;
    std::condition_variable wakeup_cv_;
    std::uint64_t           wakeup_seq_{0};

    // Backoff state (reset on first successful scan).
    std::chrono::milliseconds current_backoff_;

    // Share-vs-cache reconciliation flag. true means the next reachable scan
    // must reconcile cache against the share (mark anything in cache that is
    // no longer present on the share for removal). Set on construction and
    // re-armed every time the share goes unreachable, so a network blip
    // followed by deletions on the share is caught on reconnect rather than
    // never.
    bool need_reconcile_ = true;

    std::shared_ptr<spdlog::logger> logger_;
};
