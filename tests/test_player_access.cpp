// players/PlayerAccess — who may see and command which player.
//
// The model (B13): all -> object -> group, a player sits in one group.
// Rights are given by an administrator on an object or on a group; a right on
// an object covers all of its groups. The role still decides what a person may
// do to a single screen (a viewer only looks, an operator also commands); the
// extra marks widen it to a whole group or object, to moving screens, to audit.
//
// What these tests protect (this is the access matrix, so it is spelled out):
//   * nothing is visible without a right (fail closed), for every role but admin;
//   * a group right covers that group only, an object right all its groups;
//   * a viewer never commands, whatever marks it carries;
//   * commanding a group, an object and "everyone" are three separate powers,
//     "everyone" belongs to administrators alone;
//   * a right on a whole object implies the right on each of its groups
//     (nobody can reboot the building but not the floor);
//   * moving a screen needs the move mark on BOTH groups, and never crosses
//     objects without an administrator;
//   * a player without a place is the administrator's business only.

#include "players/PlayerAccess.h"

#include <gtest/gtest.h>

using liveqx::auth::PlayerScopeGrant;
using liveqx::auth::Role;
using liveqx::players::Place;
using liveqx::players::PlayerAccess;
using Scope = PlayerScopeGrant::Scope;

namespace {

// O1 = {G11, G12}, O2 = {G21}
constexpr std::int64_t O1 = 1, O2 = 2, G11 = 11, G12 = 12, G21 = 21;
const Place P11{G11, O1};
const Place P12{G12, O1};
const Place P21{G21, O2};

PlayerScopeGrant group(std::int64_t id, bool cg = false, bool mv = false, bool au = false) {
    PlayerScopeGrant g;
    g.scope = Scope::Group;
    g.scope_id = id;
    g.object_id = id / 10;                  // G11, G12 -> O1; G21 -> O2 (the loader fills this from the hierarchy)
    g.can_command_group = cg;
    g.can_move = mv;
    g.can_view_audit = au;
    return g;
}
PlayerScopeGrant object(std::int64_t id, bool cg = false, bool co = false, bool mv = false, bool au = false) {
    PlayerScopeGrant g;
    g.scope = Scope::Object;
    g.scope_id = id;
    g.can_command_group = cg;
    g.can_command_object = co;
    g.can_move = mv;
    g.can_view_audit = au;
    return g;
}

}  // namespace

TEST(PlayerAccess, AnAdministratorMayDoEverythingEverywhere) {
    PlayerAccess a(Role::Admin, {});
    for (const Place& p : {P11, P12, P21}) {
        EXPECT_TRUE(a.canView(p));
        EXPECT_TRUE(a.canCommandPlayer(p));
        EXPECT_TRUE(a.canCommandGroup(p));
        EXPECT_TRUE(a.canViewAudit(p));
    }
    EXPECT_TRUE(a.canCommandObject(O1));
    EXPECT_TRUE(a.canCommandAll());
    EXPECT_TRUE(a.canMove(P11, P21)) << "only an administrator moves screens between objects";
    EXPECT_TRUE(a.canView(Place{}));
    EXPECT_TRUE(a.canSeeGroup(G21, O2));
    EXPECT_TRUE(a.canSeeObject(O2));
}

TEST(PlayerAccess, WithoutARightNothingIsVisibleOrAllowed) {
    for (Role role : {Role::Viewer, Role::Operator}) {
        PlayerAccess a(role, {});
        EXPECT_FALSE(a.canView(P11));
        EXPECT_FALSE(a.canCommandPlayer(P11));
        EXPECT_FALSE(a.canCommandGroup(P11));
        EXPECT_FALSE(a.canCommandObject(O1));
        EXPECT_FALSE(a.canViewAudit(P11));
        EXPECT_FALSE(a.canMove(P11, P12));
        EXPECT_FALSE(a.canSeeGroup(G11, O1));
        EXPECT_FALSE(a.canSeeObject(O1));
        EXPECT_FALSE(a.hasAnyGrant());
    }
}

TEST(PlayerAccess, AGroupRightCoversThatGroupOnly) {
    PlayerAccess a(Role::Operator, {group(G11)});
    EXPECT_TRUE(a.canView(P11));
    EXPECT_FALSE(a.canView(P12)) << "a sibling group in the same object is not covered";
    EXPECT_FALSE(a.canView(P21));
    EXPECT_TRUE(a.canCommandPlayer(P11)) << "an operator commands a single screen of a group it has";
    EXPECT_FALSE(a.canCommandPlayer(P12));
    EXPECT_FALSE(a.canCommandGroup(P11)) << "the group-wide power is a separate mark";
    EXPECT_FALSE(a.canCommandObject(O1));
    EXPECT_FALSE(a.canMove(P11, P12));
}

TEST(PlayerAccess, AnObjectRightCoversAllItsGroups) {
    PlayerAccess a(Role::Operator, {object(O1)});
    EXPECT_TRUE(a.canView(P11));
    EXPECT_TRUE(a.canView(P12));
    EXPECT_FALSE(a.canView(P21));
    EXPECT_TRUE(a.canCommandPlayer(P12));
    EXPECT_FALSE(a.canCommandGroup(P12));
    EXPECT_FALSE(a.canCommandObject(O1));
}

TEST(PlayerAccess, TheMarkToCommandGroupsWorksPerGroupOrAcrossAnObject) {
    PlayerAccess one(Role::Operator, {group(G11, /*cg=*/true)});
    EXPECT_TRUE(one.canCommandGroup(P11));
    EXPECT_FALSE(one.canCommandGroup(P12));

    PlayerAccess all(Role::Operator, {object(O1, /*cg=*/true)});
    EXPECT_TRUE(all.canCommandGroup(P11));
    EXPECT_TRUE(all.canCommandGroup(P12));
    EXPECT_FALSE(all.canCommandGroup(P21));
    EXPECT_FALSE(all.canCommandObject(O1)) << "restarting groups one by one is not restarting the building";
}

TEST(PlayerAccess, TheMarkToCommandAnObjectImpliesItsGroupsButNotTheReverse) {
    PlayerAccess a(Role::Operator, {object(O1, /*cg=*/false, /*co=*/true)});
    EXPECT_TRUE(a.canCommandObject(O1));
    EXPECT_FALSE(a.canCommandObject(O2));
    EXPECT_TRUE(a.canCommandGroup(P11)) << "whoever may reboot the whole object may reboot a floor of it";
    EXPECT_TRUE(a.canCommandGroup(P12));
    EXPECT_FALSE(a.canCommandGroup(P21));
}

TEST(PlayerAccess, ThePowerToCommandEveryoneIsAnAdministratorsAlone) {
    PlayerAccess a(Role::Operator, {object(O1, true, true, true, true), object(O2, true, true, true, true)});
    EXPECT_FALSE(a.canCommandAll()) << "even with every mark on every object";
}

TEST(PlayerAccess, AViewerLooksButNeverCommandsWhateverItCarries) {
    PlayerAccess a(Role::Viewer, {object(O1, true, true, true, true)});
    EXPECT_TRUE(a.canView(P11));
    EXPECT_FALSE(a.canCommandPlayer(P11));
    EXPECT_FALSE(a.canCommandGroup(P11));
    EXPECT_FALSE(a.canCommandObject(O1));
    EXPECT_FALSE(a.canMove(P11, P12));
    EXPECT_TRUE(a.canViewAudit(P11)) << "looking at the trail is not a command";
}

TEST(PlayerAccess, TheMoveButtonIsOfferedOnlyWhereTheMarkIsHeldAndNeverToAViewer) {
    PlayerAccess mover(Role::Operator, {group(G11, false, /*mv=*/true)});
    EXPECT_TRUE(mover.canMoveFrom(P11));
    EXPECT_FALSE(mover.canMoveFrom(P12)) << "no mark on that group";
    EXPECT_FALSE(mover.canMoveFrom(P21));
    PlayerAccess byObject(Role::Operator, {object(O1, false, false, /*mv=*/true)});
    EXPECT_TRUE(byObject.canMoveFrom(P11));
    EXPECT_TRUE(byObject.canMoveFrom(P12)) << "an object mark covers its groups";
    EXPECT_FALSE(byObject.canMoveFrom(P21));
    PlayerAccess viewer(Role::Viewer, {object(O1, true, true, true, true)});
    EXPECT_FALSE(viewer.canMoveFrom(P11));
    PlayerAccess admin(Role::Admin, {});
    EXPECT_TRUE(admin.canMoveFrom(P11));
    EXPECT_TRUE(admin.canMoveFrom(Place{}));
    EXPECT_FALSE(mover.canMoveFrom(Place{})) << "a screen with no known place is for administrators";
}

TEST(PlayerAccess, WhatTheButtonPromisesIsWhatTheMoveDecides) {
    // canMove() is canMoveFrom() on both ends inside one object: the two cannot drift apart.
    PlayerAccess a(Role::Operator, {group(G11, false, true), group(G12, false, true)});
    EXPECT_EQ(a.canMove(P11, P12), a.canMoveFrom(P11) && a.canMoveFrom(P12));
    PlayerAccess half(Role::Operator, {group(G11, false, true)});
    EXPECT_EQ(half.canMove(P11, P12), half.canMoveFrom(P11) && half.canMoveFrom(P12));
}

TEST(PlayerAccess, MovingNeedsTheMarkOnBothGroups) {
    PlayerAccess both(Role::Operator, {group(G11, false, /*mv=*/true), group(G12, false, /*mv=*/true)});
    EXPECT_TRUE(both.canMove(P11, P12));
    EXPECT_TRUE(both.canMove(P12, P11));

    PlayerAccess half(Role::Operator, {group(G11, false, true), group(G12)});
    EXPECT_FALSE(half.canMove(P11, P12)) << "the destination is not his to fill";
    EXPECT_FALSE(half.canMove(P12, P11)) << "the source is not his to empty";
}

TEST(PlayerAccess, AnObjectRightWithTheMoveMarkMovesScreensAmongItsGroups) {
    PlayerAccess a(Role::Operator, {object(O1, false, false, /*mv=*/true)});
    EXPECT_TRUE(a.canMove(P11, P12));
}

TEST(PlayerAccess, NobodyButAnAdministratorMovesAScreenBetweenObjects) {
    PlayerAccess a(Role::Operator, {object(O1, false, false, true), object(O2, false, false, true)});
    EXPECT_FALSE(a.canMove(P11, P21)) << "even holding the mark on both objects";
    EXPECT_TRUE(a.canMove(P11, P12));
}

TEST(PlayerAccess, AuditIsGivenPerGroupOrObjectAndNeedsItsOwnMark) {
    PlayerAccess none(Role::Operator, {group(G11), object(O2)});
    EXPECT_FALSE(none.canViewAudit(P11));
    EXPECT_FALSE(none.canViewAudit(P21));

    PlayerAccess g(Role::Operator, {group(G11, false, false, /*au=*/true)});
    EXPECT_TRUE(g.canViewAudit(P11));
    EXPECT_FALSE(g.canViewAudit(P12));

    PlayerAccess o(Role::Operator, {object(O1, false, false, false, /*au=*/true)});
    EXPECT_TRUE(o.canViewAudit(P11));
    EXPECT_TRUE(o.canViewAudit(P12));
    EXPECT_FALSE(o.canViewAudit(P21));
}

TEST(PlayerAccess, APlayerWithoutAPlaceIsForAdministratorsOnly) {
    const Place nowhere;
    PlayerAccess op(Role::Operator, {object(O1, true, true, true, true), group(G21, true, true, true)});
    EXPECT_FALSE(op.canView(nowhere));
    EXPECT_FALSE(op.canCommandPlayer(nowhere));
    EXPECT_FALSE(op.canCommandGroup(nowhere));
    EXPECT_FALSE(op.canMove(nowhere, P11)) << "nor can it be taken out of limbo by someone else";
    EXPECT_FALSE(op.canMove(P11, nowhere));

    const Place half{G11, std::nullopt};                // a group id but no object: not trustworthy
    EXPECT_TRUE(op.canView(Place{G21, O2})) << "...whereas a full place is fine";
    EXPECT_FALSE(op.canView(half)) << "a half-known place is refused";
}

TEST(PlayerAccess, TheTreeShowsTheGrantedObjectAndGroupsAndNothingElse) {
    PlayerAccess g(Role::Operator, {group(G11)});
    EXPECT_TRUE(g.canSeeGroup(G11, O1));
    EXPECT_FALSE(g.canSeeGroup(G12, O1)) << "the other floors of the building stay hidden";
    EXPECT_FALSE(g.canSeeObject(O1)) << "an object right is not implied by a group right";
    EXPECT_TRUE(g.seesObjectContaining(O1)) << "but the building holding the granted group has to be named";
    EXPECT_FALSE(g.seesObjectContaining(O2));

    PlayerAccess o(Role::Operator, {object(O1)});
    EXPECT_TRUE(o.canSeeObject(O1));
    EXPECT_TRUE(o.canSeeGroup(G11, O1));
    EXPECT_TRUE(o.canSeeGroup(G12, O1)) << "also groups created later";
    EXPECT_FALSE(o.canSeeGroup(G21, O2));
}

TEST(PlayerAccess, TheUnrestrictedModeIsForSetupsWithoutAuthentication) {
    auto a = PlayerAccess::unrestricted();
    EXPECT_TRUE(a.isAdmin());
    EXPECT_TRUE(a.canCommandAll());
}
