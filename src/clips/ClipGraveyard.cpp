#include "clips/ClipGraveyard.h"

#include "utils/Log.h"

ClipGraveyard::ClipGraveyard() {
    worker_ = std::thread([this] { workerLoop(); });
}

ClipGraveyard::~ClipGraveyard() {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        stop_.store(true, std::memory_order_release);
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void ClipGraveyard::drainSync() {
    std::unique_lock<std::mutex> lk(mtx_);
    drained_cv_.wait(lk, [this] {
        return queue_.empty() && pending_.load(std::memory_order_relaxed) == 0;
    });
}

void ClipGraveyard::bury(IClip* p) noexcept {
    if (!p) return;

    // Shutdown fallback: if the worker has already exited (or is in
    // mid-shutdown and won't pick up new work), destroy inline on the
    // caller. Acceptable because by the time this branch fires, the
    // render loop is stopped — no realtime budget to protect.
    if (stop_.load(std::memory_order_acquire)) {
        std::unique_ptr<IClip> owned(p);
        return;
    }

    try {
        std::unique_ptr<IClip> owned(p);
        {
            std::lock_guard<std::mutex> lk(mtx_);
            // Re-check under lock — the worker could have flipped stop_
            // between our acquire-load and lock acquisition.
            if (stop_.load(std::memory_order_relaxed)) {
                return;  // owned destructs here, on caller thread
            }
            queue_.emplace_back(std::move(owned));
            pending_.fetch_add(1, std::memory_order_relaxed);
        }
        cv_.notify_one();
    } catch (...) {
        // Allocation failure for unique_ptr or queue push — extremely
        // unlikely. Fall back to inline destruction; bury() must not
        // throw because it is called from shared_ptr's deleter context
        // where exceptions are forbidden.
        std::unique_ptr<IClip> owned(p);
        (void)owned;
    }
}

void ClipGraveyard::workerLoop() {
    for (;;) {
        std::unique_ptr<IClip> victim;
        {
            std::unique_lock<std::mutex> lk(mtx_);
            cv_.wait(lk, [this] {
                return stop_.load(std::memory_order_relaxed)
                       || !queue_.empty();
            });
            if (queue_.empty()) {
                // stop_ was set and queue is drained — exit.
                return;
            }
            victim = std::move(queue_.front());
            queue_.pop_front();
        }
        // Run the destructor off-lock. ~Derived() may be 30-50ms (decoder
        // thread joins) or longer; holding mtx_ here would serialize bury()
        // calls behind it, defeating the off-thread guarantee.
        //
        // pending_ is decremented AFTER the dtor finishes (not after pop),
        // so pendingCount() == "objects not yet fully destroyed" — that is
        // also the invariant drainSync() waits on.
        try {
            victim.reset();
        } catch (const std::exception& e) {
            LOG_ERROR("ClipGraveyard: destructor threw: {}", e.what());
        } catch (...) {
            LOG_ERROR("ClipGraveyard: destructor threw unknown exception");
        }
        {
            std::lock_guard<std::mutex> lk(mtx_);
            pending_.fetch_sub(1, std::memory_order_acq_rel);
            if (queue_.empty()
                    && pending_.load(std::memory_order_relaxed) == 0) {
                drained_cv_.notify_all();
            }
        }
    }
}
