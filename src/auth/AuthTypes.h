#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace liveqx::auth {

enum class Role {
    Viewer,
    Operator,
    Admin,
};

const char* roleName(Role r) noexcept;
std::optional<Role> roleFromString(std::string_view s) noexcept;

enum class Source {
    Local,
    Ldap,
};

const char* sourceName(Source s) noexcept;
std::optional<Source> sourceFromString(std::string_view s) noexcept;

enum class ChannelPermission {
    View,
    Operate,
};

const char* channelPermissionName(ChannelPermission p) noexcept;
std::optional<ChannelPermission> channelPermissionFromString(std::string_view s) noexcept;

struct User {
    std::int64_t id{0};
    std::string  username;
    std::string  email;
    std::string  password_hash;
    Source       source{Source::Local};
    Role         role{Role::Viewer};
    bool         must_change_password{false};
    std::optional<std::int64_t> initial_password_expires_at;
    std::optional<std::int64_t> password_changed_at;
    std::optional<std::int64_t> last_login_at;
    std::string  last_login_ip;
    bool         disabled{false};
    std::int64_t created_at{0};
    std::optional<std::int64_t> created_by;
    // commit 13/24 — счётчик подряд идущих неудачных логинов и абсолютный
    // unix-ts, до которого учётная запись заблокирована. Сбрасываются на
    // успешном логине и на admin-unlock'е. Локап применяется только к
    // local-учёткам; LDAP-flow живёт по своим правилам директории.
    int          failed_login_count{0};
    std::optional<std::int64_t> locked_until;
    // commit 20/24 — кэш групп LDAP-юзера и timestamp последнего
    // успешного LDAP login'а. Используется refresh()'ом для пересчёта
    // channel_grants без обращения к директории (downtime read-only).
    // Local-юзеры эти поля не используют — лежит NULL/empty.
    std::string                  ldap_groups_json;
    std::optional<std::int64_t>  ldap_groups_cached_at;
};

struct Session {
    std::int64_t id{0};
    std::int64_t user_id{0};
    std::string  jwt_id;
    std::string  refresh_token_hash;
    std::string  ip;
    std::string  user_agent;
    std::int64_t created_at{0};
    std::int64_t expires_at{0};
    std::optional<std::int64_t> revoked_at;
    // schema v5 (fix32 B2): unix-sec последнего успешного access verify.
    // NULL для строк, созданных до миграции, и до первого touchSession().
    std::optional<std::int64_t> last_seen_at;
};

struct ChannelGrant {
    std::int64_t       channel_id{0};
    ChannelPermission  permission{ChannelPermission::View};
};

struct RequestContext {
    std::int64_t user_id{0};
    std::string  username;
    Role         role{Role::Viewer};
    bool         must_change_password{false};
    std::vector<ChannelGrant> channel_grants;
};

// Audit event (commit 12/24).
//
// Хранится в auth_audit. event — текстовый код, не enum: операторы пишут
// фильтры в SIEM по строкам, и любой новый event-тип (например, ldap.bind)
// должен появляться без перекомпиляции callers/REST API. Канонические
// имена перечислены в AuthService::audit*; держим их рядом с тем, кто
// их эмитит, а не в централизованном enum.
//
// details_json — произвольный JSON-string (опционально). Туда лежат
// контекстные поля, не вынесенные в первоклассные колонки: например
// "reason":"bad_password", "old_role":"viewer","new_role":"operator".
struct AuditEvent {
    std::int64_t                  id{0};
    std::int64_t                  ts{0};            // unix-sec
    std::string                   event;            // e.g. "login.ok"
    std::optional<std::int64_t>   user_id;          // nullable
    std::string                   username;         // empty if unknown
    std::string                   ip;               // remote ip
    std::string                   details_json;     // raw JSON string
};

// Filter for AuthDb::listAuditEvents.
struct AuditFilter {
    std::optional<std::int64_t>   from_ts;          // ts >= from_ts
    std::optional<std::int64_t>   to_ts;            // ts <  to_ts (exclusive)
    std::optional<std::int64_t>   user_id;
    std::string                   username;         // exact match if non-empty
    std::string                   event;            // exact match if non-empty
    int                           limit{100};       // ceiling 1000
    int                           offset{0};
};

}  // namespace liveqx::auth
