// fix33 A3 — SNTP client (RFC 4330).
//
// Один UDP-roundtrip. 48-byte payload:
//   [0]      LI(2) | VN(3, =4) | Mode(3, =3 client)
//   [1..3]   stratum, poll, precision (нам не важны)
//   [4..7]   root delay
//   [8..11]  root dispersion
//   [12..15] reference identifier
//   [16..23] reference timestamp (T0)
//   [24..31] originate timestamp  (T1) — клиент → сервер
//   [32..39] receive timestamp    (T2) — сервер
//   [40..47] transmit timestamp   (T3) — сервер
//
// Каждый timestamp — uint32 seconds since 1900-01-01 UTC + uint32 fraction
// (2^32 = 1 second). Конвертим: unix_sec = ntp_sec - 2208988800.
//
// offset_ms = ((T2-T1) + (T3-T4)) / 2
// rtt_ms    = (T4-T1) - (T3-T2)

#include "auth/SntpClient.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <string>

namespace liveqx::auth {

namespace {

// Между 1900-01-01 и 1970-01-01 — 70 лет, из них 17 високосных.
constexpr std::uint64_t kNtpUnixEpochDeltaSec = 2208988800ull;

// Конвертирует (ntp_sec, ntp_frac) → unix-milliseconds.
std::int64_t ntpToUnixMs(std::uint32_t ntp_sec, std::uint32_t ntp_frac) noexcept {
    const std::int64_t unix_sec = static_cast<std::int64_t>(ntp_sec) -
                                  static_cast<std::int64_t>(kNtpUnixEpochDeltaSec);
    // frac / 2^32 → доля секунды; * 1000 → ms.
    const std::int64_t ms_part =
        (static_cast<std::int64_t>(ntp_frac) * 1000ll) >> 32;
    return unix_sec * 1000ll + ms_part;
}

// Текущее system_clock как unix-ms (для T1/T4).
std::int64_t systemUnixMs() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

void setSocketTimeout(int fd, std::chrono::milliseconds timeout) noexcept {
    timeval tv{};
    tv.tv_sec  = static_cast<long>(timeout.count() / 1000);
    tv.tv_usec = static_cast<long>((timeout.count() % 1000) * 1000);
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

}  // namespace

std::optional<SntpResult> SntpClient::query(
    std::string_view host,
    int              port,
    std::chrono::milliseconds timeout) {

    if (host.empty() || port <= 0 || port > 65535) return std::nullopt;

    // DNS resolve (поддерживаем IPv4 + IPv6).
    addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;

    const std::string host_str(host);
    const std::string port_str = std::to_string(port);
    addrinfo* res = nullptr;
    if (::getaddrinfo(host_str.c_str(), port_str.c_str(), &hints, &res) != 0 ||
        !res) {
        return std::nullopt;
    }

    std::optional<SntpResult> result;

    for (addrinfo* it = res; it; it = it->ai_next) {
        const int fd = ::socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd < 0) continue;
        setSocketTimeout(fd, timeout);

        // Запрос: LI=0, VN=4, Mode=3 (client) → 0b00'100'011 = 0x23.
        std::array<std::uint8_t, 48> tx{};
        tx[0] = 0x23;

        // T1 — наш origin timestamp. Кладём в transmit поле (offset 40).
        // Server вернёт его как origin → T1 на ответе.
        const std::int64_t t1_ms = systemUnixMs();
        const std::uint64_t t1_ntp_sec =
            static_cast<std::uint64_t>(t1_ms / 1000) + kNtpUnixEpochDeltaSec;
        const std::uint64_t t1_ntp_frac =
            (static_cast<std::uint64_t>(t1_ms % 1000) << 32) / 1000;
        const std::uint32_t tx_sec_be  = htonl(static_cast<std::uint32_t>(t1_ntp_sec));
        const std::uint32_t tx_frac_be = htonl(static_cast<std::uint32_t>(t1_ntp_frac));
        std::memcpy(&tx[40], &tx_sec_be,  4);
        std::memcpy(&tx[44], &tx_frac_be, 4);

        const ssize_t sent = ::sendto(fd, tx.data(), tx.size(), 0,
                                      it->ai_addr, it->ai_addrlen);
        if (sent != static_cast<ssize_t>(tx.size())) {
            ::close(fd);
            continue;
        }

        std::array<std::uint8_t, 48> rx{};
        sockaddr_storage from{};
        socklen_t        from_len = sizeof(from);
        const ssize_t got = ::recvfrom(fd, rx.data(), rx.size(), 0,
                                       reinterpret_cast<sockaddr*>(&from), &from_len);
        const std::int64_t t4_ms = systemUnixMs();
        ::close(fd);

        if (got != static_cast<ssize_t>(rx.size())) continue;

        // Mode сервера должно быть 4 (server).
        const std::uint8_t mode = rx[0] & 0x07;
        if (mode != 4) continue;

        // T2 = receive timestamp (offset 32).
        std::uint32_t t2_sec_be, t2_frac_be, t3_sec_be, t3_frac_be;
        std::memcpy(&t2_sec_be,  &rx[32], 4);
        std::memcpy(&t2_frac_be, &rx[36], 4);
        std::memcpy(&t3_sec_be,  &rx[40], 4);
        std::memcpy(&t3_frac_be, &rx[44], 4);

        const std::int64_t t2_ms = ntpToUnixMs(ntohl(t2_sec_be), ntohl(t2_frac_be));
        const std::int64_t t3_ms = ntpToUnixMs(ntohl(t3_sec_be), ntohl(t3_frac_be));

        // Нулевой transmit timestamp — KoD или мусор.
        if (t3_ms <= 0 || t2_ms <= 0) continue;

        SntpResult r{};
        r.offset_ms      = ((t2_ms - t1_ms) + (t3_ms - t4_ms)) / 2;
        r.round_trip_ms  = (t4_ms - t1_ms) - (t3_ms - t2_ms);
        r.server_unix_ms = t3_ms;
        result = r;
        break;
    }

    ::freeaddrinfo(res);
    return result;
}

}  // namespace liveqx::auth
