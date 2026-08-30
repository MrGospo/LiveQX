#pragma once
#include <atomic>
#include <cstdint>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <spdlog/logger.h>
#include "core/Preloader.h"
#include "core/Timeline.h"
#include "metrics/ChannelMetrics.h"
#include "metrics/ChannelProfiler.h"
#include "render/ICompositor.h"
#include "transitions/ITransition.h"
#include "utils/SpscQueue.h"

class Encoder;
class AudioMixer;

// Emitted by RenderLoop on every clip boundary (active_idx change). The
// dispatcher thread invokes BoundaryCb sequentially, so subscribers may do
// blocking work (file IO, sqlite insert) without affecting render-thread
// timing. The render-thread only does a non-blocking try_push.
struct ClipBoundaryEvent {
    int            prev_idx       = -1;     // index of clip that just ended
    std::string    prev_path;                // cache_path or empty
    std::string    prev_type;                // "image" | "video" | "fallback"
    int64_t        started_at_ns  = 0;       // wall-clock (system_clock)
    int64_t        ended_at_ns    = 0;
    double         played_sec     = 0.0;
    TransitionType transition     = TransitionType::CrossFade;
    std::string    status         = "completed";  // overridden in fix8 step 8
    std::string    error_reason;
};

class RenderLoop {
public:
    RenderLoop(int fps, Timeline& timeline,
               ICompositor& compositor, Encoder& encoder,
               std::shared_ptr<ChannelMetrics> metrics = nullptr);
    ~RenderLoop();

    void start();
    void stop();

    // Must be called before start(). Enables background preload/release of clips.
    // preload_sec should exceed the longest transition duration.
    void enablePreloader(double preload_sec);

    // Channel identifier used in 60s metric logs (default: "0").
    void setChannelId(std::string id);

    // NUMA hint applied at the start of run(). -1 = OS scheduler.
    void setNumaNode(int node) noexcept { numa_node_ = node; }

    // Per-channel logger. If unset, falls back to spdlog::default_logger().
    // Also forwarded to the embedded Preloader.
    void setLogger(std::shared_ptr<spdlog::logger> lg);

    // Atomic snapshot of current metrics — safe to call from any thread.
    ChannelMetricsSnapshot getMetrics() const;

    // Shared metrics handle — VideoClip increments its decode-side counters
    // (loop_fallback_count, audio_loop_glitches, decode_errors, frames_dropped)
    // through this. Lifetime owned by RenderLoop.
    std::shared_ptr<ChannelMetrics> metrics() const { return metrics_; }

    // When called before start(), writes raw interleaved float32 stereo 48kHz
    // PCM to `path` for offline analysis (Audacity: 32-bit float, 2ch, 48000 Hz).
    void enablePcmDump(const std::string& path);

    bool isRunning() const { return running_.load(); }

    // Index of the currently-playing clip (clipA) in the active Timeline
    // snapshot, or -1 if not yet rendering. Read-safe from any thread —
    // ContentSync uses this to refuse to evict an active cache file.
    int activeClipIndex() const noexcept {
        return active_idx_.load(std::memory_order_relaxed);
    }

    // Per-channel profiler. Lives on the render thread but is safe to
    // access from any thread for snapshot / mode change. Off by default —
    // hot path is a single relaxed atomic load when not in use.
    liveqx::profiler::ChannelProfiler& profiler() noexcept {
        return profiler_;
    }
    const liveqx::profiler::ChannelProfiler& profiler() const noexcept {
        return profiler_;
    }

    // Subscribe to clip-boundary events. The callback runs on a dedicated
    // dispatcher thread, never on the render-thread. May be called before
    // start(); replacing while running() drops in-flight events.
    using BoundaryCb = std::function<void(const ClipBoundaryEvent&)>;
    void onClipBoundary(BoundaryCb cb);

    // fix34 D2.8 — frame tap for the preview pipeline. Invoked synchronously
    // on the render thread after composite() produces the final RGBA frame
    // and BEFORE the main Encoder.pushFrame(). The tap must not block (it's
    // on the hot path); the consumer is expected to enqueue / fan-out cheaply
    // — for PreviewManager that's a mutex + ffmpeg encode (acceptable for the
    // 25fps × 854×480 preview budget). Nullptr disables the tap.
    using FrameTap = std::function<void(const Frame&)>;
    void setFrameTap(FrameTap tap);

private:
    static constexpr size_t kBoundaryQueueSize = 1024;
    void run(std::stop_token st);
    spdlog::logger& lg() noexcept;

    Frame safeGetFrame(IClip* clip);
    AudioFrame safeGetAudio(IClip* clip, int target_samples);
    Frame safeGetTailFrame(IClip* clip);
    AudioFrame safeGetTailAudio(IClip* clip, int target_samples);

    int fps_;
    Timeline& timeline_;
    ICompositor& compositor_;
    Encoder& encoder_;

    std::atomic<bool> running_{false};
    std::jthread thread_;

    Frame last_good_frame_;
    AudioFrame last_good_audio_;

    std::string           pcm_dump_path_;
    std::ofstream         pcm_dump_;

    int64_t               total_audio_samples_ = 0; // master clock: elapsed = this / 48000.0
    std::optional<Preloader> preloader_;

    // ── Metrics ──────────────────────────────────────────────────────────────
    std::string           channel_id_{"0"};
    std::shared_ptr<ChannelMetrics> metrics_;

    // Updated every tick from the render thread; -1 until first frame.
    std::atomic<int> active_idx_{-1};

    int numa_node_ = -1;
    std::shared_ptr<spdlog::logger> logger_;

    liveqx::profiler::ChannelProfiler profiler_;

    // ── Clip-boundary hook ──────────────────────────────────────────────────
    // Producer (render-thread) → bounded SPSC → dispatcher-thread → subscriber.
    // Render-thread never blocks on the subscriber.
    // fix34 D2.8 — frame tap, set once before start() (or hot from any
    // thread; we read under tap_mu_ on render-thread edge to avoid torn
    // std::function).
    FrameTap              frame_tap_;
    mutable std::mutex    frame_tap_mu_;

    SpscQueue<ClipBoundaryEvent, kBoundaryQueueSize> boundary_q_;
    std::atomic<bool>     boundary_running_{false};
    std::jthread          boundary_dispatcher_;
    BoundaryCb            boundary_cb_;
    mutable std::mutex    boundary_cb_mu_;

    void boundaryDispatcherLoop(std::stop_token st);
    bool emitBoundary(ClipBoundaryEvent ev);
};
