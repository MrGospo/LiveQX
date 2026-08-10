// fix17 step 3 — debounced writer thread for ChannelStatePersistence.
#include "persistence/ChannelStateSaver.h"

#include "utils/Log.h"

namespace liveqx::persistence {

ChannelStateSaver::ChannelStateSaver(ChannelStatePersistence& persist,
                                     Capture capture,
                                     std::chrono::milliseconds debounce)
    : persist_(persist),
      capture_(std::move(capture)),
      debounce_(debounce) {
    worker_ = std::thread([this] { run(); });
}

ChannelStateSaver::~ChannelStateSaver() {
    {
        std::lock_guard lk(mu_);
        stop_ = true;
    }
    wake_cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void ChannelStateSaver::scheduleSave() {
    {
        std::lock_guard lk(mu_);
        dirty_ = true;
    }
    wake_cv_.notify_one();
}

void ChannelStateSaver::flush() {
    std::unique_lock lk(mu_);
    if (!dirty_ && !in_flight_) return;
    // Ask the worker to skip the debounce on the *next* iteration.
    // Using writes_done_ as a wait-token: capture the current counter,
    // wait until it advances *and* dirty/in_flight have settled.
    const auto target = writes_done_ + (dirty_ ? 1 : 0);
    // Nudge the worker so it doesn't sleep through the full debounce.
    wake_cv_.notify_all();
    flushed_cv_.wait(lk, [&] {
        return !dirty_ && !in_flight_ && writes_done_ >= target;
    });
}

void ChannelStateSaver::run() {
    while (true) {
        ChannelStateSnapshot snap;
        {
            std::unique_lock lk(mu_);
            wake_cv_.wait(lk, [&] { return dirty_ || stop_; });

            // Debounce window — coalesce subsequent triggers. flush()
            // is allowed to interrupt by notify; we proceed regardless
            // since we already know dirty_ is set.
            if (!stop_) {
                wake_cv_.wait_for(lk, debounce_, [&] { return stop_; });
            }

            if (!dirty_) {
                if (stop_) {
                    flushed_cv_.notify_all();
                    return;
                }
                continue;
            }

            // Capture under-lock so scheduleSave() that arrives mid-capture
            // doesn't lose the dirty bit (we clear *before* calling capture
            // so concurrent triggers re-set it for a follow-up write).
            dirty_     = false;
            in_flight_ = true;
            lk.unlock();

            try {
                snap = capture_();
            } catch (const std::exception& e) {
                LOG_ERROR("ChannelStateSaver: capture threw: {}", e.what());
                lk.lock();
                in_flight_ = false;
                ++writes_done_;
                flushed_cv_.notify_all();
                if (stop_) return;
                continue;
            }
        }

        const bool ok = persist_.save(snap);
        if (!ok) {
            LOG_WARN("ChannelStateSaver: save() returned false at {}",
                     persist_.path().string());
        }

        bool should_exit;
        {
            std::lock_guard lk(mu_);
            in_flight_ = false;
            ++writes_done_;
            flushed_cv_.notify_all();
            should_exit = stop_ && !dirty_;
        }
        if (should_exit) return;
    }
}

}  // namespace liveqx::persistence
