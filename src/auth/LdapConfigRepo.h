#pragma once

// fix22 commit 17/24 — UI/REST-обёртка вокруг ldap_config row.
//
// Что делает:
//   load(): читает row из AuthDb, расшифровывает bind_password_encrypted
//           master key'ём, парсит group_role_map_json / channel_acl_json
//           в C++-структуры. Возвращает LdapConfig (POD из commit 16).
//   save(): обратное направление — JSON'ит maps, шифрует bind_password
//           (если non-empty), UPSERT'ит row, audit-event.
//
// Что НЕ делает:
//   - сетевую проверку (это LdapClient::ping в commit 21).
//   - кэш — каждый load() читает из БД и расшифровывает заново. На
//     production-сервере login flow не настолько частый, чтобы оптимизировать;
//     если станет узким местом — добавим кэш с invalidation на save().
//
// Threading: все методы ходят через AuthDb (он сам lock'ает),
// MasterKey thread-safe для encrypt/decrypt после load(). LdapConfigRepo
// stateless — можно создавать в разных потоках.

#include <cstdint>
#include <optional>
#include <string>

#include "auth/AuthDb.h"
#include "auth/LdapConfig.h"
#include "auth/MasterKey.h"

namespace liveqx::auth {

class LdapConfigRepo {
public:
    LdapConfigRepo(AuthDb& db, MasterKey& master_key);

    // Возвращает std::nullopt если row отсутствует.
    // Возвращает unexpected (через optional + LOG_WARN), если decrypt
    // упал — это серьёзная ошибка, не молчим, но и стартовать сервер
    // не запрещаем (commit 18 поймёт что ldap-источник недоступен).
    std::optional<LdapConfig> load();

    // Возвращает true на полный успех (encrypt + write + audit).
    bool save(const LdapConfig& cfg,
              std::optional<std::int64_t> updated_by_user_id);

    // ── Helpers, exposed для тестов ────────────────────────────────────

    // Строковая форма tls_mode (для БД-колонки).
    static std::string tlsModeToString(LdapTlsMode m) noexcept;
    static std::optional<LdapTlsMode> tlsModeFromString(std::string_view s) noexcept;

    // group_role_map ↔ JSON ({ "CN=admins,...": "admin", ... }).
    static std::string groupRoleMapToJson(const std::map<std::string, Role>& m);
    static std::map<std::string, Role> groupRoleMapFromJson(std::string_view s);

    // channel_acl ↔ JSON.
    // [
    //   {"channel_id": 1,
    //    "groups": {"CN=ops,...": "operate", "CN=ad,...": "view"}},
    //   ...
    // ]
    static std::string channelAclToJson(const std::vector<LdapConfig::ChannelAcl>& v);
    static std::vector<LdapConfig::ChannelAcl> channelAclFromJson(std::string_view s);

private:
    AuthDb&     db_;
    MasterKey&  master_key_;
};

}  // namespace liveqx::auth
