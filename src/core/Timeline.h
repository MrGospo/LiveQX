#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "clips/IClip.h"
#include "transitions/ITransition.h"

struct TimelineState {
    IClip* clipA              = nullptr;
    IClip* clipB              = nullptr;   // non-null only during a transition
    bool   in_transition      = false;
    bool   clipA_in_tail      = false;     // RenderLoop: getTailFrame iff true
    bool   clipB_in_tail      = false;     // RenderLoop: getTailFrame iff true
                                           //  (FreezeFade ⇒ true; LiveMix ⇒ false)
    float  transition_progress = 0.0f;    // 0.0 = fully A, 1.0 = fully B
    TransitionType transition_type = TransitionType::CrossFade;
    Easing transition_easing = Easing::Linear;  // fix20
};

// Immutable view of the playlist. Exposed via Timeline::snapshot() so consumers
// can hold a stable copy across a render tick while ContentSync mutates the
// active version.
struct PlaylistSnapshot {
    std::vector<std::shared_ptr<IClip>> clips;
    std::vector<TransitionConfig>       transitions;
    std::vector<std::string>            cache_paths;     // parallel to clips, "" if not cache-backed
    std::vector<bool>                   pending_remove;  // parallel to clips
};

// Scheduler with a stateful cursor on the render-loop thread plus a stateless
// fmod-based legacy API used by tests. Snapshot pointer is swapped atomically;
// readers are lock-free.
class Timeline {
public:
    Timeline();

    // Move ctor for test ergonomics (return-by-value). Not safe to move while
    // mutators or readers are active on the source — production code keeps
    // Timeline pinned in a Channel struct.
    Timeline(Timeline&& other) noexcept;
    Timeline& operator=(Timeline&&) = delete;
    Timeline(const Timeline&)       = delete;
    Timeline& operator=(const Timeline&) = delete;

    // Replaces the playlist atomically. Existing snapshot stays valid for any
    // reader still holding it. Resets the cursor to the start.
    //
    // unique_ptr overloads convert via the default deleter — production code
    // (ChannelInstance / ContentSync) MUST use the shared_ptr overloads with
    // wrapClip(...) so render-thread drops route through ClipGraveyard.
    // unique_ptr overloads remain for tests that don't need the graveyard.
    void setPlaylist(std::vector<std::unique_ptr<IClip>> clips,
                     std::vector<TransitionConfig>        transitions);

    // Same as above but tags each clip with its cache path (used by
    // ContentSync to match remove requests).
    void setPlaylist(std::vector<std::unique_ptr<IClip>> clips,
                     std::vector<TransitionConfig>        transitions,
                     std::vector<std::string>             cache_paths);

    // shared_ptr overloads — production path. Caller is responsible for
    // wrapping the clips through wrapClip(uniq, graveyard) so that the
    // last-shared_ptr-drop on the render thread routes destruction off-thread.
    void setPlaylist(std::vector<std::shared_ptr<IClip>> clips,
                     std::vector<TransitionConfig>        transitions);
    void setPlaylist(std::vector<std::shared_ptr<IClip>> clips,
                     std::vector<TransitionConfig>        transitions,
                     std::vector<std::string>             cache_paths);

    // Shown when the playlist is empty. fix13 c9 — takes shared_ptr so
    // ChannelInstance can keep a co-owning pointer and feed it into a
    // LiveClip's OnLossProvider without rebuilding it twice. unique_ptr
    // call sites still work via the implicit unique→shared conversion.
    void setFallback(std::shared_ptr<IClip> fallback);

    // ── Hot mutation (Phase B) ───────────────────────────────────────────────
    // Append a clip + transition to the END of the playlist. Safe to call
    // concurrently with readers; serialized against other mutators. Cursor
    // stays on whatever clip it was on — no jump.
    void appendClip(std::shared_ptr<IClip> clip,
                    TransitionConfig       transition,
                    std::string            cache_path = "");

    // Mark a clip (matched by cache_path) for removal. The clip stays
    // playable until reapRemovable() is invoked at a safe boundary.
    // Returns true if a matching unmarked entry was found.
    bool markForRemoval(const std::string& cache_path);

    // Build a new snapshot dropping every entry where pending_remove==true
    // and cache_path != active_cache_path. Returns the cache_paths that were
    // physically removed from the playlist (caller is responsible for
    // evicting from the cache disk). Cursor self-heals on next advance() if
    // the active clip is no longer present.
    // Also drains any cache_paths that were auto-reaped via
    // reapPendingActive() since the previous call.
    std::vector<std::string> reapRemovable(const std::string& active_cache_path);

    // Remove the entry at `idx` from the playlist.
    //
    // If `idx` matches the cursor's currently-active clip, the entry is only
    // marked pending_remove — the Preloader will drop it at the natural slot
    // wrap via reapPendingActive(), so playback is not cut mid-frame.
    // Otherwise the snapshot is rebuilt without the entry immediately. Cursor
    // is unaffected (anchored on shared_ptr identity, not index).
    enum class RemoveResult { Removed, MarkedActive, NotFound };
    RemoveResult removeAt(std::size_t idx);

    // Drop the active entry from the playlist if it is pending_remove.
    // Called by the Preloader at slot wrap when the active clip's source
    // has been deleted — gives the Timeline a clean transition boundary
    // to swap to fallback. The evicted cache_path is queued for the next
    // reapRemovable() so ContentSync can clean up the cache disk.
    bool reapPendingActive();

    // Snapshot pointer — stable for the lifetime of the returned shared_ptr.
    // Hot path consumers (RenderLoop) may hold one snapshot per tick.
    std::shared_ptr<const PlaylistSnapshot> snapshot() const noexcept;

    // ── Stateful cursor API (render-loop thread) ─────────────────────────────
    // The cursor anchors on the *active clip identity* (shared_ptr), not on
    // an index. Hot mutations (appendClip / reapRemovable) shift indices and
    // change total length, but the cursor stays on the same clip with the
    // same intra-slot position — no jumps mid-playback.
    //
    // Designed for a single driver thread. advance() must not run concurrently
    // with peek() / getActiveIndex() / getRemainingTime() from another thread.
    // Mutations on other threads are safe.
    void          advance(double dt_sec);
    TimelineState peek() const;
    int           getActiveIndex() const;        // -1 if playlist empty
    double        getRemainingTime() const;      // remaining in active slot (incl. tail)

    // fix17 — thread-safe snapshot of (idx, slot_pos_sec, active_clip)
    // for the persistence layer. Reads under cursor_mu_; safe to call
    // concurrently with mutators and with the render thread's
    // advance()/peek() (the lock is per-call so the values are
    // self-consistent even if the cursor moves between calls).
    struct CursorSnapshot {
        int                    active_idx = -1;
        double                 slot_pos_sec = 0.0;
        std::shared_ptr<IClip> active_clip;
    };
    CursorSnapshot getCursorSnapshot() const;

    // Force the cursor to the next clip (slot_pos = 0). No-op on empty playlist.
    // Safe to call from any thread — locks cursor_mu_ briefly.
    void          skipToNext();

    // fix17 — restore the cursor to a previously-persisted (idx, slot_pos)
    // anchor. No-op when the playlist is empty or `idx` is out of range. The
    // cursor re-anchors on the matching shared_ptr identity so subsequent
    // hot-mutations preserve the restored position. slot_pos is clamped to
    // [0, clip->getDuration()) — out-of-bounds restores resume from the
    // start of the same clip rather than overshooting into the next slot.
    // Returns true if the cursor was actually moved.
    bool          restoreCursor(int idx, double slot_pos_sec);

    // ── Boundary status (fix8 step 8) ────────────────────────────────────────
    // Captures *why* the active clip is about to change. Default Completed,
    // overwritten by skipToNext / reapPendingActive at the moment the cursor
    // is bumped. RenderLoop calls consumeBoundaryStatus() on the next tick
    // where it observes idx change, atomically resetting to Completed.
    enum class BoundaryStatus { Completed, SkippedUser, Removed };
    static const char* boundaryStatusName(BoundaryStatus s) noexcept;
    BoundaryStatus     consumeBoundaryStatus() noexcept;

    // ── Prepare callback (fix13 c5) ──────────────────────────────────────────
    // Fired once per upcoming clip when the cursor enters that clip's
    // warm-up window — i.e. the remaining time in the active slot drops
    // below next_clip->warmUpSec(). Lets ChannelInstance kick off a slow
    // open (RTMP handshake, multicast IGMP join) ahead of the cut so the
    // boundary isn't dead air.
    //
    // The callback runs synchronously from advance() on the render thread
    // — the consumer is expected to enqueue the actual prepare work onto
    // a thread pool and return immediately. Re-entering Timeline from
    // inside the callback is undefined.
    //
    // `seconds_until_start` is how far we still are from the boundary
    // (>= 0); ChannelInstance turns it into an absolute scheduled-start
    // wall-clock for LiveClip::setScheduledStart().
    //
    // Each upcoming clip fires at most once per inbound slot crossing
    // — Timeline tracks identity of the last prepared clip and clears
    // it when the slot rolls.
    using PrepareCallback =
        std::function<void(std::shared_ptr<IClip> next, double seconds_until_start)>;
    void setPrepareCallback(PrepareCallback cb);

    // Reset the intra-slot position to 0 without changing the active clip.
    // Used by Preloader after a single-clip wrap reset() to keep the cursor
    // aligned with the clip's freshly-rewound internal playback — otherwise
    // slot_pos and clip's internal time drift apart by the reset lead time
    // and EOF starts arriving mid-slot.
    void          snapCursorToActiveStart();

    // ── Legacy stateless API (tests only) ────────────────────────────────────
    // Pure functions of elapsed_sec — do NOT consult the cursor. Kept for
    // existing test coverage of fmod-based scheduling math. New code should
    // use the cursor API.
    TimelineState getState(double elapsed_sec) const;
    int    getCurrentIndex  (double elapsed_sec) const;
    double getRemainingTime (double elapsed_sec) const;

    IClip* getClipAt        (int idx)            const;
    int    getPlaylistSize  ()                   const;

private:
    // Atomic shared_ptr swap (C++20). Writers serialize on mutator_mu_.
    std::atomic<std::shared_ptr<const PlaylistSnapshot>> current_;
    std::mutex                                           mutator_mu_;
    std::shared_ptr<IClip>                               fallback_;

    // Cursor — pointer-anchored playback position. Protected by cursor_mu_.
    // Mutators clear active_clip_ if it gets reaped; advance() then heals.
    mutable std::mutex     cursor_mu_;
    std::shared_ptr<IClip> active_clip_;
    double                 slot_pos_sec_ = 0.0;

    // cache_paths auto-reaped by reapPendingActive() but not yet drained
    // by reapRemovable(). Protected by mutator_mu_.
    std::vector<std::string> auto_reaped_paths_;

    // Boundary status for the *next* observed clip change. Last writer wins
    // (skip then reap collapses to Removed; reap then skip → SkippedUser).
    // Render thread resets to Completed on every consume.
    std::atomic<BoundaryStatus> pending_boundary_status_{BoundaryStatus::Completed};

    // Prepare callback (fix13 c5). Stored under cb_mu_ so REST/init can
    // swap it while advance() is running on the render thread. Token
    // last_prepared_ deduplicates fires across consecutive advance() ticks
    // for the same upcoming clip; cleared on slot crossings and on
    // setPlaylist.
    mutable std::mutex             cb_mu_;
    PrepareCallback                prepare_cb_;
    std::shared_ptr<IClip>         last_prepared_;
};
