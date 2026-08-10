#pragma once

// fix33 A2 — логический clock liveqx.
//
// ITimeSource — единая точка получения effective wall-clock'а для всех
// consumer'ов (Scheduler, playback log, audit). Стандартный
// std::chrono::system_clock::now() используется только внутри реализаций,
// никогда — напрямую в бизнес-логике (это позволяет менять источник
// без touch'ей по всему коду).
//
// Реализации:
//   - SystemLocalSource: passthrough system_clock (offset=0).
//   - ManualOffsetSource: фиксированный offset_ms от system_clock,
//     выставляется на save() из TimeConfig.manual.offset_ms.
//   - NtpSource (A3): держит offset_ms из фонового SNTP-поллера,
//     atomically обновляет на каждом успешном sync'е.
//
// TimeSourceManager — runtime-владелец активной реализации; REST PUT
// /api/system/time дёргает reconfigure(), который пересоздаёт source
// под новый TimeConfig.source. Manager thread-safe для now().

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>

#include "auth/TimeConfig.h"

namespace liveqx::auth {

class ISntpClient;  // fwd — для DI в NtpSource через TimeSourceManager.

class ITimeSource {
public:
    virtual ~ITimeSource() = default;

    // Effective wall-clock — с учётом offset'а источника.
    virtual std::chrono::system_clock::time_point now() const = 0;

    // Текущий offset от system_clock'а в миллисекундах.
    // 0 для system_local, ±N для manual/ntp.
    virtual std::int64_t offsetMs() const = 0;

    // Имя источника для отображения (совпадает с timeSourceToString).
    virtual std::string sourceName() const = 0;

    // Удобный helper для callers, которым нужен unix-ms напрямую.
    std::int64_t effectiveUnixMs() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            now().time_since_epoch()).count();
    }
};

class SystemLocalSource final : public ITimeSource {
public:
    std::chrono::system_clock::time_point now() const override {
        return std::chrono::system_clock::now();
    }
    std::int64_t offsetMs() const override { return 0; }
    std::string  sourceName() const override { return "system_local"; }
};

class ManualOffsetSource final : public ITimeSource {
public:
    explicit ManualOffsetSource(std::int64_t offset_ms) noexcept
        : offset_ms_(offset_ms) {}

    std::chrono::system_clock::time_point now() const override {
        return std::chrono::system_clock::now() +
               std::chrono::milliseconds(offset_ms_.load(
                   std::memory_order_relaxed));
    }
    std::int64_t offsetMs() const override {
        return offset_ms_.load(std::memory_order_relaxed);
    }
    std::string sourceName() const override { return "manual"; }

    void setOffsetMs(std::int64_t v) noexcept {
        offset_ms_.store(v, std::memory_order_relaxed);
    }

private:
    std::atomic<std::int64_t> offset_ms_;
};

// Runtime-владелец активного источника. now() lock-free через
// shared_mutex в read-режиме; reconfigure() — exclusive lock на момент
// swap'а unique_ptr'ов. Допустимо вызывать reconfigure() в любой момент
// — consumer'ы получат новый source со следующего now().
class TimeSourceManager {
public:
    using NtpSyncCallback = std::function<void(std::int64_t offset_ms,
                                               std::int64_t sync_at_unix_sec)>;

    TimeSourceManager();  // default: SystemLocalSource.

    // Зависимости для построения NtpSource при cfg.source == Ntp:
    //   - sntp:    общий SntpClient (или fake в тестах);
    //   - on_sync: persist-callback в TimeConfigRepo::updateNtpSyncResult.
    // Эти setter'ы вызываются один раз на старте (DI из ControlApi).
    // reconfigure() без выставленных deps на Ntp фолбэкает в ManualOffsetSource(0).
    void setNtpDependencies(std::shared_ptr<ISntpClient> sntp,
                            NtpSyncCallback              on_sync);

    // Создаёт новый source по cfg.source и применяет offset.
    void reconfigure(const TimeConfig& cfg);

    std::chrono::system_clock::time_point now() const;
    std::int64_t offsetMs() const;
    std::string  sourceName() const;

private:
    mutable std::shared_mutex          mu_;
    std::unique_ptr<ITimeSource>       source_;
    std::shared_ptr<ISntpClient>       sntp_;
    NtpSyncCallback                    on_sync_;
};

}  // namespace liveqx::auth
