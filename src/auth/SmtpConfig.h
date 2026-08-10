#pragma once

// fix22 commit 23/24 — POD-конфиг для SMTP-нотификаций.
//
// Это plaintext-форма того, что лежит в auth.db как smtp_config:
// SmtpConfigRepo читает row, расшифровывает password_encrypted master
// key'ом и передаёт в SmtpClient уже plaintext. SmtpClient про
// шифрование не знает — это слой над libcurl SMTP.

#include <cstdint>
#include <string>

namespace liveqx::auth {

enum class SmtpSecurity {
    None,      // plain SMTP (port 25, тестовый/intranet only).
    StartTls,  // STARTTLS upgrade на тех же 587/25.
    Tls,       // SMTPS (implicit TLS) — 465.
};

struct SmtpConfig {
    bool          enabled{false};

    std::string   server;          // host (без схемы, без порта).
    std::uint16_t port{587};
    SmtpSecurity  security{SmtpSecurity::StartTls};

    std::string   username;        // auth login (часто == from_email).
    std::string   password;        // plaintext в этом слое.

    std::string   from_email;      // MAIL FROM + From: header.
    std::string   from_name;       // optional, для From: "<name> <email>".

    // Сетевой timeout (sec) на установку соединения + отправку.
    int           timeout_sec{15};
};

}  // namespace liveqx::auth
