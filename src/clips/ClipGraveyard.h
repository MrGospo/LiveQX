#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include "clips/IClip.h"

// Off-thread destructor pool for IClip instances.
//
// Problem this solves: ~VideoClip() (and other IClip subclasses with worker
// threads) calls release() → stopThreads() synchronously, joining 3+ decoder
// threads — a 30-50ms blocking operation. When the last shared_ptr<IClip>
// drops on the render thread inside Preloader::tick() / Timeline mutations,
// the destructor blows the 40ms frame budget and produces visible jitter.
//
// Usage: own one ClipGraveyard per channel (declared as the FIRST member of
// ChannelInstance so it destructs last, after Timeline/Preloader have finished
// burying their shared_ptr drops). Wrap clip shared_ptrs via wrapClip() in
// IClip.h — its custom deleter routes deletion through bury() instead of
// `delete p`. The single worker thread inside Graveyard then runs the actual
// destructor off the calling thread.
//
// Bury is O(1) and never blocks (mutex held only for queue push). Shutdown
// drains all pending objects on the worker before the worker exits, so no
// IClip leaks.
class ClipGraveyard {
public:
    // Spawns the worker thread.
    ClipGraveyard();

    // Signals the worker to stop, drains any remaining queued objects on
    // the worker thread (so the destructors still run off-caller), then joins.
    ~ClipGraveyard();

    ClipGraveyard(const ClipGraveyard&) = delete;
    ClipGraveyard& operator=(const ClipGraveyard&) = delete;
    ClipGraveyard(ClipGraveyard&&) = delete;
    ClipGraveyard& operator=(ClipGraveyard&&) = delete;

    // Hand off ownership of `p` to the graveyard worker for asynchronous
    // destruction. Caller must not access `p` after this call.
    //
    // If the graveyard is in shutdown (~ClipGraveyard already started), the
    // object is destroyed inline on the calling thread — there is no worker
    // left to take it. This only happens during process teardown when the
    // render loop is already stopped, so the synchronous fallback cannot
    // affect runtime jitter.
    //
    // noexcept: bury must never throw — it is called from custom deleters
    // inside shared_ptr destruction.
    void bury(IClip* p) noexcept;

    // Diagnostic: queue depth at this instant. Lock-free read of the
    // approximate count. Useful for metrics / stress tests.
    std::size_t pendingCount() const noexcept {
        return pending_.load(std::memory_order_relaxed);
    }

    // Synchronously block until the queue is fully drained. Intended for
    // shutdown paths (Timeline::clear, ChannelInstance::stop) where the
    // caller wants to guarantee no IClip outlives the channel. Called
    // from non-render threads only — drainSync() can take 30-50ms per
    // queued clip.
    void drainSync();

private:
    void workerLoop();

    std::mutex                              mtx_;
    std::condition_variable                 cv_;
    std::deque<std::unique_ptr<IClip>>      queue_;
    std::atomic<bool>                       stop_{false};
    std::atomic<std::size_t>                pending_{0};
    std::thread                             worker_;

    // Signaled by the worker each time the queue empties. drainSync()
    // waits on it. Separate from cv_ so a sleeping caller is not woken
    // by every bury() that puts work on the queue.
    std::condition_variable                 drained_cv_;
};

// Wrap an owning unique_ptr into a shared_ptr whose deleter routes
// destruction through `g.bury(p)` instead of `delete p`. Use this at
// every unique→shared transition for clips that may be observed by the
// render thread (Timeline / Preloader / RenderLoop). The graveyard's
// lifetime must outlive every shared_ptr produced by this helper —
// declare the ClipGraveyard as the FIRST member of the owning
// channel/instance struct so it destructs LAST.
inline std::shared_ptr<IClip> wrapClip(std::unique_ptr<IClip> p,
                                       ClipGraveyard&         g) noexcept {
    if (!p) return {};
    return std::shared_ptr<IClip>(p.release(),
                                  [&g](IClip* raw) noexcept { g.bury(raw); });
}

// Raw-ptr variant for call sites that have ownership but not a
// unique_ptr in hand (e.g. constructing a freshly-new'd FallbackClip).
inline std::shared_ptr<IClip> wrapClip(IClip*         p,
                                       ClipGraveyard& g) noexcept {
    if (!p) return {};
    return std::shared_ptr<IClip>(p,
                                  [&g](IClip* raw) noexcept { g.bury(raw); });
}

// Convenience: drain a vector<unique_ptr> through wrapClip into a
// vector<shared_ptr> ready for Timeline::setPlaylist. Used by
// ChannelInstance::buildLongLived / replacePlaylist / schedule swap.
inline std::vector<std::shared_ptr<IClip>>
wrapClips(std::vector<std::unique_ptr<IClip>>&& src,
          ClipGraveyard&                         g) noexcept {
    std::vector<std::shared_ptr<IClip>> out;
    out.reserve(src.size());
    for (auto& u : src) out.emplace_back(wrapClip(std::move(u), g));
    return out;
}
