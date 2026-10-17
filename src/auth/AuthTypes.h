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

// A right on the screens of an object or of one group (B13). An administrator
// gives it; a right on an object covers all of its groups, present and future.
// `can_command_group` / `can_command_object` widen what an operator may command
// from one screen to a whole group / object; `can_move` lets screens be moved
// between groups; `can_view_audit` opens the trail of that part of the tree.
struct PlayerScopeGrant {
    enum class Scope { Object, Group };
    Scope        scope{Scope::Group};
    std::int64_t scope_id{0};
    // For a group right: the object the group belongs to. Filled by whoever
    // loads the grant (it is not stored with the right).
    std::int64_t object_id{0};
    bool         can_command_group{false};
    bool         can_command_object{false};    // meaningful on an object only
    bool         can_move{false};
    bool         can_view_audit{false};
};

struct RequestContext {
    std::int64_t user_id{0};
    std::string  username;
    Role         role{Role::Viewer};
    bool         must_change_password{false};
    std::vector<ChannelGrant> channel_grants;
    // Rights on players by object and group. Not filled by the RBAC middleware:
    // they are read fresh by whoever needs them (they are not in the token).
    std::vector<PlayerScopeGrant> player_grants;
};

}  // namespace liveqx::auth
