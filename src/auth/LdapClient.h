#pragma once

// fix22 commit 16/24 — LDAP client wrapper поверх libldap (OpenLDAP).
//
// Что мы делаем:
//   authenticate(username, password):
//     1. Init handle (ldap_initialize) с URL'ом из config.server + tls_mode.
//     2. set_option: PROTO_VERSION=3, NETWORK_TIMEOUT, REFERRALS=0.
//     3. Если StartTLS — ldap_start_tls_s.
//     4. Bind как service-account (config.bind_dn / bind_password).
//     5. Search: base = config.base_dn, scope = SUBTREE,
//        filter = config.user_filter с '%s' → escapeFilter(username).
//     6. Из результата читаем DN юзера + email + group_attribute.
//     7. Re-bind handle'а как user_dn + переданный password — это
//        собственно password-verify.
//     8. Unbind, возвращаем AuthResult.
//
//   ping():
//     Init + service bind + unbind. Используется для health-check'а
//     и admin /api/auth/ldap/test (commit 21).
//
// Что мы НЕ делаем здесь:
//   - расшифровку bind_password — caller передаёт plaintext (ConfigRepo
//     в commit 17 расшифрует BLOB master key'ём).
//   - mapping group → role — это в LdapDirectoryService поверх клиента
//     (commit 18).
//   - кэш downtime'а — в commit 20.
//
// Threading: класс stateless после ctor'а; authenticate() и ping() могут
// идти из разных потоков. libldap handle создаётся локально внутри вызова
// и не разделяется между ними (cheap relative to network RTT).

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "auth/LdapConfig.h"

namespace liveqx::auth {

class LdapClient {
public:
    explicit LdapClient(LdapConfig cfg);

    struct AuthResult {
        bool                       ok{false};
        std::string                user_dn;
        std::string                email;
        std::vector<std::string>   groups;        // raw DN-строки из group_attribute
        std::string                error;         // empty if ok==true
        // Discriminator для AuthService (commit 18) чтобы отличать
        // «связь упала» от «пароль неверен» — ATB lockout-policy не
        // должна срабатывать на сетевых сбоях.
        enum class Reason {
            Ok,
            ConnectionFailed,
            BindServiceFailed,    // service-account creds битые
            UserNotFound,
            InvalidCredentials,   // user-bind не прошёл
            ConfigError,          // что-то пустое в config
            Other,
        } reason{Reason::Other};
    };

    AuthResult authenticate(std::string_view username,
                            std::string_view password);

    struct PingResult {
        bool         ok{false};
        std::string  error;
        // Время полного round-trip'а (ms), включая TLS handshake. Удобно
        // показать оператору на /api/auth/ldap/test.
        std::int64_t latency_ms{0};
    };

    PingResult ping();

    // ── helpers, exposed для тестов ────────────────────────────────────

    // RFC 4515 §3 — escape для search filter values. Экранируем
    // \, *, (, ), NUL.
    static std::string escapeFilter(std::string_view raw);

    // RFC 4514 — escape для DN component'а. Экранируем
    // \, ",, +, ;, <, >, NUL и leading/trailing whitespace + #.
    // Используется в commit 18 для построения custom DN при
    // bind-as-DN режиме.
    static std::string escapeDn(std::string_view raw);

    // Строит ldap[s]://host[:port] URI из cfg. Если порт явно задан —
    // не подставляем default; иначе 389 plain/starttls и 636 ldaps.
    static std::string buildUri(const LdapConfig& cfg);

    // Подставляет '%s' → escapeFilter(username) в template. Если '%s'
    // встречается несколько раз — заменяем все.
    static std::string renderUserFilter(std::string_view filter_template,
                                        std::string_view username);

    // Sanity-check'и для config'а: enabled+server+base_dn непустые,
    // user_filter содержит '%s'. Возвращает empty string если ok.
    static std::string validate(const LdapConfig& cfg);

    const LdapConfig& config() const noexcept { return cfg_; }

private:
    LdapConfig cfg_;
};

}  // namespace liveqx::auth
