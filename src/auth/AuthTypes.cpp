#include "auth/AuthTypes.h"

namespace liveqx::auth {

const char* roleName(Role r) noexcept {
    switch (r) {
        case Role::Viewer:   return "viewer";
        case Role::Operator: return "operator";
        case Role::Admin:    return "admin";
    }
    return "viewer";
}

std::optional<Role> roleFromString(std::string_view s) noexcept {
    if (s == "viewer")   return Role::Viewer;
    if (s == "operator") return Role::Operator;
    if (s == "admin")    return Role::Admin;
    return std::nullopt;
}

const char* sourceName(Source s) noexcept {
    switch (s) {
        case Source::Local: return "local";
        case Source::Ldap:  return "ldap";
    }
    return "local";
}

std::optional<Source> sourceFromString(std::string_view s) noexcept {
    if (s == "local") return Source::Local;
    if (s == "ldap")  return Source::Ldap;
    return std::nullopt;
}

const char* channelPermissionName(ChannelPermission p) noexcept {
    switch (p) {
        case ChannelPermission::View:    return "view";
        case ChannelPermission::Operate: return "operate";
    }
    return "view";
}

std::optional<ChannelPermission> channelPermissionFromString(std::string_view s) noexcept {
    if (s == "view")    return ChannelPermission::View;
    if (s == "operate") return ChannelPermission::Operate;
    return std::nullopt;
}

}  // namespace liveqx::auth
