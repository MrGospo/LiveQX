#include "auth/SmtpConfigRepo.h"

#include <chrono>

#include "utils/Log.h"

namespace liveqx::auth {

namespace {

std::int64_t nowUnixSec() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

}  // namespace

SmtpConfigRepo::SmtpConfigRepo(AuthDb& db, MasterKey& master_key)
    : db_(db), master_key_(master_key) {}

std::string SmtpConfigRepo::securityToString(SmtpSecurity s) noexcept {
    switch (s) {
        case SmtpSecurity::None:     return "none";
        case SmtpSecurity::StartTls: return "starttls";
        case SmtpSecurity::Tls:      return "tls";
    }
    return "starttls";
}

std::optional<SmtpSecurity>
SmtpConfigRepo::securityFromString(std::string_view s) noexcept {
    if (s == "none")     return SmtpSecurity::None;
    if (s == "starttls") return SmtpSecurity::StartTls;
    if (s == "tls")      return SmtpSecurity::Tls;
    return std::nullopt;
}

std::optional<SmtpConfig> SmtpConfigRepo::load() {
    auto row = db_.readSmtpConfigRow();
    if (!row.has_value()) return std::nullopt;

    SmtpConfig cfg;
    cfg.enabled     = row->enabled;
    cfg.server      = row->server;
    cfg.port        = static_cast<std::uint16_t>(
                        row->port > 0 && row->port <= 65535 ? row->port : 587);
    cfg.username    = row->username;
    cfg.from_email  = row->from_email;
    cfg.from_name   = row->from_name;
    cfg.timeout_sec = row->timeout_sec > 0 ? row->timeout_sec : 15;
    if (auto m = securityFromString(row->security); m.has_value()) {
        cfg.security = *m;
    }

    if (!row->password_encrypted.empty()) {
        if (!master_key_.loaded()) {
            LOG_WARN("SmtpConfigRepo: master key not loaded — "
                     "password left empty");
        } else {
            auto pw = master_key_.decrypt(row->password_encrypted);
            if (pw.has_value()) {
                cfg.password = *pw;
            } else {
                LOG_ERROR("SmtpConfigRepo: cannot decrypt password "
                          "(MAC failed or wrong key)");
            }
        }
    }
    return cfg;
}

bool SmtpConfigRepo::save(const SmtpConfig& cfg,
                          std::optional<std::int64_t> updated_by_user_id) {
    AuthDb::SmtpConfigRow row;
    row.enabled     = cfg.enabled;
    row.server      = cfg.server;
    row.port        = cfg.port == 0 ? 587 : cfg.port;
    row.security    = securityToString(cfg.security);
    row.username    = cfg.username;
    row.from_email  = cfg.from_email;
    row.from_name   = cfg.from_name;
    row.timeout_sec = cfg.timeout_sec > 0 ? cfg.timeout_sec : 15;
    row.updated_at  = nowUnixSec();
    row.updated_by  = updated_by_user_id;

    if (!cfg.password.empty()) {
        if (!master_key_.loaded()) {
            LOG_ERROR("SmtpConfigRepo::save: master key not loaded — "
                      "refusing to store password as plaintext");
            return false;
        }
        row.password_encrypted = master_key_.encrypt(cfg.password);
        if (row.password_encrypted.empty()) {
            LOG_ERROR("SmtpConfigRepo::save: encrypt of password "
                      "returned empty blob");
            return false;
        }
    } else {
        row.password_encrypted.clear();
    }

    if (!db_.writeSmtpConfigRow(row)) {
        LOG_ERROR("SmtpConfigRepo::save: writeSmtpConfigRow failed");
        return false;
    }
    return true;
}

}  // namespace liveqx::auth
