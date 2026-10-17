#pragma once
//
// Who may see and command which player (B13).
//
// Pure logic over a role and the rights an administrator has given the person:
// no database, no HTTP, so the whole matrix is unit-tested. Whoever calls it
// loads the rights fresh on every request (rights are NOT carried in the access
// token, so taking one away works at once) and tells it where each player sits.
//
// The rules, in short:
//   * an administrator may do everything, everywhere;
//   * anyone else needs a right on the player's group or on its object, and
//     without one sees nothing (fail closed) — this includes viewers;
//   * the role decides what a person may do to ONE screen (viewer: look,
//     operator: also command); the marks on a right widen it to a group / an
//     object, to moving screens, to audit;
//   * commanding "everyone" is for administrators only;
//   * moving a screen needs the move mark on both groups and never crosses
//     objects without an administrator;
//   * a player without a (fully known) place is for administrators only.

#include "auth/AuthTypes.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace liveqx::players {

// Where a player sits. Both ids must be known for a non-administrator to be
// allowed anything.
struct Place {
    std::optional<std::int64_t> group_id;
    std::optional<std::int64_t> object_id;
    bool known() const { return group_id.has_value() && object_id.has_value(); }
};

class PlayerAccess {
public:
    PlayerAccess(auth::Role role, std::vector<auth::PlayerScopeGrant> grants);
    // Setups without authentication (development, tests): everything allowed.
    static PlayerAccess unrestricted();

    bool isAdmin() const { return role_ == auth::Role::Admin; }
    bool hasAnyGrant() const { return !grants_.empty(); }

    bool canView(const Place& p) const;
    // The same decision without building an object (SSE filters every event).
    static bool canViewWith(auth::Role role, const std::vector<auth::PlayerScopeGrant>& grants,
                            const Place& p) noexcept;
    bool canCommandPlayer(const Place& p) const;        // one screen
    bool canCommandGroup(const Place& p) const;         // every screen of the player's group
    bool canCommandObject(std::int64_t object_id) const;
    bool canCommandAll() const { return isAdmin(); }
    bool canMove(const Place& from, const Place& to) const;
    bool canViewAudit(const Place& p) const;

    // For the tree: a group is shown when it is covered; an object is "seen"
    // by an object right, and merely "contained" when only one of its groups
    // is granted (its name is shown as the container, its other groups are not).
    bool canSeeGroup(std::int64_t group_id, std::int64_t object_id) const;
    bool canSeeObject(std::int64_t object_id) const;
    bool seesObjectContaining(std::int64_t object_id) const;

private:
    const auth::PlayerScopeGrant* groupGrant(std::int64_t group_id) const;
    const auth::PlayerScopeGrant* objectGrant(std::int64_t object_id) const;
    bool operatorOrAbove() const { return role_ == auth::Role::Operator || role_ == auth::Role::Admin; }

    auth::Role                          role_;
    std::vector<auth::PlayerScopeGrant> grants_;
};

}  // namespace liveqx::players
