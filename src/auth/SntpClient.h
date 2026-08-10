#pragma once

// fix33 A3 — SNTP (RFC 4330) клиент.
//
// Минимальный синхронный UDP/123 клиент: один 48-byte NTP-запрос, один
// 48-byte ответ. Без depend'ов поверх стандартной libc + posix sockets.
//
// Используется NtpSource'ом фоновым поллером — каждые poll_interval_s
// (5..3600s) перебираем cfg.ntp.servers до первого успешного результата
// и сохраняем offset_ms в atomic.
//
// Семантика offset: `effective_now = system_now + offset_ms`. Если
// server-клок впереди системы на 5 секунд → offset_ms = +5000.
//
// host может быть как DNS-именем ("pool.ntp.org"), так и литералом
// IPv4/IPv6 ("192.168.1.1", "[::1]"). Port-форма "host:port" парсится
// внутри NtpSource'а, до querySntp().
//
// Reasons for failure возвращаются как nullopt — логировать должен caller
// (как минимум NtpSource), у нас нет spdlog-context'а.

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace liveqx::auth {

struct SntpResult {
    std::int64_t  offset_ms;        // server_time - system_time
    std::int64_t  round_trip_ms;    // T4-T1 - (T3-T2)
    std::int64_t  server_unix_ms;   // T3 (transmit timestamp) в unix-ms
};

class ISntpClient {
public:
    virtual ~ISntpClient() = default;
    virtual std::optional<SntpResult> query(
        std::string_view host,
        int              port    = 123,
        std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) = 0;
};

class SntpClient : public ISntpClient {
public:
    std::optional<SntpResult> query(
        std::string_view host,
        int              port    = 123,
        std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) override;
};

}  // namespace liveqx::auth
