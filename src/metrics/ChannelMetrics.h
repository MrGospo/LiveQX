#pragma once
#include <atomic>
#include <cstdint>
#include "metrics/RollingCounter.h"

// Plain POD copy returned from snapshot() — safe to hand to REST/Prometheus
// exporters without exposing atomic internals.
struct ChannelMetricsSnapshot {
    uint64_t frames_rendered     = 0;
    uint64_t frames_dropped      = 0; // video drop on queue full or invalid getFrame()
    uint64_t audio_underruns     = 0; // getAudio() padded with silence
    uint64_t audio_loop_glitches = 0; // crossfade fallback / FLUSH path on audio
    uint64_t decode_errors       = 0; // avcodec_send_packet / receive_frame failures
    uint64_t loop_fallback_count = 0; // EOF without warm preload context
    int64_t  frame_time_max_us   = 0; // peak frame time in current 60s window
    double   actual_fps          = 0.0;

    // Rolling rates (events / window_seconds), populated from RollingCounters.
    uint64_t drops_10s           = 0;
    uint64_t underruns_10s       = 0;
    uint64_t loop_fallback_10s   = 0;
    uint64_t rendered_10s        = 0; // for rolling fps = rendered_10s / 10
    int64_t  last_tick_ns        = 0; // monotonic ns of latest render tick

    // ── ContentSync (stage 2.10) ─────────────────────────────────────────────
    uint64_t cache_files_count       = 0;
    uint64_t cache_size_bytes        = 0;
    uint64_t pending_deletes         = 0;
    uint64_t share_unreachable_count = 0;
    uint64_t cache_copy_errors       = 0;
    uint64_t oversized_skipped       = 0;
    int64_t  last_share_ok_ns        = 0; // monotonic ns; 0 = share not configured

    // ── fix16 ────────────────────────────────────────────────────────────────
    uint64_t clip_changes      = 0;  // total active-clip transitions on this run
    bool     transition_active = false; // 1 iff render thread is mid-transition
};

// Shared between RenderLoop (writer/reader for render-side counters) and the
// active VideoClip (writer for decode-side counters). Lifetime is owned by
// RenderLoop; clips receive a non-owning view via shared_ptr.
struct ChannelMetrics {
    std::atomic<uint64_t> frames_rendered{0};
    std::atomic<uint64_t> frames_dropped{0};
    std::atomic<uint64_t> audio_underruns{0};
    std::atomic<uint64_t> audio_loop_glitches{0};
    std::atomic<uint64_t> decode_errors{0};
    std::atomic<uint64_t> loop_fallback_count{0};
    std::atomic<int64_t>  frame_time_max_us{0};
    std::atomic<double>   actual_fps{0.0};

    // Rolling 60-bucket × 1s = 60s window. Tick every 1s from a single ticker.
    RollingCounter<60> rolling_drops;
    RollingCounter<60> rolling_underruns;
    RollingCounter<60> rolling_loop_fallback;
    RollingCounter<60> rolling_rendered;

    // Heartbeat: monotonic ns of the latest render tick. Watchdog uses
    // (now_ns - last_tick_ns) to detect stuck render threads.
    std::atomic<int64_t> last_tick_ns{0};

    // ── ContentSync (stage 2.10) ─────────────────────────────────────────────
    // Written by ContentSync thread, read by metrics/health.
    std::atomic<uint64_t> cache_files_count       {0};
    std::atomic<uint64_t> cache_size_bytes        {0};
    std::atomic<uint64_t> pending_deletes         {0};
    std::atomic<uint64_t> share_unreachable_count {0};
    std::atomic<uint64_t> cache_copy_errors       {0};
    std::atomic<uint64_t> oversized_skipped       {0};
    // Wall-clock-ns of the last successful share scan; 0 = never.
    std::atomic<int64_t>  last_share_ok_ns        {0};

    // ── fix16 ────────────────────────────────────────────────────────────────
    // clip_changes: incremented by RenderLoop on every observed active-idx
    // change after the first clip starts (so the first start is *not*
    // counted as a "change"). transition_active: written by RenderLoop each
    // tick from state.in_transition; read lock-free by MetricsCollector.
    std::atomic<uint64_t> clip_changes      {0};
    std::atomic<bool>     transition_active {false};

    // Reset transient render-side counters and rolling windows. Called by
    // ChannelInstance::play() so that a stop/play cycle does not bleed stale
    // cumulative values into the freshly-started RenderLoop's first fps
    // window. ContentSync counters and last_share_ok_ns are NOT touched —
    // those are state of the long-lived sync loop, not per-run metrics.
    // Caller must ensure no writer is active (i.e. RenderLoop is destroyed).
    void resetTransient() noexcept {
        frames_rendered.store(0, std::memory_order_relaxed);
        frames_dropped.store(0, std::memory_order_relaxed);
        audio_underruns.store(0, std::memory_order_relaxed);
        audio_loop_glitches.store(0, std::memory_order_relaxed);
        decode_errors.store(0, std::memory_order_relaxed);
        loop_fallback_count.store(0, std::memory_order_relaxed);
        frame_time_max_us.store(0, std::memory_order_relaxed);
        actual_fps.store(0.0, std::memory_order_relaxed);
        clip_changes.store(0, std::memory_order_relaxed);
        transition_active.store(false, std::memory_order_relaxed);
        rolling_drops.reset();
        rolling_underruns.reset();
        rolling_loop_fallback.reset();
        rolling_rendered.reset();
    }

    // Advance all rolling buckets. Call once per second.
    void tickRolling() noexcept {
        rolling_drops.tick();
        rolling_underruns.tick();
        rolling_loop_fallback.tick();
        rolling_rendered.tick();
    }

    ChannelMetricsSnapshot snapshot() const {
        ChannelMetricsSnapshot s;
        s.frames_rendered     = frames_rendered.load(std::memory_order_relaxed);
        s.frames_dropped      = frames_dropped.load(std::memory_order_relaxed);
        s.audio_underruns     = audio_underruns.load(std::memory_order_relaxed);
        s.audio_loop_glitches = audio_loop_glitches.load(std::memory_order_relaxed);
        s.decode_errors       = decode_errors.load(std::memory_order_relaxed);
        s.loop_fallback_count = loop_fallback_count.load(std::memory_order_relaxed);
        s.frame_time_max_us   = frame_time_max_us.load(std::memory_order_relaxed);
        s.actual_fps          = actual_fps.load(std::memory_order_relaxed);
        s.drops_10s           = rolling_drops.rate(10);
        s.underruns_10s       = rolling_underruns.rate(10);
        s.loop_fallback_10s   = rolling_loop_fallback.rate(10);
        s.rendered_10s        = rolling_rendered.rate(10);
        s.last_tick_ns        = last_tick_ns.load(std::memory_order_relaxed);
        s.cache_files_count       = cache_files_count.load(std::memory_order_relaxed);
        s.cache_size_bytes        = cache_size_bytes.load(std::memory_order_relaxed);
        s.pending_deletes         = pending_deletes.load(std::memory_order_relaxed);
        s.share_unreachable_count = share_unreachable_count.load(std::memory_order_relaxed);
        s.cache_copy_errors       = cache_copy_errors.load(std::memory_order_relaxed);
        s.oversized_skipped       = oversized_skipped.load(std::memory_order_relaxed);
        s.last_share_ok_ns        = last_share_ok_ns.load(std::memory_order_relaxed);
        s.clip_changes            = clip_changes.load(std::memory_order_relaxed);
        s.transition_active       = transition_active.load(std::memory_order_relaxed);
        return s;
    }
};
