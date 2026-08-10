#pragma once
#include <future>
#include <memory>
#include <vector>
#include <spdlog/logger.h>
#include "core/Timeline.h"

// Manages async prepare()/release() lifecycle for playlist clips.
// Call tick() once per render frame — it never blocks (all .get() are
// guarded by wait_for(0s) == ready before calling).
class Preloader {
public:
    // preload_sec: how many seconds before clip end to start prepare().
    // Should be > transition duration so clipB is ready when crossfade starts.
    Preloader(Timeline& timeline, double preload_sec);
    ~Preloader();

    // Reads cursor state from Timeline (peek/getActiveIndex/getRemainingTime).
    // Designed to be called from the same thread that drives Timeline::advance.
    void tick();

    // Per-channel logger. If unset, falls back to spdlog::default_logger().
    void setLogger(std::shared_ptr<spdlog::logger> lg) { logger_ = std::move(lg); }

    // fix30: warm-set policy. The clips returned here must remain prepared at
    // every tick — section 3 (preload) drives prepare for any unprepared
    // member, section 4 (release) refuses to release any member. cur_idx is
    // clamped into [0, n); negative or out-of-range values fold via modulo.
    // Returns indices in priority order: active first, then forward neighbours
    // wrapping around the playlist. For n ≤ cap the entire playlist is warm
    // (the original release-on-wrap pingpong is a fix30 regression case).
    static std::vector<int> computeWarmSet(int n, int cur_idx, int cap);

    // Fixed warm-set capacity for fix30 phase 1 — kept const to make the
    // policy transparent. If a per-channel override becomes needed, expose
    // via constructor or setter without changing the policy contract.
    static constexpr int kWarmCap = 2;

private:
    spdlog::logger& lg() noexcept;

    Timeline& timeline_;
    double    preload_sec_;

    // Active-clip identity tracked by pointer (via shared_ptr).
    // Indices shift whenever the playlist mutates (appendClip, reapRemovable
    // change total length → fmod-based getCurrentIndex maps the same elapsed
    // time to a different number). Pointer identity is stable across mutations.
    // shared_ptr keeps the clip alive until release() finishes.
    std::shared_ptr<IClip> last_clip_;
    std::shared_ptr<IClip> preloading_clip_;
    std::shared_ptr<IClip> prepared_for_clip_;  // sentinel: which active clip we already preloaded for
    std::shared_ptr<IClip> releasing_clip_;

    std::future<void> preload_future_;
    std::future<void> release_future_;

    int64_t preload_started_ns_ = 0;
    int64_t release_started_ns_ = 0;
    int     preloading_log_idx_ = -1;
    int     releasing_log_idx_  = -1;

    // For single-clip loops cur_clip pointer is constant across cycles, so
    // the prepared_for_clip_ sentinel never naturally clears on a slot
    // boundary. Track previous remaining time: when it jumps upward we just
    // wrapped — invalidate the sentinel so the next cycle's reset() fires.
    double  prev_remaining_sec_ = 0.0;

    // True when the in-flight preload_future_ is a single-clip wrap reset
    // for a HardCut loop. Triggers snapCursorToActiveStart() once the
    // future resolves to align the cursor with the freshly-reset decoder.
    bool    pending_wrap_snap_  = false;

    // Set when a single-clip transition loop has entered head-playback mode
    // and frozen the tail. On natural slot wrap (detected via remaining-jump)
    // we exit head playback and unfreeze — the live decoder is already at
    // t=0 (from the parallel reset()) so cursor and decoder line up at
    // slot_pos=0 with no extra adjustment.
    bool    pending_head_loop_ = false;

    std::shared_ptr<spdlog::logger> logger_;
};
