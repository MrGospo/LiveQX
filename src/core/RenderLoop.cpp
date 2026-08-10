#include "core/RenderLoop.h"
#include "audio/AudioMixer.h"
#include "encoding/Encoder.h"
#include "utils/CpuAffinity.h"
#include "utils/Log.h"
#include "utils/RateLimitedLog.h"
#include <chrono>
#include <fstream>
#include <thread>

using namespace std::chrono;

RenderLoop::RenderLoop(int fps, Timeline& timeline,
                       ICompositor& compositor, Encoder& encoder,
                       std::shared_ptr<ChannelMetrics> metrics)
    : fps_(fps), timeline_(timeline),
      compositor_(compositor), encoder_(encoder),
      metrics_(metrics ? std::move(metrics) : std::make_shared<ChannelMetrics>()) {}

RenderLoop::~RenderLoop() { stop(); }

spdlog::logger& RenderLoop::lg() noexcept {
    return logger_ ? *logger_ : *spdlog::default_logger();
}

void RenderLoop::setLogger(std::shared_ptr<spdlog::logger> lg) {
    logger_ = std::move(lg);
    if (preloader_) preloader_->setLogger(logger_);
}

void RenderLoop::start() {
    if (running_.exchange(true)) return; // already running
    boundary_running_.store(true, std::memory_order_release);
    boundary_dispatcher_ = std::jthread(
        [this](std::stop_token st) { boundaryDispatcherLoop(st); });
    thread_ = std::jthread([this](std::stop_token st) { run(st); });
}

void RenderLoop::stop() {
    running_.store(false);
    thread_.request_stop();
    if (thread_.joinable()) thread_.join();
    // Render-thread is done — drain the dispatcher so any boundary events
    // emitted in the final ticks reach the subscriber before we return.
    boundary_running_.store(false, std::memory_order_release);
    boundary_q_.notify_all();
    boundary_dispatcher_.request_stop();
    if (boundary_dispatcher_.joinable()) boundary_dispatcher_.join();
    if (pcm_dump_.is_open()) pcm_dump_.close();
}

void RenderLoop::onClipBoundary(BoundaryCb cb) {
    std::lock_guard<std::mutex> lk(boundary_cb_mu_);
    boundary_cb_ = std::move(cb);
}

void RenderLoop::setFrameTap(FrameTap tap) {
    std::lock_guard<std::mutex> lk(frame_tap_mu_);
    frame_tap_ = std::move(tap);
}

void RenderLoop::emitBoundary(ClipBoundaryEvent ev) {
    if (!boundary_q_.push(std::move(ev))) {
        boundary_drops_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    boundary_q_.notify();
}

void RenderLoop::boundaryDispatcherLoop(std::stop_token st) {
    Log::setThreadName("ch" + channel_id_ + "-boundary");
    using namespace std::chrono;
    while (!st.stop_requested() &&
           (boundary_running_.load(std::memory_order_acquire) ||
            !boundary_q_.empty())) {
        auto opt = boundary_q_.pop_wait(milliseconds(100));
        if (!opt) continue;
        BoundaryCb cb_copy;
        {
            std::lock_guard<std::mutex> lk(boundary_cb_mu_);
            cb_copy = boundary_cb_;
        }
        if (cb_copy) {
            try { cb_copy(*opt); }
            catch (const std::exception& e) {
                lg().warn("RenderLoop: boundary subscriber threw: {}",
                          e.what());
            }
        }
    }
}

void RenderLoop::enablePreloader(double preload_sec) {
    preloader_.emplace(timeline_, preload_sec);
    if (logger_) preloader_->setLogger(logger_);
}

void RenderLoop::setChannelId(std::string id) {
    channel_id_ = std::move(id);
}

ChannelMetricsSnapshot RenderLoop::getMetrics() const {
    return metrics_->snapshot();
}

void RenderLoop::enablePcmDump(const std::string& path) {
    pcm_dump_path_ = path;
}

void RenderLoop::run(std::stop_token st) {
    numa::bindCurrentThreadToNode(numa_node_);
    Log::setThreadName("ch" + channel_id_ + "-render");
    if (!pcm_dump_path_.empty()) {
        pcm_dump_.open(pcm_dump_path_, std::ios::binary | std::ios::trunc);
        if (pcm_dump_.is_open())
            lg().info("RenderLoop: PCM dump → {}", pcm_dump_path_);
        else
            lg().warn("RenderLoop: failed to open PCM dump file '{}'", pcm_dump_path_);
    }

    AudioMixer mixer;
    const auto frame_dur       = microseconds(1'000'000 / fps_);
    double     elapsed         = 0.0;
    const int  samples_per_frame = 48000 / fps_;

    // 1s rolling-counter ticker (single-threaded driver from this loop)
    auto     last_tick_tp    = steady_clock::now();

    // Profiling: encoder.pushFrame() latency (resets each 60s window)
    int64_t  push_max_us     = 0;
    int64_t  push_sum_us     = 0;
    uint64_t push_count      = 0;

    // 60s metrics tracking (local to this run, resets on restart)
    auto     last_report_tp   = steady_clock::now();
    uint64_t last_rendered    = 0;  // updated every 1s tick (for actual_fps)
    uint64_t last_rendered_60s = 0; // updated every 60s log
    uint64_t last_dropped    = 0;
    uint64_t last_underruns  = 0;
    uint64_t last_fallbacks  = 0;
    uint64_t last_glitches   = 0;
    uint64_t last_decerrs    = 0;

    // Boundary tracking — what was active last tick, when did it start.
    int            boundary_prev_idx        = -1;
    std::string    boundary_prev_path;
    std::string    boundary_prev_type;
    int64_t        boundary_prev_started_ns = 0;
    TransitionType boundary_pending_type    = TransitionType::HardCut;

    while (!st.stop_requested() && running_.load(std::memory_order_relaxed)) {
        auto t0 = steady_clock::now();
        // Heartbeat: written before any work — Watchdog reads (now - last_tick_ns).
        metrics_->last_tick_ns.store(
            duration_cast<nanoseconds>(t0.time_since_epoch()).count(),
            std::memory_order_relaxed);

        profiler_.enterStage(liveqx::profiler::Stage::Total);

        if (preloader_) preloader_->tick();
        auto state = timeline_.peek();
        const int idx_now = timeline_.getActiveIndex();
        active_idx_.store(idx_now, std::memory_order_relaxed);

        // fix16: surface the in-transition flag for /api/metrics readers.
        // Single-writer (this thread); MetricsCollector reads relaxed.
        metrics_->transition_active.store(state.in_transition,
                                          std::memory_order_relaxed);

        // Capture the transition currently in progress so the *outgoing*
        // clip's boundary event reports how it was crossfaded into the next.
        if (state.in_transition) boundary_pending_type = state.transition_type;

        if (idx_now != boundary_prev_idx) {
            const int64_t now_wall_ns = duration_cast<nanoseconds>(
                system_clock::now().time_since_epoch()).count();
            // Always consume — even when boundary_prev_idx is -1 (first clip
            // becomes active) so a stray pre-play skip/reap signal cannot
            // leak into the next real boundary.
            const auto bstatus = timeline_.consumeBoundaryStatus();
            if (boundary_prev_idx >= 0) {
                // fix16: count *real* boundaries only — first-clip start
                // (boundary_prev_idx == -1) is not a transition.
                metrics_->clip_changes.fetch_add(1, std::memory_order_relaxed);
                ClipBoundaryEvent ev;
                ev.prev_idx       = boundary_prev_idx;
                ev.prev_path      = boundary_prev_path;
                ev.prev_type      = boundary_prev_type;
                ev.started_at_ns  = boundary_prev_started_ns;
                ev.ended_at_ns    = now_wall_ns;
                ev.played_sec     =
                    (now_wall_ns - boundary_prev_started_ns) / 1e9;
                ev.transition     = boundary_pending_type;
                ev.status         = Timeline::boundaryStatusName(bstatus);
                emitBoundary(std::move(ev));
            }
            // Refresh tracking for the new active clip.
            boundary_prev_idx        = idx_now;
            boundary_prev_started_ns = now_wall_ns;
            boundary_pending_type    = TransitionType::HardCut;
            boundary_prev_path.clear();
            boundary_prev_type.clear();
            if (idx_now >= 0) {
                if (auto snap = timeline_.snapshot()) {
                    if (idx_now < static_cast<int>(snap->clips.size())) {
                        if (idx_now < static_cast<int>(snap->cache_paths.size()))
                            boundary_prev_path = snap->cache_paths[idx_now];
                        if (snap->clips[idx_now])
                            boundary_prev_type =
                                snap->clips[idx_now]->clipType();
                    }
                }
            }
        }

        // Broadcast-model dispatch: clipA_in_tail / clipB_in_tail flags from
        // TimelineState pick between content (advances) and tail (frozen).
        // HardCut    ⇒ never in transition window.
        // FreezeFade ⇒ both clipA_in_tail and clipB_in_tail = true.
        // LiveMix    ⇒ clipA_in_tail = true, clipB_in_tail = false.
        profiler_.enterStage(liveqx::profiler::Stage::Decode);
        Frame      frameA = state.clipA_in_tail
                              ? safeGetTailFrame(state.clipA)
                              : safeGetFrame   (state.clipA);
        AudioFrame audioA = state.clipA_in_tail
                              ? safeGetTailAudio(state.clipA, samples_per_frame)
                              : safeGetAudio   (state.clipA, samples_per_frame);

        Frame      video_out;
        AudioFrame audio_out;
        Frame      frameB;
        AudioFrame audioB;
        const bool have_b = state.in_transition && state.clipB;

        if (have_b) {
            frameB = state.clipB_in_tail
                                  ? safeGetTailFrame(state.clipB)
                                  : safeGetFrame   (state.clipB);
            audioB = state.clipB_in_tail
                                  ? safeGetTailAudio(state.clipB, samples_per_frame)
                                  : safeGetAudio   (state.clipB, samples_per_frame);
        }
        profiler_.leaveStage(liveqx::profiler::Stage::Decode);

        profiler_.enterStage(liveqx::profiler::Stage::Compose);
        if (have_b) {
            video_out = compositor_.composite(
                frameA, frameB, state.transition_type, state.transition_progress,
                state.transition_easing);
            audio_out = mixer.crossfade(audioA, audioB, state.transition_progress);
        } else {
            video_out = frameA;
            audio_out = audioA;
        }
        profiler_.leaveStage(liveqx::profiler::Stage::Compose);

        if (pcm_dump_.is_open() && audio_out.valid && !audio_out.samples.empty())
            pcm_dump_.write(reinterpret_cast<const char*>(audio_out.samples.data()),
                            static_cast<std::streamsize>(
                                audio_out.samples.size() * sizeof(float)));

        // fix34 D2.8 — preview tap. Snapshot the std::function under the
        // mutex (cheap), drop the lock, then invoke. Tap is allowed to be
        // expensive (preview encoder runs synchronously) but must not throw.
        FrameTap tap_now;
        {
            std::lock_guard<std::mutex> lk(frame_tap_mu_);
            tap_now = frame_tap_;
        }
        if (tap_now && video_out.valid()) {
            try { tap_now(video_out); }
            catch (const std::exception& e) {
                lg().warn("RenderLoop[{}]: frame tap threw: {}",
                          channel_id_, e.what());
            }
        }

        profiler_.enterStage(liveqx::profiler::Stage::Encode);
        const auto t_push = steady_clock::now();
        encoder_.pushFrame(video_out, audio_out);
        const int64_t push_us =
            duration_cast<microseconds>(steady_clock::now() - t_push).count();
        profiler_.leaveStage(liveqx::profiler::Stage::Encode);
        push_max_us  = std::max(push_max_us, push_us);
        push_sum_us += push_us;
        ++push_count;

        metrics_->frames_rendered.fetch_add(1, std::memory_order_relaxed);
        metrics_->rolling_rendered.add();

        total_audio_samples_ += samples_per_frame;
        const double prev_elapsed = elapsed;
        elapsed = static_cast<double>(total_audio_samples_) / 48000.0;
        // Advance the cursor by audio-clock dt — pointer-anchored, immune to
        // playlist mutations (appendClip/reapRemovable). Replaces the old
        // fmod(elapsed, total) scheduling that jumped on mutations.
        timeline_.advance(elapsed - prev_elapsed);

        // fix13 c9: drive every clip's state machine once per tick. Most
        // clips noop; LiveClip uses this to walk its Idle→WarmingUp→Live
        // pipeline against the wall clock without owning a clock of its
        // own. Snapshot is the same one peek() saw — pointer-stable for
        // the duration of the tick.
        if (auto sn = timeline_.snapshot()) {
            const std::uint64_t wall_now_ns = duration_cast<nanoseconds>(
                system_clock::now().time_since_epoch()).count();
            for (const auto& cp : sn->clips) {
                if (cp) cp->onTick(wall_now_ns);
            }
        }

        profiler_.leaveStage(liveqx::profiler::Stage::Total);

        auto work_dur = steady_clock::now() - t0;

        // CAS-update rolling peak frame time
        const int64_t work_us = duration_cast<microseconds>(work_dur).count();
        {
            int64_t cur = metrics_->frame_time_max_us.load(std::memory_order_relaxed);
            while (work_us > cur &&
                   !metrics_->frame_time_max_us.compare_exchange_weak(
                       cur, work_us, std::memory_order_relaxed)) {}
        }

        // 1s rolling-window tick (drives RollingCounter buckets + live FPS)
        const auto now_tp = steady_clock::now();
        if (duration_cast<milliseconds>(now_tp - last_tick_tp).count() >= 1000) {
            metrics_->tickRolling();
            // Update actual_fps every second so short-duration stress tests
            // (< 60s) always get a non-zero reading for every channel.
            const uint64_t rendered_now = metrics_->frames_rendered.load(std::memory_order_relaxed);
            const double   tick_elapsed =
                duration_cast<microseconds>(now_tp - last_tick_tp).count() / 1e6;
            if (tick_elapsed > 0.0)
                metrics_->actual_fps.store(
                    static_cast<double>(rendered_now - last_rendered) / tick_elapsed,
                    std::memory_order_relaxed);
            last_rendered = rendered_now;
            last_tick_tp  = now_tp;
        }

        // 60s metrics log
        if (duration_cast<seconds>(now_tp - last_report_tp).count() >= 60) {
            const uint64_t rendered  = metrics_->frames_rendered.load(std::memory_order_relaxed);
            const uint64_t dropped   = metrics_->frames_dropped.load(std::memory_order_relaxed);
            const uint64_t underruns = metrics_->audio_underruns.load(std::memory_order_relaxed);
            const uint64_t fallbacks = metrics_->loop_fallback_count.load(std::memory_order_relaxed);
            const uint64_t glitches  = metrics_->audio_loop_glitches.load(std::memory_order_relaxed);
            const uint64_t decerrs   = metrics_->decode_errors.load(std::memory_order_relaxed);
            const double   elapsed_s =
                duration_cast<microseconds>(now_tp - last_report_tp).count() / 1e6;

            const double fps = static_cast<double>(rendered - last_rendered_60s) / elapsed_s;
            metrics_->actual_fps.store(fps, std::memory_order_relaxed);

            const int64_t push_avg_us = push_count > 0
                ? static_cast<int64_t>(push_sum_us / push_count) : 0;

            lg().info("fps={:.1f} drops={} underruns={} loop_fallback={} "
                      "audio_glitch={} decode_err={} max_frame={}us "
                      "push_max={}us push_avg={}us",
                fps,
                dropped   - last_dropped,
                underruns - last_underruns,
                fallbacks - last_fallbacks,
                glitches  - last_glitches,
                decerrs   - last_decerrs,
                metrics_->frame_time_max_us.exchange(0, std::memory_order_relaxed),
                push_max_us, push_avg_us);

            push_max_us = 0;
            push_sum_us = 0;
            push_count  = 0;

            last_rendered_60s = rendered;
            last_dropped      = dropped;
            last_underruns = underruns;
            last_fallbacks = fallbacks;
            last_glitches  = glitches;
            last_decerrs   = decerrs;
            last_report_tp = now_tp;
        }

        if (work_dur < frame_dur)
            std::this_thread::sleep_for(frame_dur - work_dur);
        else
            LOG_WARN_RL_LG(lg(), 2, "RenderLoop: frame overrun by {}us",
                duration_cast<microseconds>(work_dur - frame_dur).count());
    }
}

Frame RenderLoop::safeGetFrame(IClip* clip) {
    if (!clip) return last_good_frame_;
    auto f = clip->getFrame();
    if (!f.valid()) {
        LOG_WARN_RL_LG(lg(), 5, "RenderLoop: getFrame() returned invalid frame, using last good");
        metrics_->frames_dropped.fetch_add(1, std::memory_order_relaxed);
        metrics_->rolling_drops.add();
        return last_good_frame_;
    }
    last_good_frame_ = f;
    return f;
}

AudioFrame RenderLoop::safeGetAudio(IClip* clip, int target_samples) {
    if (!clip) {
        AudioFrame silence;
        silence.sample_rate = 48000;
        silence.channels    = 2;
        silence.num_samples = target_samples;
        silence.samples.assign(static_cast<size_t>(target_samples) * 2, 0.0f);
        silence.valid = true;
        return silence;
    }

    auto result = clip->getAudio(target_samples);
    if (!result.valid) {
        metrics_->audio_underruns.fetch_add(1, std::memory_order_relaxed);
        metrics_->rolling_underruns.add();
        return last_good_audio_;
    }

    if (result.num_samples < target_samples) {
        LOG_DEBUG_RL_LG(lg(), 5, "RenderLoop: audio short — got {}/{} samples, padding with silence",
                        result.num_samples, target_samples);
        metrics_->audio_underruns.fetch_add(1, std::memory_order_relaxed);
        metrics_->rolling_underruns.add();
        const size_t needed = static_cast<size_t>(target_samples) * 2;
        result.samples.resize(needed, 0.0f);
        result.num_samples = target_samples;
    }

    last_good_audio_ = result;
    return result;
}

// ─── Tail-mode dispatch (broadcast transition window) ───────────────────────
// During a transition, the outgoing clip is rendered via getTailFrame /
// getTailAudio — these MUST NOT advance the clip's playback position.
// Fallback to last_good_* keeps the pipeline alive if the clip cannot
// produce a tail snapshot (e.g. before its first decoded frame).

Frame RenderLoop::safeGetTailFrame(IClip* clip) {
    if (!clip) return last_good_frame_;
    auto f = clip->getTailFrame();
    if (!f.valid()) return last_good_frame_;
    return f;
}

AudioFrame RenderLoop::safeGetTailAudio(IClip* clip, int target_samples) {
    if (!clip) {
        AudioFrame silence;
        silence.sample_rate = 48000;
        silence.channels    = 2;
        silence.num_samples = target_samples;
        silence.samples.assign(static_cast<size_t>(target_samples) * 2, 0.0f);
        silence.valid = true;
        return silence;
    }
    auto result = clip->getTailAudio(target_samples);
    if (!result.valid || result.num_samples < target_samples) {
        AudioFrame silence;
        silence.sample_rate = 48000;
        silence.channels    = 2;
        silence.num_samples = target_samples;
        silence.samples.assign(static_cast<size_t>(target_samples) * 2, 0.0f);
        silence.valid = true;
        return silence;
    }
    return result;
}
