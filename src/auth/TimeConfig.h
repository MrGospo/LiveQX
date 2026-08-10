#pragma once

// fix33 — серверный Time/NTP/TZ конфиг.
//
// POD-форма system_time_config singleton-row в auth.db. Хранит:
//   - source       — какой clock-источник effective (system/ntp/manual);
//   - server_timezone — IANA-имя для отображения и наследования
//                       per-channel расписаниями;
//   - ntp.*        — список SNTP-серверов, poll-интервал, последние
//                    sync-результаты (read-only мониторинг, ОС-время
//                    liveqx не меняет);
//   - manual.*     — фиксированный offset_ms от system clock, который
//                    применяется как `effective_now = system_now + offset_ms`
//                    при source=Manual.
//
// TZ остаётся общей для всех source — это представление, а не источник.
// Per-channel TZ может переопределить server_timezone через channel_timezone
// (null в JSON → inherit, явная строка → override).

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace liveqx::auth {

enum class TimeSource {
    SystemLocal,  // effective = system_now (passthrough, default).
    Ntp,          // effective = system_now + ntp.last_offset_ms.
    Manual,       // effective = system_now + manual.offset_ms.
};

struct NtpSettings {
    bool                          enabled{false};
    std::vector<std::string>      servers;            // "host" или "host:port"
    int                           poll_interval_s{60};
    std::optional<std::int64_t>   last_offset_ms;
    std::optional<std::int64_t>   last_sync_at;       // unix-sec
};

struct ManualSettings {
    std::int64_t                  offset_ms{0};       // фиксируется на save()
    std::optional<std::int64_t>   set_at;             // unix-sec
};

struct TimeConfig {
    TimeSource      source{TimeSource::SystemLocal};
    std::string     server_timezone{"UTC"};           // IANA
    NtpSettings     ntp;
    ManualSettings  manual;
};

// String-конверсии для persistence + REST.
std::string             timeSourceToString(TimeSource s) noexcept;
std::optional<TimeSource> timeSourceFromString(std::string_view s) noexcept;

// IANA-валидатор через date::locate_zone (Howard Hinnant tzdb, чтобы
// работало на libstdc++ 12). Возвращает true если зона найдена в tzdata.
// Кэшировать на caller'е не нужно — locate_zone сам делает lookup по
// предзагруженной БД.
bool isValidIanaTimezone(std::string_view name) noexcept;

}  // namespace liveqx::auth
