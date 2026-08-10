// fix26 c10 — internal cron scheduler for the stress runner.
//
// Owns a jthread that wakes every poll_interval (default 30s), checks the
// current cron expression against the current minute, and invokes the
// callback at most once per matching minute. Hot-reload of the cron is
// allowed via setCron(); the next poll picks up the new expression.
//
// We track the last-fired minute (epoch seconds // 60) to deduplicate
// across multiple polls within the same minute.

#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "stress/CronExpr.h"

namespace liveqx::stress {

class StressScheduler {
public:
    using Callback = std::function<void()>;

    // poll_interval governs how often the scheduler wakes to test the cron.
    // 30s is a good balance: minute-granular cron, but we don't want to
    // sleep a full 60s and miss the minute we wanted to fire on if startup
    // was unlucky.
    explicit StressScheduler(std::string  cron,
                             Callback     cb,
                             std::chrono::seconds poll_interval = std::chrono::seconds{30});
    ~StressScheduler();

    StressScheduler(const StressScheduler&)            = delete;
    StressScheduler& operator=(const StressScheduler&) = delete;

    // Returns true if the new expression parsed; false leaves the previous
    // schedule in place. err_out is populated with the parser message on
    // failure so REST can return a useful 400.
    bool setCron(const std::string& cron, std::string* err_out = nullptr);

    std::string cron() const;

    // Test hook: drive the scheduler manually with a synthetic timestamp.
    // Returns true if the callback was invoked on this tick.
    bool pokeForTest(std::time_t utc_seconds);

private:
    void loop(std::stop_token st);

    mutable std::mutex     mu_;
    std::shared_ptr<CronExpr> expr_;          // current parsed cron
    std::string            source_;
    Callback               cb_;
    std::chrono::seconds   poll_interval_;
    std::atomic<std::int64_t> last_fired_minute_{-1};
    std::jthread           th_;               // started last (kept after all members init)
};

}  // namespace liveqx::stress
