#pragma once

// fix22 commit 23/24 — SMTP client wrapper поверх libcurl.
//
// Что мы делаем:
//   send(to, subject, body):
//     1. Init easy handle (curl_easy_init).
//     2. Build smtp[s]://server:port URI, set TLS options по security.
//     3. CURLOPT_USERNAME / CURLOPT_PASSWORD (если username непустой).
//     4. MAIL FROM, RCPT TO.
//     5. CURLOPT_READFUNCTION → multipart-style payload (RFC 5322
//        headers + body), text/plain UTF-8.
//     6. perform → status string.
//
// Что мы НЕ делаем:
//   - расшифровку password — caller передаёт plaintext (SmtpConfigRepo
//     расшифрует BLOB master key'ом).
//   - очереди / retry на failure — это тонкость notifier-логики
//     поверх клиента (commit 24 wiring). SmtpClient — голый transport.
//   - HTML-шаблоны / multipart — body отправляется как text/plain
//     UTF-8. Templating живёт уровнем выше.
//
// Threading: stateless после ctor'а; send() из разных потоков создаёт
// независимые curl handles.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "auth/SmtpConfig.h"

namespace liveqx::auth {

class SmtpClient {
public:
    explicit SmtpClient(SmtpConfig cfg);

    struct SendResult {
        bool         ok{false};
        std::string  error;            // empty if ok==true
        std::int64_t latency_ms{0};
    };

    // Шлёт одно письмо одному получателю. Multi-recipient на ходу
    // легко добавить (curl_slist_append rcpt_list), но единичный
    // recipient покрывает 100% наших use-case'ов (notifier шлёт
    // персонально каждому юзеру).
    SendResult send(std::string_view to,
                    std::string_view subject,
                    std::string_view body);

    // Probe: TCP-connect + EHLO + (если security != None) TLS handshake +
    // (если username непустой) AUTH. БЕЗ MAIL FROM/RCPT/DATA. Аналог
    // LdapClient::ping() для UI «check connection».
    struct PingResult {
        bool         ok{false};
        std::string  error;
        std::int64_t latency_ms{0};
    };
    PingResult ping();

    // ── helpers, exposed для тестов ────────────────────────────────────

    // Строит smtp[s]://host:port из cfg. SMTPS → smtps:// + implicit TLS;
    // Plain/StartTls → smtp://. Если cfg.port==0 — подставляем default
    // (25 plain, 587 starttls, 465 smtps).
    static std::string buildUri(const SmtpConfig& cfg);

    // RFC 5322 §3.6.2 — From: header в виде «Name <email>» либо «email».
    // Имя экранируется double-quote'ом если содержит спец-символы.
    static std::string formatFromHeader(const SmtpConfig& cfg);

    // RFC 5321 §4.1.2 — простой sanity-check: одно «@», без CR/LF, не
    // пустой. Не RFC-валидатор, а шлюз против инъекций в SMTP-команду.
    static bool isPlausibleEmail(std::string_view s);

    // Sanity-check всего конфига: server непустой, from_email plausible,
    // username/password — оба либо пустые, либо оба заданы. Возвращает
    // empty string если ok, иначе текст ошибки.
    static std::string validate(const SmtpConfig& cfg);

    // Собирает полное тело письма (RFC 5322 headers + blank line + body)
    // из to/subject/body. Возвращает строку, готовую к подаче libcurl'у
    // через CURLOPT_READFUNCTION.
    static std::string buildPayload(const SmtpConfig& cfg,
                                    std::string_view  to,
                                    std::string_view  subject,
                                    std::string_view  body);

    const SmtpConfig& config() const noexcept { return cfg_; }

private:
    SmtpConfig cfg_;
};

}  // namespace liveqx::auth
