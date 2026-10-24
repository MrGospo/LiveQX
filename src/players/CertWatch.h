#pragma once
//
// Watches how close the certificates of the screens are to their end and speaks up
// when a screen moves from one step to another (see CertLifecycle.h): a line in
// the screen's journal (`cert_stage`) and an event for the pages that are open.
//
// Nothing is repeated: the step last written to the journal is the memory, so a
// restart of the server does not announce the same thing again, and a screen that
// has been bound again or has renewed is announced as "ok" once.
//
// It looks once an hour. The steps are measured in days, so an hour's delay is
// nothing; what the API shows is computed afresh on every request.

#include "events/EventBus.h"
#include "players/PlayersDb.h"

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>

namespace liveqx::players {

struct CertWatchOptions {
    events::EventBus*             events{nullptr};
    std::function<std::int64_t()> now;                  // unix seconds; null = the clock
    int                           interval_s{3600};
    int                           first_look_after_s{60};
};

class CertWatch {
public:
    CertWatch(PlayersDb& db, CertWatchOptions opts);
    ~CertWatch();
    CertWatch(const CertWatch&)            = delete;
    CertWatch& operator=(const CertWatch&) = delete;

    void start();
    void stop();

    // One look now. Returns how many screens changed their step.
    std::size_t scan();

private:
    void run();

    PlayersDb&       db_;
    CertWatchOptions opts_;
    std::thread      thread_;
    std::mutex       mu_;
    std::condition_variable cv_;
    bool             stopping_{false};
};

}  // namespace liveqx::players
