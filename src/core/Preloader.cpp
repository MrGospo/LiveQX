#include "core/Preloader.h"
#include "transitions/ITransition.h"
#include "utils/Log.h"

#include <chrono>
#include <unordered_set>

Preloader::Preloader(Timeline& timeline, double preload_sec)
    : timeline_(timeline), preload_sec_(preload_sec) {}

std::vector<int> Preloader::computeWarmSet(int n, int cur_idx, int cap) {
    std::vector<int> out;
    if (n <= 0 || cap <= 0) return out;
    // Fold a stale or negative cursor back into [0, n). C++ % on negative
    // values is implementation-defined for some types — handle explicitly.
    if (cur_idx < 0) {
        cur_idx = ((cur_idx % n) + n) % n;
    } else if (cur_idx >= n) {
        cur_idx %= n;
    }
    const int take = std::min(cap, n);
    out.reserve(static_cast<size_t>(take));
    for (int i = 0; i < take; ++i) {
        out.push_back((cur_idx + i) % n);
    }
    return out;
}

spdlog::logger& Preloader::lg() noexcept {
    return logger_ ? *logger_ : *spdlog::default_logger();
}

Preloader::~Preloader() {
    // Block until background tasks finish — avoids dangerously destroying
    // a clip that is still being prepared/released in another thread.
    if (preload_future_.valid()) preload_future_.wait();
    if (release_future_.valid()) release_future_.wait();
}

void Preloader::tick() {
    auto snap = timeline_.snapshot();
    if (snap->clips.empty()) return;

    const int n = static_cast<int>(snap->clips.size());
    const int cur_idx = timeline_.getActiveIndex();
    if (cur_idx < 0 || cur_idx >= n) return;

    const int next_idx        = (cur_idx + 1) % n;
    auto      cur_clip_sp     = snap->clips[cur_idx];
    auto      next_clip_sp    = snap->clips[next_idx];
    IClip*    cur_clip        = cur_clip_sp.get();
    IClip*    next_clip       = next_clip_sp.get();
    const double remaining    = timeline_.getRemainingTime();
    const auto   state        = timeline_.peek();

    // fix30: warm-set is the policy contract for which clips must remain
    // prepared. Computed once per tick, used by section 3 (prepare cold
    // members) and section 4 (refuse to release members; release non-members
    // that are still prepared). For n ≤ kWarmCap the set covers the entire
    // playlist — short-playlist release-on-wrap pingpong (the live-test
    // fix30 trigger) is structurally impossible.
    const auto warm_set = computeWarmSet(n, cur_idx, kWarmCap);
    std::unordered_set<IClip*> warm_ptrs;
    warm_ptrs.reserve(warm_set.size());
    for (int idx : warm_set) {
        if (idx >= 0 && idx < n) warm_ptrs.insert(snap->clips[idx].get());
    }

    // Active source was deleted from the share. Timeline's buildState swaps
    // clipB to fallback during the transition window for a smooth crossfade.
    // On the natural slot wrap, drop the entry — sections 1/2/4 below still
    // handle completion of any in-flight prepare/release futures, section 3
    // is gated off so no new reset is started.
    const bool active_pending =
        static_cast<size_t>(cur_idx) < snap->pending_remove.size()
        && snap->pending_remove[cur_idx];
    if (active_pending && cur_clip) {
        // Stop any self-loop state — fallback is taking over on transition.
        cur_clip->setHeadPlayback(false);
        cur_clip->setFreezeTail(false);
        pending_head_loop_ = false;
        pending_wrap_snap_ = false;
        if (remaining > prev_remaining_sec_ + 1.0) {
            // Slot wrapped — clip finished its final cycle including the
            // fallback crossfade. Drop it; cursor self-heals on next
            // advance(). last_clip_ stays as cur_clip_sp so section 4 will
            // schedule the async release once active changes.
            timeline_.reapPendingActive();
            prev_remaining_sec_ = 0.0;
        } else {
            prev_remaining_sec_ = remaining;
        }
    }

    // Outgoing-tail length of cur_clip's slot — duration of the transition
    // window. Zero for HardCut. Drives single-clip self-loop reset timing.
    double tail_len = 0.0;
    if (static_cast<size_t>(cur_idx) < snap->transitions.size()) {
        const auto& tr = snap->transitions[cur_idx];
        if (tr.type != TransitionType::HardCut
                && tr.mode != TransitionMode::HardCut)
            tail_len = std::max(0.0, tr.duration_sec);
    }

    auto now_ns = [] {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    };

    // ── 1. Non-blocking check: preload finished ───────────────────────────────
    if (preload_future_.valid()
            && preload_future_.wait_for(std::chrono::seconds(0))
               == std::future_status::ready) {
        const int64_t elapsed_ms =
            preload_started_ns_ > 0 ? (now_ns() - preload_started_ns_) / 1'000'000 : -1;
        try {
            preload_future_.get();
            lg().info("Preloader: clip {} prepare/reset finished in {}ms",
                      preloading_log_idx_, elapsed_ms);
        } catch (const std::exception& e) {
            lg().error("Preloader: prepare failed for clip {}: {}", preloading_log_idx_, e.what());
        }
        preloading_clip_.reset();
        preload_started_ns_ = 0;

        // Single-clip wrap: clip is now back at t=0. Snap the cursor's
        // intra-slot position to 0 so it stays aligned with the clip's
        // internal playback. Otherwise slot_pos and clip-time drift apart
        // by the lead time, causing EOF mid-slot in subsequent cycles.
        if (pending_wrap_snap_) {
            timeline_.snapCursorToActiveStart();
            pending_wrap_snap_ = false;
        }
    }

    // ── 2. Non-blocking check: release finished ───────────────────────────────
    if (release_future_.valid()
            && release_future_.wait_for(std::chrono::seconds(0))
               == std::future_status::ready) {
        const int64_t elapsed_ms =
            release_started_ns_ > 0 ? (now_ns() - release_started_ns_) / 1'000'000 : -1;
        try {
            release_future_.get();
            lg().info("Preloader: clip {} release finished in {}ms",
                      releasing_log_idx_, elapsed_ms);
        } catch (const std::exception& e) {
            lg().error("Preloader: release failed: {}", e.what());
        }
        release_started_ns_ = 0;
        releasing_clip_.reset();
    }

    // ── 3. Start preload if approaching end of current clip ───────────────────
    // Detect playlist wrap: remaining jumped upward by a non-trivial amount.
    // Clears the prepared_for_clip_ sentinel so a single-clip loop can
    // re-issue reset() each cycle. Threshold (preload_sec_) is conservative —
    // normal monotonic ticks decrease remaining by ~frame_dt each call.
    // While a self-loop reset is in flight, gate head playback to the actual
    // transition window — clipB role is only queried when in_transition is
    // true, but clipA falls back to getFrame() outside it, so flipping head
    // playback early would substitute a t=0 frame for the natural last
    // frames of content. Idempotent: setHeadPlayback only resets cursors on
    // rising edge.
    if (pending_head_loop_ && cur_clip)
        cur_clip->setHeadPlayback(state.in_transition);

    const bool wrap_detected =
        prepared_for_clip_.get() == cur_clip
        && remaining > prev_remaining_sec_ + preload_sec_;
    if (wrap_detected) {
        prepared_for_clip_.reset();
        // Single-clip transition loop: clipB consumed head-buffer frames
        // during the transition; the live decoder was reset() in parallel
        // and is now at t=0, queue-filled and back-pressured. Cursor just
        // wrapped to slot_pos=0 — both line up cleanly. Disable head
        // playback so getFrame() resumes from the live queue, and unfreeze
        // the tail so subsequent frames refresh it for the next cycle.
        if (pending_head_loop_) {
            if (cur_clip) {
                cur_clip->setHeadPlayback(false);
                cur_clip->setFreezeTail(false);
            }
            pending_head_loop_ = false;
        }
    }
    prev_remaining_sec_ = remaining;

    // Single-clip case: next_clip == cur_clip, reset() rewinds the same
    // instance that's currently feeding frames to the renderer. To minimize
    // the visual jump, fire reset as close to natural EOF as possible. The
    // ~80ms async reset is masked by RenderLoop's last-good-frame fallback.
    const bool   single_clip          = (n == 1);
    // Single-clip with a real transition: fire reset right when the
    // transition window opens. clipB serves from the pre-decoded head
    // buffer during the ~80ms reset latency; by the time the buffer
    // approaches its end, the live decoder is already producing fresh
    // frames at t=0 for the next cycle.
    const bool   self_transition_loop = single_clip && tail_len > 0.0;
    const double effective_preload_sec =
        self_transition_loop ? tail_len
                             : (single_clip ? 0.5 : preload_sec_);

    const bool preload_due  = remaining < effective_preload_sec;
    const bool not_loading  = next_clip != preloading_clip_.get();
    const bool future_free  = !preload_future_.valid()
                              || preload_future_.wait_for(std::chrono::seconds(0))
                                 == std::future_status::ready;

    // Sentinel: only fire once per active-clip rotation. cur_clip pointer
    // changes only when playback crosses a slot boundary, so this guarantees
    // at most one prepare/reset per next_clip per visit to the current clip.
    // For a single-clip playlist next_clip == cur_clip, so the wrap branch
    // below issues reset() once per loop and the sentinel suppresses repeats.
    const bool already_prepared_here = (prepared_for_clip_.get() == cur_clip);

    // Active is marked for removal AND is the only candidate slot —
    // doomed clip, fallback owns the crossfade, no preload makes sense.
    // Multi-clip pending (n≥2) keeps the gate open: next_clip is a real
    // forward slot that must remain prepared so cursor self-heal lands
    // on a ready clip after reapPendingActive() drops the pending entry.
    const bool single_clip_pending = (next_clip == cur_clip) && active_pending;

    if (preload_due && not_loading && future_free
            && !already_prepared_here && next_clip && !single_clip_pending) {
        if (!next_clip->isPrepared()) {
            preload_future_ = std::async(std::launch::async,
                                         [next_clip] { next_clip->prepare(); });
            preloading_clip_     = next_clip_sp;
            preloading_log_idx_  = next_idx;
            preload_started_ns_  = now_ns();
            prepared_for_clip_   = cur_clip_sp;
            lg().info("Preloader: started prepare for clip {} ({:.1f}s remaining)",
                      next_idx, remaining);
        } else if (next_idx <= cur_idx) {
            // Playlist wrap: clip already played in a prior cycle and is
            // still prepared. Rewind it back to t=0 before reuse.
            // Single-clip self-transition: clipA renders the frozen tail and
            // clipB pulls from the pre-decoded head buffer. We still reset
            // the live decoder in parallel so it's primed at t=0 by the time
            // the transition window closes — no cursor adjustment needed.
            // Tail freeze starts now (lead time = reset latency); head
            // playback toggles on/off below as state.in_transition changes,
            // so the user sees full content_len of normal frames first.
            if (self_transition_loop && next_clip == cur_clip) {
                cur_clip->setFreezeTail(true);
            }
            preload_future_ = std::async(std::launch::async,
                                         [next_clip] { next_clip->reset(); });
            preloading_clip_     = next_clip_sp;
            preloading_log_idx_  = next_idx;
            preload_started_ns_  = now_ns();
            prepared_for_clip_   = cur_clip_sp;
            if (self_transition_loop) {
                // Disable head playback + unfreeze on the natural slot wrap
                // detected via remaining-jump in section above. Live decoder
                // ends up at t=0 right when slot_pos wraps to 0 — aligned.
                pending_head_loop_ = true;
            } else {
                // HardCut single-clip: snap cursor to 0 once reset completes
                // (see section 1) — there is no transition window to consume
                // decoder time, so clip's internal t=0 aligns with slot_pos=0.
                pending_wrap_snap_ = single_clip;
            }
            lg().info("Preloader: started reset for clip {} (wrap, {:.1f}s remaining)",
                      next_idx, remaining);
        }
    }

    // Warm-set maintenance: scan the policy-defined warm set and fire
    // prepare for any cold member that is not currently in flight. This
    // is the timing-independent half of the warm-set contract — it runs
    // every tick, not gated on remaining time, so a member that became
    // unprepared (release race, freshly-added clip, post-reap fold-in)
    // is brought back to ready well before its slot becomes active.
    // Skips cur_clip (rendering ⇒ prepared) and the doomed single-clip
    // pending case. One in-flight future at a time — next cold member
    // gets prepared on the following tick once section 1 finalizes.
    if (!single_clip_pending) {
        const bool future_idle = !preload_future_.valid()
                                 || preload_future_.wait_for(std::chrono::seconds(0))
                                    == std::future_status::ready;
        if (future_idle) {
            for (int idx : warm_set) {
                if (idx == cur_idx) continue;
                auto sp = snap->clips[idx];
                IClip* c = sp.get();
                if (!c || c->isPrepared()) continue;
                if (c == preloading_clip_.get()) continue;
                preload_future_     = std::async(std::launch::async,
                                                  [c] { c->prepare(); });
                preloading_clip_    = sp;
                preloading_log_idx_ = idx;
                preload_started_ns_ = now_ns();
                lg().info("Preloader: warm-set prepare for clip {} ({:.1f}s remaining)",
                          idx, remaining);
                break;
            }
        }
    }

    // ── 4. Release policy ────────────────────────────────────────────────────
    // Two halves:
    //   (a) On slot-wrap, release the previous active iff it has fallen out
    //       of the warm-set. Pointer comparison — immune to index shifts
    //       from appendClip/reapRemovable mid-flight.
    //   (b) Cold-set sweep: any prepared clip whose pointer is not in the
    //       warm-set is a release candidate. One per tick (staggered) so
    //       a cap=2 playlist of length n sheds n−2 prepared clips over
    //       n−2 ticks rather than spawning an async storm.
    // For n ≤ kWarmCap warm_ptrs covers the entire playlist, so this section
    // is a no-op — exactly the desired short-playlist behaviour.
    auto release_idle = [this] {
        return !release_future_.valid()
               || release_future_.wait_for(std::chrono::seconds(0))
                  == std::future_status::ready;
    };

    if (cur_clip != last_clip_.get() && !state.in_transition) {
        IClip* old_clip = last_clip_.get();
        if (old_clip && old_clip->isPrepared()
                && warm_ptrs.find(old_clip) == warm_ptrs.end()
                && release_idle()) {
            int old_idx = -1;
            for (size_t i = 0; i < snap->clips.size(); ++i) {
                if (snap->clips[i].get() == old_clip) {
                    old_idx = static_cast<int>(i);
                    break;
                }
            }
            releasing_clip_     = last_clip_;  // keep alive across async release
            release_future_     = std::async(std::launch::async,
                                              [old_clip] { old_clip->release(); });
            release_started_ns_ = now_ns();
            releasing_log_idx_  = old_idx;
            lg().info("Preloader: started release for clip {} (slot-wrap, out of warm-set)",
                      old_idx);
        }
        last_clip_ = cur_clip_sp;
    }

    // (b) Staggered cold sweep — one cold-but-prepared release per tick.
    if (release_idle()) {
        for (size_t i = 0; i < snap->clips.size(); ++i) {
            IClip* c = snap->clips[i].get();
            if (!c) continue;
            if (warm_ptrs.find(c) != warm_ptrs.end()) continue;
            if (!c->isPrepared()) continue;
            if (c == preloading_clip_.get()) continue;  // prepare in flight
            if (c == releasing_clip_.get()) continue;   // already releasing
            releasing_clip_     = snap->clips[i];
            release_future_     = std::async(std::launch::async,
                                              [c] { c->release(); });
            release_started_ns_ = now_ns();
            releasing_log_idx_  = static_cast<int>(i);
            lg().info("Preloader: cold-sweep release for clip {} (out of warm-set)",
                      i);
            break;
        }
    }
}
