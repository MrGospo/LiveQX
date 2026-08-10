#pragma once
//
// fix17 step 3 — debounced writer for ChannelStatePersistence.
//
// One ChannelStateSaver per ChannelInstance. Owns a worker thread that
// coalesces save requests inside a small debounce window (default 500ms)
// so high-frequency triggers (every render-loop tick during play()) end
// up as ≤2 disk writes/sec.
//
// Flow:
//   trigger        - any code calls scheduleSave() on the channel
//   debouncer      - worker wakes on CV, sleeps `debounce` more, captures
//                    the snapshot via a caller-supplied lambda, writes it
//   force-flush    - flush() blocks until the queue drains (used by
//                    SIGTERM / dtor); ~ChannelStateSaver also flushes.
//
// scheduleSave() and flush() are safe from any thread. capture() is
// invoked from the worker thread only — caller is responsible for
// taking whatever locks the snapshot needs (Timeline mutex, etc.).
//
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

#include "persistence/StatePersistence.h"

namespace liveqx::persistence {

class ChannelStateSaver {
public:
    using Capture = std::function<ChannelStateSnapshot()>;

    ChannelStateSaver(ChannelStatePersistence& persist,
                      Capture capture,
                      std::chrono::milliseconds debounce =
                          std::chrono::milliseconds(500));
    ~ChannelStateSaver();   // joins thread + force-flushes any dirty state

    ChannelStateSaver(const ChannelStateSaver&)            = delete;
    ChannelStateSaver& operator=(const ChannelStateSaver&) = delete;

    // Mark the channel dirty. Worker wakes, sleeps `debounce_` more, then
    // calls capture() + persist.save(). Coalesces freely — N requests in
    // a row collapse into one write.
    void scheduleSave();

    // Block until any pending dirty state has been written. Returns
    // immediately if nothing is queued. Safe to call concurrently with
    // scheduleSave().
    void flush();

private:
    void run();

    ChannelStatePersistence& persist_;
    Capture                  capture_;
    std::chrono::milliseconds debounce_;

    std::mutex              mu_;
    std::condition_variable wake_cv_;
    std::condition_variable flushed_cv_;
    bool                    dirty_      = false;
    bool                    stop_       = false;
    bool                    in_flight_  = false;     // worker is writing
    std::uint64_t           writes_done_ = 0;        // monotonic counter

    std::thread worker_;
};

}  // namespace liveqx::persistence
