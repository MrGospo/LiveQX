#include "core/Timeline.h"
#include "utils/Log.h"
#include "utils/RateLimitedLog.h"
#include <chrono>
#include <cmath>
#include <utility>

namespace {

inline TransitionConfig transitionFor(
    const std::vector<TransitionConfig>& transitions, size_t i) {
    static const TransitionConfig kHardCut{
        TransitionType::HardCut, TransitionMode::HardCut, 0.0
    };
    if (i >= transitions.size()) return kHardCut;
    TransitionConfig tr = transitions[i];
    if (tr.type == TransitionType::HardCut || tr.duration_sec <= 0.0) {
        tr.mode         = TransitionMode::HardCut;
        tr.duration_sec = 0.0;
    }
    return tr;
}

inline double outgoingTailLen(const TransitionConfig& tr) {
    return (tr.mode == TransitionMode::HardCut) ? 0.0 : tr.duration_sec;
}

inline double prevConsumed(const TransitionConfig& prev_tr) {
    return (prev_tr.mode == TransitionMode::LiveMix) ? prev_tr.duration_sec : 0.0;
}

inline double slotLength(const IClip& clip,
                         const TransitionConfig& prev_tr,
                         const TransitionConfig& own_tr) {
    const double content = clip.getDuration() - prevConsumed(prev_tr);
    return std::max(0.0, content) + outgoingTailLen(own_tr);
}

inline TransitionConfig prevTransition(
    const std::vector<TransitionConfig>& transitions, size_t i, size_t n) {
    if (n == 0) {
        return TransitionConfig{
            TransitionType::HardCut, TransitionMode::HardCut, 0.0};
    }
    return transitionFor(transitions, (i + n - 1) % n);
}

double timelineTotal(const PlaylistSnapshot& snap) {
    const size_t n = snap.clips.size();
    double total = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const auto own  = transitionFor(snap.transitions, i);
        const auto prev = prevTransition(snap.transitions, i, n);
        total += slotLength(*snap.clips[i], prev, own);
    }
    return total;
}

size_t findSlotIndex(const PlaylistSnapshot& snap,
                     double t,
                     double* slot_start_out = nullptr,
                     double* slot_len_out   = nullptr) {
    const size_t n = snap.clips.size();
    double run = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const auto own  = transitionFor(snap.transitions, i);
        const auto prev = prevTransition(snap.transitions, i, n);
        const double slen = slotLength(*snap.clips[i], prev, own);
        if (t < run + slen || i == n - 1) {
            if (slot_start_out) *slot_start_out = run;
            if (slot_len_out)   *slot_len_out   = slen;
            return i;
        }
        run += slen;
    }
    return n - 1;
}

// Build TimelineState for snapshot at slot `idx` with intra-slot position
// `pos_in_slot`. Shared by both cursor (peek) and legacy fmod-based getState.
// `fallback` (optional) is used as the transition target when the active
// slot is pending_remove — gives a smooth crossfade to fallback instead of
// a hard cut when the source file is deleted from the share.
TimelineState buildState(const PlaylistSnapshot& snap,
                         size_t idx, double pos_in_slot,
                         IClip* fallback = nullptr) {
    const size_t n           = snap.clips.size();
    const auto   own_tr      = transitionFor(snap.transitions, idx);
    const auto   prev_tr     = prevTransition(snap.transitions, idx, n);
    const double clip_dur    = snap.clips[idx]->getDuration();
    const double content_len = std::max(0.0, clip_dur - prevConsumed(prev_tr));
    const double tail_len    = outgoingTailLen(own_tr);

    TimelineState state;
    state.clipA = snap.clips[idx].get();

    // fix30: defensive substitution. The Preloader warm-set policy keeps
    // clipA prepared at every tick, but a freshly-added clip or post-reap
    // fold-in can leave a one-frame gap before its async prepare resolves.
    // Substituting fallback (if configured) avoids handing an invalid frame
    // to the encoder. Rate-limited to surface incidents without flooding.
    if (state.clipA && !state.clipA->isPrepared() && fallback) {
        LOG_WARN_RL(2, "Timeline: clipA idx={} not prepared, substituting fallback", idx);
        state.clipA = fallback;
    }

    if (tail_len > 0.0 && pos_in_slot >= content_len) {
        const bool active_pending =
            idx < snap.pending_remove.size() && snap.pending_remove[idx];

        // Default — следующий слот в плейлисте.
        const size_t next_idx       = (idx + 1) % n;
        IClip*       next_clip      = snap.clips[next_idx].get();
        const bool   next_pending   =
            next_idx < snap.pending_remove.size()
            && snap.pending_remove[next_idx];
        const bool   next_is_self   = (next_clip == state.clipA);

        // fix30 follow-up: подставлять fallback **только** когда нет живого
        // следующего слота, в который можно сделать честный crossfade.
        // Случаи: (a) одиночный playlist с pending_remove (next == cur),
        // (b) следующий слот тоже помечен на удаление, (c) следующий клип
        // не подготовлен. Иначе клип, обречённый на удаление, плавно
        // переходит в реальный следующий клип, и после wrap не будет
        // вспышки fallback с последующим хард-катом.
        bool to_fallback = false;
        if (active_pending && fallback) {
            const bool no_live_next =
                next_is_self || next_pending
                || !next_clip || !next_clip->isPrepared();
            if (no_live_next) {
                next_clip   = fallback;
                to_fallback = true;
            }
        }
        const bool self_loop = (next_clip == state.clipA);

        if (!next_clip || (!to_fallback && !next_clip->isPrepared())) {
            LOG_ERROR_RL(2, "Timeline: transition target not ready at idx={}", idx);
        } else {
            state.in_transition       = true;
            state.clipA_in_tail       = true;
            // Fallback clip is a static image — getFrame() returns a single
            // cached frame regardless of the in_tail flag, so either choice
            // works. Use !in_tail (LiveMix style) for self-loop and fallback;
            // FreezeFade only for genuine multi-clip pairs.
            state.clipB_in_tail       = (self_loop || to_fallback)
                                        ? false
                                        : (own_tr.mode == TransitionMode::FreezeFade);
            state.clipB               = next_clip;
            state.transition_type     = own_tr.type;
            state.transition_easing   = own_tr.easing;
            state.transition_progress = static_cast<float>(
                (pos_in_slot - content_len) / tail_len);
        }
    }
    return state;
}

// Locate active_clip in snap by shared_ptr identity. Returns -1 if missing.
int findActiveIdx(const PlaylistSnapshot& snap,
                  const std::shared_ptr<IClip>& active_clip) {
    if (!active_clip) return -1;
    for (size_t i = 0; i < snap.clips.size(); ++i)
        if (snap.clips[i] == active_clip) return static_cast<int>(i);
    return -1;
}

}  // namespace

Timeline::Timeline()
    : current_(std::make_shared<const PlaylistSnapshot>()) {}

Timeline::Timeline(Timeline&& other) noexcept
    : current_(other.current_.load(std::memory_order_acquire)),
      fallback_(std::move(other.fallback_)),
      active_clip_(std::move(other.active_clip_)),
      slot_pos_sec_(other.slot_pos_sec_) {}

void Timeline::setPlaylist(std::vector<std::unique_ptr<IClip>> clips,
                           std::vector<TransitionConfig>        transitions) {
    std::vector<std::string> empty(clips.size());
    setPlaylist(std::move(clips), std::move(transitions), std::move(empty));
}

void Timeline::setPlaylist(std::vector<std::unique_ptr<IClip>> clips,
                           std::vector<TransitionConfig>        transitions,
                           std::vector<std::string>             cache_paths) {
    // unique_ptr overload — no graveyard, default deleter. Test-only path.
    std::vector<std::shared_ptr<IClip>> shared;
    shared.reserve(clips.size());
    for (auto& uptr : clips) shared.emplace_back(std::move(uptr));
    setPlaylist(std::move(shared), std::move(transitions), std::move(cache_paths));
}

void Timeline::setPlaylist(std::vector<std::shared_ptr<IClip>> clips,
                           std::vector<TransitionConfig>        transitions) {
    std::vector<std::string> empty(clips.size());
    setPlaylist(std::move(clips), std::move(transitions), std::move(empty));
}

void Timeline::setPlaylist(std::vector<std::shared_ptr<IClip>> clips,
                           std::vector<TransitionConfig>        transitions,
                           std::vector<std::string>             cache_paths) {
    auto snap = std::make_shared<PlaylistSnapshot>();
    snap->clips          = std::move(clips);
    snap->transitions    = std::move(transitions);
    snap->cache_paths    = std::move(cache_paths);
    snap->pending_remove.assign(snap->clips.size(), false);
    if (snap->cache_paths.size() < snap->clips.size())
        snap->cache_paths.resize(snap->clips.size(), "");

    std::lock_guard<std::mutex> lk(mutator_mu_);
    {
        std::lock_guard<std::mutex> ck(cursor_mu_);
        active_clip_  = snap->clips.empty() ? nullptr : snap->clips.front();
        slot_pos_sec_ = 0.0;
    }
    {
        // setPlaylist replaces every clip identity → the prepare token
        // from the previous playlist no longer points to anything in the
        // new snapshot. Clear it so the next advance() re-fires for the
        // new upcoming clip.
        std::lock_guard<std::mutex> cb(cb_mu_);
        last_prepared_.reset();
    }
    current_.store(std::move(snap), std::memory_order_release);
}

void Timeline::setFallback(std::shared_ptr<IClip> fallback) {
    fallback_ = std::move(fallback);
}

void Timeline::appendClip(std::shared_ptr<IClip> clip,
                          TransitionConfig       transition,
                          std::string            cache_path) {
    std::lock_guard<std::mutex> lk(mutator_mu_);
    auto cur = current_.load(std::memory_order_acquire);
    auto next = std::make_shared<PlaylistSnapshot>(*cur);
    next->clips.emplace_back(std::move(clip));
    next->transitions.emplace_back(transition);
    next->cache_paths.emplace_back(std::move(cache_path));
    next->pending_remove.emplace_back(false);
    // Cursor unchanged: appending to the end never affects the active clip's
    // identity or its intra-slot position.
    current_.store(std::move(next), std::memory_order_release);
}

bool Timeline::markForRemoval(const std::string& cache_path) {
    if (cache_path.empty()) return false;
    std::lock_guard<std::mutex> lk(mutator_mu_);
    auto cur = current_.load(std::memory_order_acquire);
    auto next = std::make_shared<PlaylistSnapshot>(*cur);
    bool found = false;
    for (size_t i = 0; i < next->clips.size(); ++i) {
        if (!next->pending_remove[i] && next->cache_paths[i] == cache_path) {
            next->pending_remove[i] = true;
            found = true;
            // Mark every duplicate slot — otherwise reapRemovable() keeps the
            // playlist non-empty and the deleted file plays forever.
        }
    }
    if (!found) return false;
    current_.store(std::move(next), std::memory_order_release);
    return true;
}

std::vector<std::string> Timeline::reapRemovable(const std::string& active_cache_path) {
    std::vector<std::string> evicted;
    std::lock_guard<std::mutex> lk(mutator_mu_);
    auto cur = current_.load(std::memory_order_acquire);
    auto next = std::make_shared<PlaylistSnapshot>();
    next->clips.reserve(cur->clips.size());
    next->transitions.reserve(cur->transitions.size());
    next->cache_paths.reserve(cur->cache_paths.size());
    next->pending_remove.reserve(cur->pending_remove.size());

    // The active entry is preserved on this path so playback isn't cut
    // mid-stream. The Preloader removes the active pending entry at the
    // natural slot wrap (after the fallback crossfade completes) via
    // reapPendingActive() — those evictions are drained below.
    bool changed = false;
    for (size_t i = 0; i < cur->clips.size(); ++i) {
        const bool pending   = cur->pending_remove[i];
        const bool is_active = !active_cache_path.empty() &&
                                cur->cache_paths[i] == active_cache_path;
        if (pending && !is_active) {
            evicted.emplace_back(cur->cache_paths[i]);
            changed = true;
            continue;
        }
        next->clips.emplace_back(cur->clips[i]);
        next->transitions.emplace_back(cur->transitions[i]);
        next->cache_paths.emplace_back(cur->cache_paths[i]);
        next->pending_remove.emplace_back(pending);
    }
    // Drain auto-reaped paths from prior reapPendingActive() calls.
    for (auto& p : auto_reaped_paths_) evicted.emplace_back(std::move(p));
    auto_reaped_paths_.clear();
    if (changed)
        current_.store(std::move(next), std::memory_order_release);
    // Cursor self-heals on next advance()/peek() if the active clip was
    // reaped: findActiveIdx returns -1 → callers fall back to clips[0].
    return evicted;
}

Timeline::RemoveResult Timeline::removeAt(std::size_t idx) {
    std::shared_ptr<IClip> active_snapshot;
    {
        std::lock_guard<std::mutex> ck(cursor_mu_);
        active_snapshot = active_clip_;
    }

    std::lock_guard<std::mutex> lk(mutator_mu_);
    auto cur = current_.load(std::memory_order_acquire);
    if (idx >= cur->clips.size()) return RemoveResult::NotFound;

    if (active_snapshot && cur->clips[idx] == active_snapshot) {
        // Active — defer. Preloader::tick() will reap on the natural wrap
        // after the fallback/next-clip crossfade completes.
        if (cur->pending_remove[idx]) return RemoveResult::MarkedActive;
        auto next = std::make_shared<PlaylistSnapshot>(*cur);
        next->pending_remove[idx] = true;
        current_.store(std::move(next), std::memory_order_release);
        return RemoveResult::MarkedActive;
    }

    // Non-active — physical removal.
    auto next = std::make_shared<PlaylistSnapshot>();
    next->clips.reserve(cur->clips.size() - 1);
    next->transitions.reserve(cur->transitions.size() - 1);
    next->cache_paths.reserve(cur->cache_paths.size() - 1);
    next->pending_remove.reserve(cur->pending_remove.size() - 1);
    for (std::size_t i = 0; i < cur->clips.size(); ++i) {
        if (i == idx) continue;
        next->clips.emplace_back(cur->clips[i]);
        next->transitions.emplace_back(cur->transitions[i]);
        next->cache_paths.emplace_back(cur->cache_paths[i]);
        next->pending_remove.emplace_back(cur->pending_remove[i]);
    }
    current_.store(std::move(next), std::memory_order_release);
    return RemoveResult::Removed;
}

bool Timeline::reapPendingActive() {
    std::shared_ptr<IClip> active_snapshot;
    {
        std::lock_guard<std::mutex> ck(cursor_mu_);
        active_snapshot = active_clip_;
    }
    if (!active_snapshot) return false;

    std::lock_guard<std::mutex> lk(mutator_mu_);
    auto cur = current_.load(std::memory_order_acquire);
    int idx = -1;
    for (size_t i = 0; i < cur->clips.size(); ++i) {
        if (cur->clips[i] == active_snapshot) { idx = static_cast<int>(i); break; }
    }
    if (idx < 0) return false;
    if (!cur->pending_remove[idx]) return false;

    auto next = std::make_shared<PlaylistSnapshot>();
    next->clips.reserve(cur->clips.size() - 1);
    next->transitions.reserve(cur->transitions.size() - 1);
    next->cache_paths.reserve(cur->cache_paths.size() - 1);
    next->pending_remove.reserve(cur->pending_remove.size() - 1);
    for (size_t i = 0; i < cur->clips.size(); ++i) {
        if (static_cast<int>(i) == idx) continue;
        next->clips.emplace_back(cur->clips[i]);
        next->transitions.emplace_back(cur->transitions[i]);
        next->cache_paths.emplace_back(cur->cache_paths[i]);
        next->pending_remove.emplace_back(cur->pending_remove[i]);
    }
    auto_reaped_paths_.emplace_back(cur->cache_paths[idx]);
    current_.store(std::move(next), std::memory_order_release);
    // The previously-active clip was just removed because its source file
    // was deleted. RenderLoop will see idx change on its next tick — tag the
    // proof-of-play row as "removed".
    pending_boundary_status_.store(BoundaryStatus::Removed,
                                   std::memory_order_release);
    return true;
}

std::shared_ptr<const PlaylistSnapshot> Timeline::snapshot() const noexcept {
    return current_.load(std::memory_order_acquire);
}

// ─── Stateful cursor API ─────────────────────────────────────────────────────

void Timeline::advance(double dt_sec) {
    auto snap = current_.load(std::memory_order_acquire);

    // Track whether we crossed a slot boundary on this tick so we can clear
    // last_prepared_ — the prepare token belongs to the slot we just left.
    bool slot_rolled = false;
    std::shared_ptr<IClip> fire_clip;
    double                 fire_remaining = 0.0;

    {
        std::lock_guard<std::mutex> ck(cursor_mu_);

        if (snap->clips.empty()) {
            active_clip_  = nullptr;
            slot_pos_sec_ = 0.0;
        } else {
            int idx = findActiveIdx(*snap, active_clip_);
            if (idx < 0) {
                // Active clip was reaped or this is the first tick after a
                // fresh setPlaylist with new identities. Heal: jump to playlist
                // head.
                idx           = 0;
                active_clip_  = snap->clips[0];
                slot_pos_sec_ = 0.0;
            }

            const int idx_before = idx;
            slot_pos_sec_ += dt_sec;

            const size_t n = snap->clips.size();
            // Roll across slot boundaries — a single advance() may straddle
            // multiple slots if dt_sec is large or slots are short.
            for (int guard = 0; guard <= static_cast<int>(n); ++guard) {
                const auto own  = transitionFor(snap->transitions, idx);
                const auto prev = prevTransition(snap->transitions, idx, n);
                const double slen = slotLength(*snap->clips[idx], prev, own);
                if (slen <= 0.0 || slot_pos_sec_ < slen) break;
                slot_pos_sec_ -= slen;
                idx           = static_cast<int>((idx + 1) % n);
                active_clip_  = snap->clips[idx];
            }
            slot_rolled = (idx != idx_before);

            // Compute remaining time in current slot to decide whether the
            // upcoming clip is now inside its warm-up window.
            const auto own  = transitionFor(snap->transitions, idx);
            const auto prev = prevTransition(snap->transitions, idx, n);
            const double slen = slotLength(*snap->clips[idx], prev, own);
            const double remaining = std::max(0.0, slen - slot_pos_sec_);

            const size_t next_idx = static_cast<size_t>((idx + 1) % n);
            auto next_clip = snap->clips[next_idx];
            if (next_clip) {
                const double warm_up = next_clip->warmUpSec();
                if (warm_up > 0.0 && remaining <= warm_up) {
                    // Capture under cursor_mu_; firing happens after we
                    // release it so the callback can re-enter peek()/advance
                    // siblings without deadlock.
                    fire_clip      = std::move(next_clip);
                    fire_remaining = remaining;
                }
            }
        }
    } // cursor_mu_ released

    // Reconcile the prepare token outside the cursor lock — fire at most
    // once per (slot, upcoming clip) combination.
    if (slot_rolled || fire_clip) {
        std::shared_ptr<IClip> deduped;
        PrepareCallback        cb_copy;
        {
            std::lock_guard<std::mutex> cb(cb_mu_);
            if (slot_rolled) last_prepared_.reset();
            if (fire_clip && fire_clip != last_prepared_) {
                last_prepared_ = fire_clip;
                deduped        = fire_clip;
                cb_copy        = prepare_cb_;
            }
        }
        if (deduped && cb_copy) cb_copy(deduped, fire_remaining);
    }
}

void Timeline::setPrepareCallback(PrepareCallback cb) {
    std::lock_guard<std::mutex> lk(cb_mu_);
    prepare_cb_ = std::move(cb);
    // Don't reset last_prepared_ here — operator may swap callbacks while
    // the same upcoming clip is still pending; we don't want to re-fire.
}

TimelineState Timeline::peek() const {
    auto snap = current_.load(std::memory_order_acquire);
    std::lock_guard<std::mutex> ck(cursor_mu_);

    if (snap->clips.empty()) {
        TimelineState s;
        s.clipA = fallback_.get();
        return s;
    }

    int idx = findActiveIdx(*snap, active_clip_);
    if (idx < 0) idx = 0;  // soft-heal — advance() will commit
    return buildState(*snap, static_cast<size_t>(idx), slot_pos_sec_,
                      fallback_.get());
}

int Timeline::getActiveIndex() const {
    auto snap = current_.load(std::memory_order_acquire);
    std::lock_guard<std::mutex> ck(cursor_mu_);
    if (snap->clips.empty()) return -1;
    int idx = findActiveIdx(*snap, active_clip_);
    return idx < 0 ? 0 : idx;
}

Timeline::CursorSnapshot Timeline::getCursorSnapshot() const {
    auto snap = current_.load(std::memory_order_acquire);
    std::lock_guard<std::mutex> ck(cursor_mu_);
    CursorSnapshot out;
    out.slot_pos_sec = slot_pos_sec_;
    out.active_clip  = active_clip_;
    if (!snap || snap->clips.empty()) {
        out.active_idx = -1;
    } else {
        int idx = findActiveIdx(*snap, active_clip_);
        out.active_idx = (idx < 0 ? 0 : idx);
    }
    return out;
}

void Timeline::snapCursorToActiveStart() {
    std::lock_guard<std::mutex> ck(cursor_mu_);
    slot_pos_sec_ = 0.0;
}

void Timeline::skipToNext() {
    auto snap = current_.load(std::memory_order_acquire);
    if (!snap || snap->clips.empty()) return;
    std::lock_guard<std::mutex> ck(cursor_mu_);
    const size_t n = snap->clips.size();
    int idx = findActiveIdx(*snap, active_clip_);
    if (idx < 0) idx = 0;
    idx = static_cast<int>((idx + 1) % n);
    active_clip_  = snap->clips[idx];
    slot_pos_sec_ = 0.0;
    // Mark the upcoming boundary as user-skip. RenderLoop will pick this up
    // on its next idx-change tick and tag the proof-of-play row accordingly.
    pending_boundary_status_.store(BoundaryStatus::SkippedUser,
                                   std::memory_order_release);
}

bool Timeline::restoreCursor(int idx, double slot_pos_sec) {
    auto snap = current_.load(std::memory_order_acquire);
    if (!snap || snap->clips.empty())                       return false;
    if (idx < 0 || static_cast<size_t>(idx) >= snap->clips.size())
        return false;
    auto clip = snap->clips[static_cast<size_t>(idx)];
    if (!clip) return false;

    // Clamp into the clip's actual duration so a stale snapshot can't push
    // the cursor past the slot boundary on first advance. A negative or NaN
    // value collapses to 0.0.
    const double dur = clip->getDuration();
    if (!std::isfinite(slot_pos_sec) || slot_pos_sec < 0.0) slot_pos_sec = 0.0;
    if (dur > 0.0 && slot_pos_sec >= dur) slot_pos_sec = 0.0;

    std::lock_guard<std::mutex> ck(cursor_mu_);
    active_clip_  = std::move(clip);
    slot_pos_sec_ = slot_pos_sec;
    return true;
}

const char* Timeline::boundaryStatusName(BoundaryStatus s) noexcept {
    switch (s) {
        case BoundaryStatus::Completed:   return "completed";
        case BoundaryStatus::SkippedUser: return "skipped_user";
        case BoundaryStatus::Removed:     return "removed";
    }
    return "completed";
}

Timeline::BoundaryStatus Timeline::consumeBoundaryStatus() noexcept {
    return pending_boundary_status_.exchange(BoundaryStatus::Completed,
                                             std::memory_order_acq_rel);
}

double Timeline::getRemainingTime() const {
    auto snap = current_.load(std::memory_order_acquire);
    std::lock_guard<std::mutex> ck(cursor_mu_);
    if (snap->clips.empty()) return 0.0;
    int idx = findActiveIdx(*snap, active_clip_);
    if (idx < 0) idx = 0;
    const size_t n    = snap->clips.size();
    const auto   own  = transitionFor(snap->transitions, idx);
    const auto   prev = prevTransition(snap->transitions, idx, n);
    const double slen = slotLength(*snap->clips[idx], prev, own);
    return std::max(0.0, slen - slot_pos_sec_);
}

// ─── Legacy stateless API (tests only) ───────────────────────────────────────

TimelineState Timeline::getState(double elapsed_sec) const {
    auto snap = current_.load(std::memory_order_acquire);
    if (snap->clips.empty()) {
        TimelineState s;
        s.clipA = fallback_.get();
        return s;
    }
    const double total = timelineTotal(*snap);
    if (total <= 0.0) return {};
    double t = std::fmod(elapsed_sec, total);
    if (t < 0.0) t += total;
    double slot_start = 0.0;
    const size_t idx  = findSlotIndex(*snap, t, &slot_start, nullptr);
    return buildState(*snap, idx, t - slot_start);
}

int Timeline::getCurrentIndex(double elapsed_sec) const {
    auto snap = current_.load(std::memory_order_acquire);
    if (snap->clips.empty()) return -1;
    const double total = timelineTotal(*snap);
    if (total <= 0.0) return -1;
    double t = std::fmod(elapsed_sec, total);
    if (t < 0.0) t += total;
    return static_cast<int>(findSlotIndex(*snap, t));
}

double Timeline::getRemainingTime(double elapsed_sec) const {
    auto snap = current_.load(std::memory_order_acquire);
    if (snap->clips.empty()) return 0.0;
    const double total = timelineTotal(*snap);
    if (total <= 0.0) return 0.0;
    double t = std::fmod(elapsed_sec, total);
    if (t < 0.0) t += total;
    double slot_start = 0.0;
    double slot_len   = 0.0;
    findSlotIndex(*snap, t, &slot_start, &slot_len);
    return (slot_start + slot_len) - t;
}

IClip* Timeline::getClipAt(int idx) const {
    auto snap = current_.load(std::memory_order_acquire);
    if (idx < 0 || static_cast<size_t>(idx) >= snap->clips.size()) return nullptr;
    return snap->clips[idx].get();
}

int Timeline::getPlaylistSize() const {
    return static_cast<int>(current_.load(std::memory_order_acquire)->clips.size());
}
