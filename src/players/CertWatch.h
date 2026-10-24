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
// The same look tells whether certificate renewal is failing across the fleet (see
// renewAlarm in CertSummary.h) and speaks once when that begins and once when it ends:
// an event for the administrators' pages, a line in the log, and a callback for the audit trail.
// (The state is kept in memory: after a restart a fault that is still there is announced once more.)
//
// The server's own certificates (the API's TLS, the device authority, the broker's certificate,
// the revocation list's end) are watched the same way, in the same steps: a step reached is
// announced once to the administrators, in the audit trail and in the log, and "fine" once
// when it is renewed. (Kept in memory: after a restart a step that still stands is announced once more.)
//
// It looks once an hour. The steps are measured in days, so an hour's delay is
// nothing; what the API shows is computed afresh on every request.

#include "events/EventBus.h"
#include "players/CertSummary.h"
#include "players/PlayersDb.h"

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace liveqx::players {

struct CertWatchOptions {
    events::EventBus*             events{nullptr};
    std::function<std::int64_t()> now;                  // unix seconds; null = the clock
    int                           interval_s{3600};
    int                           first_look_after_s{60};
    // Renewal began / stopped failing across the fleet; called with no lock held.
    std::function<void(bool active, const ScreenCertSummary&)> on_renew_alarm;
    // The server's own certificates; null = not watched.
    std::function<std::vector<ServerCertEntry>()> server_certs;
    // One of them moved to another step; `days_left` rounded down, negative once over. No lock held.
    std::function<void(const ServerCertEntry&, CertStage stage, CertStage previous, std::int64_t days_left)> on_server_cert;
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
    bool             alarm_{false};                      // renewal is failing across the fleet, as last announced
    std::map<std::string, CertStage> server_stage_;      // the step of each server certificate, as last announced
};

}  // namespace liveqx::players
