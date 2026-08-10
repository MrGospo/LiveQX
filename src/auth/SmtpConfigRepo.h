#pragma once

// fix22 commit 23/24 — UI/REST-обёртка вокруг smtp_config row.
//
// Что делает:
//   load(): читает row из AuthDb, расшифровывает password_encrypted
//           master key'ом. Возвращает SmtpConfig (POD).
//   save(): обратное направление — шифрует password (если non-empty),
//           UPSERT'ит row.
//
// Что НЕ делает: сетевую проверку (SmtpClient::ping). Кэш — каждый
// load() читает из БД заново.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "auth/AuthDb.h"
#include "auth/MasterKey.h"
#include "auth/SmtpConfig.h"

namespace liveqx::auth {

class SmtpConfigRepo {
public:
    SmtpConfigRepo(AuthDb& db, MasterKey& master_key);

    std::optional<SmtpConfig> load();
    bool save(const SmtpConfig& cfg,
              std::optional<std::int64_t> updated_by_user_id);

    // ── helpers, exposed для тестов ────────────────────────────────────
    static std::string securityToString(SmtpSecurity s) noexcept;
    static std::optional<SmtpSecurity>
        securityFromString(std::string_view s) noexcept;

private:
    AuthDb&     db_;
    MasterKey&  master_key_;
};

}  // namespace liveqx::auth
