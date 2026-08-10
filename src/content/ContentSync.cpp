#include "content/ContentSync.h"

#include <algorithm>
#include <chrono>
#include <unordered_set>
#include <vector>

#include "clips/ClipFactory.h"
#include "clips/PlaylistItem.h"
#include "utils/CpuAffinity.h"
#include "utils/Log.h"

namespace fs = std::filesystem;
using namespace std::chrono;

namespace {
constexpr milliseconds kBackoffMin{2000};
constexpr milliseconds kBackoffMax{30000};
}  // namespace

ContentSync::ContentSync(Config                          cfg,
                         Timeline&                       timeline,
                         ClipGraveyard&                  graveyard,
                         std::function<int()>            active_idx_fn,
                         std::shared_ptr<FramePool>      decode_pool,
                         std::shared_ptr<ChannelMetrics> metrics,
                         std::string                     channel_id)
    : cfg_(std::move(cfg)),
      timeline_(timeline),
      graveyard_(graveyard),
      active_idx_fn_(std::move(active_idx_fn)),
      decode_pool_(std::move(decode_pool)),
      metrics_(std::move(metrics)),
      channel_id_(std::move(channel_id)),
      cache_({cfg_.cache_dir, cfg_.max_file_size_bytes}),
      scanner_(cfg_.share_dir),
      current_backoff_(cfg_.scan_interval) {
    // Mode is determined by whether a cache directory was provided.
    cfg_.use_cache = !cfg_.cache_dir.empty();
}

ContentSync::~ContentSync() { stop(); }

spdlog::logger& ContentSync::lg() noexcept {
    return logger_ ? *logger_ : *spdlog::default_logger();
}

bool ContentSync::appendByPath(const std::string& path,
                               const std::string& display_name) {
    PlaylistItem item;
    item.path             = path;
    item.display_duration = cfg_.default_photo_duration_sec;
    item.transition       = cfg_.default_transition;
    item.numa_node        = cfg_.numa_node;

    try {
        auto clip = ClipFactory::create(item, cfg_.video_width, cfg_.video_height,
                                        decode_pool_, metrics_);
        clip->setLogger(logger_);
        clip->setChannelId(channel_id_);
        if (cfg_.default_transition.mode != TransitionMode::HardCut
                && cfg_.default_transition.duration_sec > 0.0) {
            clip->setHeadBufferSeconds(cfg_.default_transition.duration_sec);
        }
        clip->prepare();
        timeline_.appendClip(wrapClip(std::move(clip), graveyard_),
                             cfg_.default_transition, path);
        lg().info("ContentSync: appended '{}'", display_name);
        return true;
    } catch (const std::exception& e) {
        lg().error("ContentSync: cannot build clip for '{}': {}",
                   display_name, e.what());
        if (metrics_) metrics_->cache_copy_errors.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
}

std::size_t ContentSync::restoreFromDisk() {
    // Passthrough mode has no cache to restore from — startup playlist is
    // built by the first successful tick() against the source dir.
    if (!cfg_.use_cache) return 0;

    const auto restored = cache_.restoreFromDisk();
    if (restored == 0) return 0;

    // Lexicographic order — deterministic startup playlist.
    std::vector<std::string> names;
    names.reserve(restored);
    for (const auto& [name, _] : cache_.listCached()) names.emplace_back(name);
    std::sort(names.begin(), names.end());

    std::size_t appended = 0;
    for (const auto& n : names) {
        auto it = cache_.listCached().find(n);
        if (it == cache_.listCached().end()) continue;
        if (appendByPath(it->second.cache_path, it->second.filename)) ++appended;
    }
    publishMetrics();
    return appended;
}

void ContentSync::start() {
    if (running_.exchange(true)) return;
    thread_ = std::jthread([this](std::stop_token st) { run(st); });
}

void ContentSync::stop() {
    if (!running_.exchange(false)) return;
    thread_.request_stop();
    {
        std::lock_guard lk(wakeup_mu_);
        ++wakeup_seq_;
    }
    wakeup_cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void ContentSync::requestRescan() {
    if (!running_.load(std::memory_order_relaxed)) return;
    {
        std::lock_guard lk(wakeup_mu_);
        ++wakeup_seq_;
    }
    wakeup_cv_.notify_all();
}

nlohmann::json ContentSync::statusJson() const {
    nlohmann::json j;
    j["mode"]                    = cfg_.use_cache ? "cache" : "passthrough";
    j["source_path"]             = cfg_.share_dir.string();
    if (cfg_.use_cache) j["cache_path"] = cfg_.cache_dir.string();
    j["scan_interval_ms"]        = static_cast<std::int64_t>(cfg_.scan_interval.count());
    j["numa_node"]               = cfg_.numa_node;
    j["current_backoff_ms"]      = static_cast<std::int64_t>(current_backoff_.count());
    j["running"]                 = running_.load(std::memory_order_relaxed);

    if (metrics_) {
        j["last_share_ok_ns"]        = metrics_->last_share_ok_ns.load(std::memory_order_relaxed);
        j["share_unreachable_count"] = metrics_->share_unreachable_count.load(std::memory_order_relaxed);
        j["cache_files_count"]       = metrics_->cache_files_count.load(std::memory_order_relaxed);
        j["cache_size_bytes"]        = metrics_->cache_size_bytes.load(std::memory_order_relaxed);
        j["pending_deletes"]         = metrics_->pending_deletes.load(std::memory_order_relaxed);
        j["cache_copy_errors"]       = metrics_->cache_copy_errors.load(std::memory_order_relaxed);
        j["oversized_skipped"]       = metrics_->oversized_skipped.load(std::memory_order_relaxed);
    }
    return j;
}

std::string ContentSync::activeCachePath() const {
    const int idx = active_idx_fn_ ? active_idx_fn_() : -1;
    if (idx < 0) return {};
    auto snap = timeline_.snapshot();
    if (static_cast<size_t>(idx) >= snap->cache_paths.size()) return {};
    return snap->cache_paths[idx];
}

void ContentSync::publishMetrics() {
    if (!metrics_) return;
    if (cfg_.use_cache) {
        metrics_->cache_files_count.store(
            cache_.listCached().size(), std::memory_order_relaxed);
        metrics_->cache_size_bytes.store(
            cache_.cacheSizeBytes(), std::memory_order_relaxed);
        metrics_->cache_copy_errors.store(
            cache_.copyErrors(), std::memory_order_relaxed);
        metrics_->oversized_skipped.store(
            cache_.oversizedSkipped(), std::memory_order_relaxed);
    } else {
        // Passthrough: cache stats are zero by definition.
        metrics_->cache_files_count.store(0, std::memory_order_relaxed);
        metrics_->cache_size_bytes.store(0, std::memory_order_relaxed);
    }

    auto snap = timeline_.snapshot();
    std::uint64_t pending = 0;
    for (auto b : snap->pending_remove) if (b) ++pending;
    metrics_->pending_deletes.store(pending, std::memory_order_relaxed);
}

bool ContentSync::tick() {
    auto diff = scanner_.scan();
    if (diff.share_unreachable) {
        if (metrics_)
            metrics_->share_unreachable_count.fetch_add(1, std::memory_order_relaxed);
        // Re-arm reconciliation: when the share comes back, files may have
        // been deleted while we were offline — those need to be reaped on the
        // first successful scan after recovery.
        need_reconcile_ = true;
        return false;
    }

    // Reconcile against the share's current contents on first reachable
    // scan after start / after reconnect. The scanner's stability rule does
    // not affect us here: we use the raw set of filenames present, so a file
    // mid-upload is still considered "present" and won't be falsely reaped.
    if (need_reconcile_) {
        const auto present = scanner_.currentFilenames();
        std::unordered_set<std::string> share_set(present.begin(), present.end());

        if (cfg_.use_cache) {
            for (const auto& [name, entry] : cache_.listCached()) {
                if (share_set.find(name) == share_set.end()) {
                    if (timeline_.markForRemoval(entry.cache_path)) {
                        lg().info("ContentSync: reconcile — '{}' missing from share, marking for removal",
                                  name);
                    } else if (cache_.evict(name)) {
                        lg().info("ContentSync: reconcile — evicted orphan '{}' from cache", name);
                    }
                }
            }
        } else {
            // Passthrough: walk timeline cache_paths (= source paths here) and
            // mark anything no longer on the source dir.
            auto snap = timeline_.snapshot();
            for (const auto& cp : snap->cache_paths) {
                if (cp.empty()) continue;
                const auto basename = fs::path(cp).filename().string();
                if (share_set.find(basename) == share_set.end()) {
                    if (timeline_.markForRemoval(cp)) {
                        lg().info("ContentSync: reconcile — '{}' missing from source, marking for removal",
                                  basename);
                    }
                }
            }
        }
        need_reconcile_ = false;
    }

    // Helper: timeline path for a given source basename, depending on mode.
    auto timeline_path_for = [&](const std::string& basename) -> std::string {
        return cfg_.use_cache
            ? (cfg_.cache_dir / basename).string()
            : (cfg_.share_dir / basename).string();
    };

    // Mark first — so a remove + add of the same name in one tick doesn't
    // collide with a stale active path.
    for (const auto& name : diff.removed) {
        const auto path = timeline_path_for(name);
        if (timeline_.markForRemoval(path)) {
            lg().info("ContentSync: marked '{}' for removal", name);
        }
    }

    // Snapshot once — used to skip files already represented in the timeline.
    auto tl_snap = timeline_.snapshot();
    auto already_in_timeline = [&](const std::string& p) {
        for (const auto& q : tl_snap->cache_paths) if (q == p) return true;
        return false;
    };

    for (const auto& share_path : diff.added) {
        const auto basename = share_path.filename().string();
        const auto tl_path  = timeline_path_for(basename);
        if (already_in_timeline(tl_path)) continue;

        if (cfg_.use_cache) {
            auto entry = cache_.ingest(share_path);
            if (!entry.has_value()) continue;
            appendByPath(entry->cache_path, entry->filename);
        } else {
            // Passthrough — feed the source path directly.
            appendByPath(share_path.string(), basename);
        }
    }

    // Reap any pending entries that are not actively playing.
    auto evicted = timeline_.reapRemovable(activeCachePath());
    for (const auto& p : evicted) {
        if (!cfg_.use_cache) continue;  // nothing to evict in passthrough mode
        const auto basename = fs::path(p).filename().string();
        if (cache_.evict(basename)) {
            lg().info("ContentSync: evicted '{}' from cache", basename);
        }
    }

    if (metrics_) {
        const auto now_ns = duration_cast<nanoseconds>(
            steady_clock::now().time_since_epoch()).count();
        metrics_->last_share_ok_ns.store(now_ns, std::memory_order_relaxed);
    }
    publishMetrics();
    return true;
}

void ContentSync::run(std::stop_token st) {
    numa::bindCurrentThreadToNode(cfg_.numa_node);
    Log::setThreadName("ch" + channel_id_ + "-csync");
    lg().info("ContentSync: starting (mode={}, source='{}', cache='{}', interval={}ms, numa_node={})",
              cfg_.use_cache ? "cache" : "passthrough",
              cfg_.share_dir.string(), cfg_.cache_dir.string(),
              cfg_.scan_interval.count(), cfg_.numa_node);

    while (!st.stop_requested() && running_.load(std::memory_order_relaxed)) {
        const bool ok = tick();
        if (ok) {
            current_backoff_ = cfg_.scan_interval;
        } else {
            // Exponential backoff for unreachable share, capped.
            current_backoff_ = std::min<milliseconds>(current_backoff_ * 2, kBackoffMax);
            if (current_backoff_ < kBackoffMin) current_backoff_ = kBackoffMin;
            lg().warn("ContentSync: share unreachable, backoff={}ms",
                      current_backoff_.count());
        }
        // CV-based sleep so requestRescan() / stop() wake us promptly.
        std::unique_lock lk(wakeup_mu_);
        const auto seq = wakeup_seq_;
        wakeup_cv_.wait_for(lk, current_backoff_, [&] {
            return st.stop_requested() ||
                   wakeup_seq_ != seq ||
                   !running_.load(std::memory_order_relaxed);
        });
    }
    lg().info("ContentSync: stopped");
}
