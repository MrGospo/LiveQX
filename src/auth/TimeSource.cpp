// fix33 A2/A3 — реализация TimeSourceManager'а.

#include "auth/TimeSource.h"

#include "auth/NtpSource.h"
#include "auth/SntpClient.h"

namespace liveqx::auth {

TimeSourceManager::TimeSourceManager()
    : source_(std::make_unique<SystemLocalSource>()) {}

void TimeSourceManager::setNtpDependencies(std::shared_ptr<ISntpClient> sntp,
                                           NtpSyncCallback              on_sync) {
    std::unique_lock lk(mu_);
    sntp_    = std::move(sntp);
    on_sync_ = std::move(on_sync);
}

void TimeSourceManager::reconfigure(const TimeConfig& cfg) {
    std::unique_ptr<ITimeSource> next;
    // Снимаем NTP-deps под shared_lock'ом — после swap'а владелец source_
    // держит свою копию shared_ptr.
    std::shared_ptr<ISntpClient> sntp_copy;
    NtpSyncCallback              cb_copy;
    {
        std::shared_lock lk(mu_);
        sntp_copy = sntp_;
        cb_copy   = on_sync_;
    }

    switch (cfg.source) {
        case TimeSource::SystemLocal:
            next = std::make_unique<SystemLocalSource>();
            break;
        case TimeSource::Manual:
            next = std::make_unique<ManualOffsetSource>(cfg.manual.offset_ms);
            break;
        case TimeSource::Ntp:
            if (sntp_copy) {
                next = std::make_unique<NtpSource>(cfg.ntp, sntp_copy, cb_copy);
            } else {
                // DI ещё не выставлен (старт до ControlApi) — фолбэк на
                // ранее известный offset как Manual, без поллера.
                next = std::make_unique<ManualOffsetSource>(
                    cfg.ntp.last_offset_ms.value_or(0));
            }
            break;
    }
    std::unique_lock lk(mu_);
    source_ = std::move(next);
}

std::chrono::system_clock::time_point TimeSourceManager::now() const {
    std::shared_lock lk(mu_);
    return source_->now();
}

std::int64_t TimeSourceManager::offsetMs() const {
    std::shared_lock lk(mu_);
    return source_->offsetMs();
}

std::string TimeSourceManager::sourceName() const {
    std::shared_lock lk(mu_);
    return source_->sourceName();
}

}  // namespace liveqx::auth
