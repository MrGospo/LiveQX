#pragma once
//
// The list of revoked player certificates, for the MQTT broker (A9, B8; the third
// lock). The broker checks it when a player connects: a revoked certificate does
// not even get through the handshake.
//
// What it writes: one file holding one CRL per generation of the device CA, each
// signed by that generation's own key. EVERY generation gets one, empty or not —
// the broker refuses a certificate whose issuer has no list at all, so a missing
// list would shut out every screen of that generation. Certificates stay valid
// after a newer generation takes over, hence the older generations too.
//
// Two ways this can go wrong, and what is done about them:
//   * A list has an expiry date, and past it the broker rejects EVERYONE. So the
//     file is rewritten and the broker told to re-read it on a schedule (daily),
//     long before the date (90 days ahead), not only when someone is revoked. The
//     refresh must come at least three times within the validity, or it is made so.
//   * Writing can fail half way. The file is replaced atomically, and if any
//     generation cannot be signed nothing is written: the old file stays in force.
//
// The broker is told with a callback, so tests need no broker and a remote
// broker (which cannot be signalled) can simply have none.

#include "players/PlayersDb.h"
#include "utils/DeviceCa.h"

#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace liveqx::players {

struct CrlPublisherOptions {
    std::filesystem::path path;                  // the file the broker reads
    int                   next_update_days{90};  // how long a list is valid
    std::int64_t          refresh_interval_s{86400};
    std::int64_t          retry_s{60};           // after a failure
    std::int64_t          waiting_retry_s{10};   // while there is no device CA yet (normal before the ceremony)
    // Which generations of the device CA exist, and how to open one.
    std::function<std::vector<int>()> generations;
    std::function<std::unique_ptr<tls::DeviceCa>(int generation, std::string& err)> open_generation;
    // Tells the broker to re-read the file. Returns "" or the reason it could not.
    // Empty = there is nobody to tell (the file is written all the same).
    std::function<std::string()> reload;
    std::function<std::int64_t()> now;           // unix seconds; null = real clock
    // Brings the rest of what the broker needs (its CA file and certificate: BrokerTls) in place
    // before the list is written. Returns "" or why it could not; `changed` is set when something
    // was written. Null = nothing to prepare.
    std::function<std::string(bool& changed)> prepare;
    // Created once the broker has everything: the list and whatever `prepare` made. A service that
    // waits for this path can start the broker. The server never removes it. Empty = none.
    std::filesystem::path ready_path;
    // Called (no lock held) after each attempt, so a page that shows the status can refresh. Keep it short.
    std::function<void()> on_change;
};

struct CrlStatus {
    std::int64_t last_attempt_at{0};
    std::int64_t last_publish_at{0};             // the last time the file was really written
    bool         last_ok{false};
    bool         waiting{false};                 // no device CA yet: nothing to publish, and that is not a fault
    std::string  error;                          // why the last attempt failed ("" if it did not)
    std::string  prepare_error;                  // the list is written but the broker's other files are not right
    std::string  reload_error;                   // the file was written but the broker was not told
    std::size_t  revoked{0};                     // entries in the lists
    std::size_t  generations{0};
    std::int64_t valid_until{0};                 // when the lists in the file run out (read from the file at start)
};

class CrlPublisher {
public:
    CrlPublisher(PlayersDb& db, CrlPublisherOptions opts);
    ~CrlPublisher();
    CrlPublisher(const CrlPublisher&)            = delete;
    CrlPublisher& operator=(const CrlPublisher&) = delete;

    // Writes the file and tells the broker, now, on the calling thread.
    CrlStatus publishNow();

    // Starts the background thread: publishes at once, again whenever asked, and
    // on the refresh schedule (sooner after a failure).
    void start();
    void stop();
    // Something was revoked: publish soon. Never blocks.
    void request();

    CrlStatus status() const;
    // How often the list is rewritten (after the check against how long it is valid).
    std::int64_t refreshIntervalSeconds() const { return opts_.refresh_interval_s; }

private:
    void run();

    PlayersDb&          db_;
    CrlPublisherOptions opts_;

    mutable std::mutex      mu_;
    std::condition_variable cv_;
    CrlStatus               status_;
    bool                    wanted_{false};
    bool                    stopping_{false};
    std::mutex              publish_mu_;         // one publication at a time
    std::thread             thread_;
};

}  // namespace liveqx::players
