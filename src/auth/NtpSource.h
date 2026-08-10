#pragma once

// fix33 A3 — NtpSource: ITimeSource c фоновым SNTP-поллером.
//
// Owner'ы: TimeSourceManager создаёт NtpSource при reconfigure(cfg, cb)
// с cfg.source == Ntp. Конструктор сразу делает один синхронный sync_now()
// чтобы offset был валиден к моменту первого now() — далее запускает thread
// и поллит каждые poll_interval_s.
//
// Перебор серверов: на каждом poll-tick'е идём по cfg.ntp.servers по порядку,
// первый успешный ответ → atomic-store offset_ms + callback. Если все
// failed → keep previous offset_ms, callback не зовётся.
//
// onSyncResult(offset_ms, sync_at_unix_sec) — callback для persist'а в
// system_time_config (через TimeConfigRepo::updateNtpSyncResult). Owner
// решает, persist'ить ли — у нас тут нет БД-доступа.
//
// Stop: dtor вызывает request_stop() и join'ит thread. cv_.notify_all()
// будит спящего поллера сразу. Безопасно создавать/уничтожать в любом
// порядке (RAII).

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "auth/SntpClient.h"
#include "auth/TimeConfig.h"
#include "auth/TimeSource.h"

namespace liveqx::auth {

class NtpSource final : public ITimeSource {
public:
    using SyncCallback = std::function<void(std::int64_t offset_ms,
                                            std::int64_t sync_at_unix_sec)>;

    // sntp - DI-точка для тестов (FakeSntpClient). В production — SntpClient.
    NtpSource(NtpSettings                     settings,
              std::shared_ptr<ISntpClient>    sntp,
              SyncCallback                    on_sync = {});

    ~NtpSource() override;

    NtpSource(const NtpSource&)            = delete;
    NtpSource& operator=(const NtpSource&) = delete;

    std::chrono::system_clock::time_point now() const override;
    std::int64_t offsetMs() const override;
    std::string  sourceName() const override { return "ntp"; }

    // Принудительный sync (один проход по серверам). Возвращает true если
    // хотя бы один сервер ответил. Используется в /api/system/time/test
    // и в конструкторе для warm-up.
    bool syncNow();

private:
    void pollLoop(std::stop_token st);

    NtpSettings                     settings_;
    std::shared_ptr<ISntpClient>    sntp_;
    SyncCallback                    on_sync_;

    std::atomic<std::int64_t>       offset_ms_{0};

    std::mutex                      cv_mu_;
    std::condition_variable_any     cv_;
    std::jthread                    worker_;
};

}  // namespace liveqx::auth
