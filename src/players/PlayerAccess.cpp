#include "players/PlayerAccess.h"

namespace liveqx::players {

using auth::PlayerScopeGrant;
using auth::Role;

PlayerAccess::PlayerAccess(Role role, std::vector<PlayerScopeGrant> grants)
    : role_(role), grants_(std::move(grants)) {}

PlayerAccess PlayerAccess::unrestricted() { return PlayerAccess(Role::Admin, {}); }

const PlayerScopeGrant* PlayerAccess::groupGrant(std::int64_t group_id) const {
    for (const auto& g : grants_)
        if (g.scope == PlayerScopeGrant::Scope::Group && g.scope_id == group_id) return &g;
    return nullptr;
}

const PlayerScopeGrant* PlayerAccess::objectGrant(std::int64_t object_id) const {
    for (const auto& g : grants_)
        if (g.scope == PlayerScopeGrant::Scope::Object && g.scope_id == object_id) return &g;
    return nullptr;
}

bool PlayerAccess::canView(const Place& p) const { return canViewWith(role_, grants_, p); }

bool PlayerAccess::canViewWith(Role role, const std::vector<PlayerScopeGrant>& grants, const Place& p) noexcept {
    if (role == Role::Admin) return true;
    if (!p.known()) return false;
    for (const auto& g : grants) {
        if (g.scope == PlayerScopeGrant::Scope::Group && g.scope_id == *p.group_id) return true;
        if (g.scope == PlayerScopeGrant::Scope::Object && g.scope_id == *p.object_id) return true;
    }
    return false;
}

bool PlayerAccess::canCommandPlayer(const Place& p) const {
    if (isAdmin()) return true;
    return operatorOrAbove() && canView(p);
}

bool PlayerAccess::canCommandGroup(const Place& p) const {
    if (isAdmin()) return true;
    if (!operatorOrAbove() || !p.known()) return false;
    if (auto* g = groupGrant(*p.group_id); g && g->can_command_group) return true;
    if (auto* o = objectGrant(*p.object_id); o && (o->can_command_group || o->can_command_object)) return true;
    return false;
}

bool PlayerAccess::canCommandObject(std::int64_t object_id) const {
    if (isAdmin()) return true;
    if (!operatorOrAbove()) return false;
    auto* o = objectGrant(object_id);
    return o && o->can_command_object;
}

bool PlayerAccess::canMove(const Place& from, const Place& to) const {
    if (isAdmin()) return true;
    if (!operatorOrAbove() || !from.known() || !to.known()) return false;
    if (*from.object_id != *to.object_id) return false;          // between objects: an administrator
    auto holds = [&](const Place& p) {
        if (auto* g = groupGrant(*p.group_id); g && g->can_move) return true;
        if (auto* o = objectGrant(*p.object_id); o && o->can_move) return true;
        return false;
    };
    return holds(from) && holds(to);
}

bool PlayerAccess::canViewAudit(const Place& p) const {
    if (isAdmin()) return true;
    if (!p.known()) return false;
    if (auto* g = groupGrant(*p.group_id); g && g->can_view_audit) return true;
    if (auto* o = objectGrant(*p.object_id); o && o->can_view_audit) return true;
    return false;
}

bool PlayerAccess::canSeeGroup(std::int64_t group_id, std::int64_t object_id) const {
    if (isAdmin()) return true;
    return groupGrant(group_id) != nullptr || objectGrant(object_id) != nullptr;
}

bool PlayerAccess::canSeeObject(std::int64_t object_id) const {
    return isAdmin() || objectGrant(object_id) != nullptr;
}

bool PlayerAccess::seesObjectContaining(std::int64_t object_id) const {
    if (canSeeObject(object_id)) return true;
    for (const auto& g : grants_)
        if (g.scope == PlayerScopeGrant::Scope::Group && g.object_id == object_id) return true;
    return false;
}

}  // namespace liveqx::players
