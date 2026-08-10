#include "stress/StressScheduler.h"

#include <ctime>
#include <utility>

#include <spdlog/spdlog.h>

namespace liveqx::stress {

StressScheduler::StressScheduler(std::string cron,
                                 Callback cb,
                                 std::chrono::seconds poll_interval)
    : source_(std::move(cron)),
      cb_(std::move(cb)),
      poll_interval_(poll_interval) {
    std::string err;
    auto parsed = CronExpr::parse(source_, &err);
    if (!parsed) {
        spdlog::warn("stress[scheduler]: invalid cron '{}' ({}) — scheduler"
                     " will be inert until setCron is called",
                     source_, err);
    } else {
        expr_ = std::make_shared<CronExpr>(std::move(*parsed));
    }
    th_ = std::jthread([this](std::stop_token st) { loop(st); });
}

StressScheduler::~StressScheduler() {
    th_.request_stop();
    // jthread join is automatic on destruction.
}

bool StressScheduler::setCron(const std::string& cron, std::string* err_out) {
    std::string err;
    auto parsed = CronExpr::parse(cron, &err);
    if (!parsed) {
        if (err_out) *err_out = err;
        return false;
    }
    auto fresh = std::make_shared<CronExpr>(std::move(*parsed));
    {
        std::scoped_lock lk(mu_);
        expr_   = std::move(fresh);
        source_ = cron;
    }
    return true;
}

std::string StressScheduler::cron() const {
    std::scoped_lock lk(mu_);
    return source_;
}

bool StressScheduler::pokeForTest(std::time_t utc_seconds) {
    std::shared_ptr<CronExpr> snapshot;
    {
        std::scoped_lock lk(mu_);
        snapshot = expr_;
    }
    if (!snapshot) return false;
    if (!snapshot->matches(utc_seconds)) return false;
    const std::int64_t minute = static_cast<std::int64_t>(utc_seconds) / 60;
    if (last_fired_minute_.exchange(minute) == minute) return false;
    if (cb_) cb_();
    return true;
}

void StressScheduler::loop(std::stop_token st) {
    while (!st.stop_requested()) {
        // Snapshot the current expression so a setCron during cb_() doesn't
        // race with us.
        std::shared_ptr<CronExpr> snapshot;
        {
            std::scoped_lock lk(mu_);
            snapshot = expr_;
        }
        if (snapshot) {
            const auto now = std::time(nullptr);
            if (snapshot->matches(now)) {
                const std::int64_t minute = static_cast<std::int64_t>(now) / 60;
                if (last_fired_minute_.exchange(minute) != minute) {
                    try {
                        if (cb_) cb_();
                    } catch (const std::exception& e) {
                        spdlog::error("stress[scheduler]: callback threw: {}",
                                      e.what());
                    } catch (...) {
                        spdlog::error("stress[scheduler]: callback threw");
                    }
                }
            }
        }

        // Cooperative sleep — wake early if stop requested.
        const auto step = std::chrono::milliseconds{200};
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            poll_interval_);
        while (remaining.count() > 0 && !st.stop_requested()) {
            const auto chunk = std::min<decltype(step)>(step, remaining);
            std::this_thread::sleep_for(chunk);
            remaining -= chunk;
        }
    }
}

}  // namespace liveqx::stress
