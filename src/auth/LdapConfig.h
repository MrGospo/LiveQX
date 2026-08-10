#pragma once

// fix22 commit 16/24 — POD-конфиг для LDAP клиента.
//
// Это «развёрнутая» (decrypted) форма того, что лежит в auth.db
// в ldap_config: ConfigRepo (commit 17) читает row, расшифровывает
// bind_password_encrypted master key'ём и передаёт в LdapClient
// уже plaintext. Сам LdapClient про шифрование не знает — это слой
// «голого» SDK поверх libldap.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "auth/AuthTypes.h"

namespace liveqx::auth {

enum class LdapTlsMode {
    Plain,     // ldap:// без шифрования (только за TLS-туннелем!)
    StartTls,  // ldap:// + StartTLS (recommended)
    Ldaps,     // ldaps:// (legacy SSL)
};

struct LdapConfig {
    bool         enabled{false};

    // host[:port] — порт необязателен (default 389 plain/starttls, 636 ldaps).
    std::string  server;
    LdapTlsMode  tls_mode{LdapTlsMode::StartTls};

    // DN-ветка для user_filter поиска. Пример: "ou=people,dc=corp,dc=example,dc=com".
    std::string  base_dn;

    // Service account для search-stage. Можно оставить пустым для
    // anonymous bind — тогда LdapClient попытается анонимно искать.
    std::string  bind_dn;
    std::string  bind_password;  // plaintext в этом слое; шифрование — выше.

    // Фильтр поиска юзера. Подстановка: '%s' → escapeFilter(username).
    // Примеры:
    //   AD: "(sAMAccountName=%s)"
    //   OpenLDAP: "(uid=%s)"
    std::string  user_filter{"(uid=%s)"};

    // Атрибут с группами юзера. memberOf — типично для AD; в OpenLDAP
    // нужен overlay 'memberof', иначе нужно делать reverse-search по
    // groupOfNames.member.
    std::string  group_attribute{"memberOf"};

    // Атрибут с email'ом. mail / userPrincipalName / proxyAddresses.
    std::string  email_attribute{"mail"};

    // Сетевой timeout всех LDAP-операций. Большие таймауты блокируют
    // login REST → плохой UX, но мелкие приведут к ложному сбою на
    // нагруженных DC.
    int          network_timeout_sec{5};

    // Mapping AD-группы (DN) → роль liveqx (commit 18 будет
    // использовать). Хранится как JSON в БД, в C++ — обычная map.
    // Ключ должен быть нормализован (lowercase) — LDAP считает DN
    // case-insensitive в attribute names и в значениях, поэтому
    // компоновать сравнение через нормализацию.
    std::map<std::string, Role> group_role_map;

    // Per-channel ACL: канал → {group_dn → permission}. Используется
    // в commit 19, здесь только структура для теста парсинга/копирования.
    struct ChannelAcl {
        std::int64_t                 channel_id{0};
        std::map<std::string,
                 ChannelPermission>  group_perms;
    };
    std::vector<ChannelAcl> channel_acl;

    // Если true — refresh JWT'а пере-резолвит группы (повторный bind).
    // false означает, что роль/grants заморожены на момент login'а до
    // следующего полного логина.
    bool         recheck_groups_on_refresh{true};
};

}  // namespace liveqx::auth
