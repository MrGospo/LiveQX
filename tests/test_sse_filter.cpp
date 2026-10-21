// fix23 commit 4 — unit tests для sseEventVisibleTo (per-event role filter).
//
// Чёрный ящик: для каждой комбинации (Role × EventType × ACL) проверяем
// решение. Запускается без сервера/сетевого стека.

#include <gtest/gtest.h>

#include <optional>

#include "api/SseFilter.h"
#include "auth/AuthTypes.h"
#include "events/EventBus.h"

namespace {

using liveqx::api::sseEventVisibleTo;
using liveqx::auth::ChannelGrant;
using liveqx::auth::ChannelPermission;
using liveqx::auth::RequestContext;
using liveqx::auth::Role;
using liveqx::events::Event;
using liveqx::events::EventType;

RequestContext ctxFor(Role r) {
    RequestContext c;
    c.role = r;
    return c;
}

Event evtFor(EventType t, int channel_id = -1) {
    Event e;
    e.id         = 1;
    e.type       = t;
    e.channel_id = channel_id;
    return e;
}

}  // namespace

TEST(SseFilter, AdminSeesEverything) {
    auto ctx = ctxFor(Role::Admin);
    EXPECT_TRUE(sseEventVisibleTo(ctx, evtFor(EventType::AuditEvent)));
    EXPECT_TRUE(sseEventVisibleTo(ctx, evtFor(EventType::PluginStatusChange)));
    EXPECT_TRUE(sseEventVisibleTo(ctx, evtFor(EventType::ChannelStateChange, 7)));
    EXPECT_TRUE(sseEventVisibleTo(ctx, evtFor(EventType::ChannelStateChange, -1)));
    EXPECT_TRUE(sseEventVisibleTo(ctx, evtFor(EventType::HealthChange, 99)));
}

TEST(SseFilter, OperatorBlockedFromAuditEvent) {
    auto ctx = ctxFor(Role::Operator);
    EXPECT_FALSE(sseEventVisibleTo(ctx, evtFor(EventType::AuditEvent)));
}

TEST(SseFilter, OperatorAllowedForPluginAndChannelEvents) {
    auto ctx = ctxFor(Role::Operator);
    EXPECT_TRUE(sseEventVisibleTo(ctx, evtFor(EventType::PluginStatusChange)));
    EXPECT_TRUE(sseEventVisibleTo(ctx, evtFor(EventType::ChannelStateChange, 1)));
    EXPECT_TRUE(sseEventVisibleTo(ctx, evtFor(EventType::OutputStateChange, 5)));
    EXPECT_TRUE(sseEventVisibleTo(ctx, evtFor(EventType::ClipChange, 5)));
    EXPECT_TRUE(sseEventVisibleTo(ctx, evtFor(EventType::HealthChange, 5)));
    EXPECT_TRUE(sseEventVisibleTo(ctx, evtFor(EventType::ScheduleActive, 5)));
    EXPECT_TRUE(sseEventVisibleTo(ctx, evtFor(EventType::GatewayStateChange)));
}

TEST(SseFilter, ViewerBlockedFromGatewayStateChange) {
    auto ctx = ctxFor(Role::Viewer);
    EXPECT_FALSE(sseEventVisibleTo(ctx, evtFor(EventType::GatewayStateChange)));
}

TEST(SseFilter, ViewerBlockedFromAuditEventAndPlugin) {
    auto ctx = ctxFor(Role::Viewer);
    EXPECT_FALSE(sseEventVisibleTo(ctx, evtFor(EventType::AuditEvent)));
    EXPECT_FALSE(sseEventVisibleTo(ctx, evtFor(EventType::PluginStatusChange)));
}

TEST(SseFilter, ViewerWithoutGrantBlockedFromChannelEvent) {
    auto ctx = ctxFor(Role::Viewer);
    EXPECT_FALSE(sseEventVisibleTo(ctx, evtFor(EventType::ChannelStateChange, 7)));
    EXPECT_FALSE(sseEventVisibleTo(ctx, evtFor(EventType::HealthChange,       7)));
    EXPECT_FALSE(sseEventVisibleTo(ctx, evtFor(EventType::ClipChange,         7)));
}

TEST(SseFilter, ViewerWithGrantSeesOnlyOwnChannel) {
    auto ctx = ctxFor(Role::Viewer);
    ctx.channel_grants.push_back(ChannelGrant{3, ChannelPermission::View});
    EXPECT_TRUE (sseEventVisibleTo(ctx, evtFor(EventType::ChannelStateChange, 3)));
    EXPECT_TRUE (sseEventVisibleTo(ctx, evtFor(EventType::HealthChange,       3)));
    EXPECT_FALSE(sseEventVisibleTo(ctx, evtFor(EventType::ChannelStateChange, 4)));
}

TEST(SseFilter, ViewerHidesChannellessEventsForChannelTypes) {
    // channel_id < 0 ⇒ Viewer cannot prove ACL → hidden by default.
    auto ctx = ctxFor(Role::Viewer);
    ctx.channel_grants.push_back(ChannelGrant{1, ChannelPermission::View});
    EXPECT_FALSE(sseEventVisibleTo(ctx, evtFor(EventType::ChannelStateChange, -1)));
    EXPECT_FALSE(sseEventVisibleTo(ctx, evtFor(EventType::HealthChange,       -1)));
}

TEST(SseFilter, OperatePermissionAlsoCounts) {
    // Permission level inside a grant doesn't restrict visibility — both
    // View and Operate let the viewer see channel events. The endpoint
    // gating (RBAC pre-handler) already enforces operate-vs-view on the
    // request route; SSE is read-only and unconstrained beyond ACL
    // membership.
    auto ctx = ctxFor(Role::Viewer);
    ctx.channel_grants.push_back(ChannelGrant{42, ChannelPermission::Operate});
    EXPECT_TRUE(sseEventVisibleTo(ctx, evtFor(EventType::ChannelStateChange, 42)));
}

// ── Default-subscription filter ───────────────────────────────────────────────
// Loud per-clip traffic is excluded from the default /api/events/stream feed;
// clients that need it subscribe explicitly (?types=clip_change). RBAC
// visibility (sseEventVisibleTo) is unaffected — this filter runs on top.

TEST(SseFilter, DefaultSubscriptionExcludesClipChange) {
    using liveqx::api::sseEventInDefaultSubscription;
    EXPECT_FALSE(sseEventInDefaultSubscription(EventType::ClipChange));
}

TEST(SseFilter, DefaultSubscriptionIncludesOperationalEvents) {
    using liveqx::api::sseEventInDefaultSubscription;
    EXPECT_TRUE(sseEventInDefaultSubscription(EventType::AuditEvent));
    EXPECT_TRUE(sseEventInDefaultSubscription(EventType::ChannelStateChange));
    EXPECT_TRUE(sseEventInDefaultSubscription(EventType::OutputStateChange));
    EXPECT_TRUE(sseEventInDefaultSubscription(EventType::HealthChange));
    EXPECT_TRUE(sseEventInDefaultSubscription(EventType::ScheduleActive));
    EXPECT_TRUE(sseEventInDefaultSubscription(EventType::PluginStatusChange));
    EXPECT_TRUE(sseEventInDefaultSubscription(EventType::StressRunStarted));
    EXPECT_TRUE(sseEventInDefaultSubscription(EventType::StressRunFinished));
    EXPECT_TRUE(sseEventInDefaultSubscription(EventType::GatewayStateChange));
}

// ── player events (LiveQX-Player integration) ───────────────────────────────
// B13: a player belongs to a group of an object, and a person sees the events of
// the players they have been given — nothing more. The event itself says where
// the player sits (group_id, object_id), so the filter needs no database.
using liveqx::auth::PlayerScopeGrant;

Event playerEvt(EventType t, std::optional<std::int64_t> group, std::optional<std::int64_t> object) {
    Event e = evtFor(t);
    e.payload = nlohmann::json::object();
    e.payload["player_id"] = "11111111-2222-4333-8444-555555555555";
    if (group)  e.payload["group_id"]  = *group;
    if (object) e.payload["object_id"] = *object;
    return e;
}

PlayerScopeGrant groupGrant(std::int64_t id) {
    PlayerScopeGrant g;
    g.scope = PlayerScopeGrant::Scope::Group;
    g.scope_id = id;
    return g;
}
PlayerScopeGrant objectGrant(std::int64_t id) {
    PlayerScopeGrant g;
    g.scope = PlayerScopeGrant::Scope::Object;
    g.scope_id = id;
    return g;
}

const EventType kPlayerEvents[] = {EventType::PlayerRegistered, EventType::PlayerOnline,
                                   EventType::PlayerOffline, EventType::PlayerHeartbeat,
                                   EventType::PlayerVersionChanged};

TEST(SseFilter, AnAdministratorSeesEveryPlayerEventEvenOneWithoutAPlace) {
    for (EventType t : kPlayerEvents) {
        EXPECT_TRUE(sseEventVisibleTo(ctxFor(Role::Admin), playerEvt(t, 11, 1))) << liveqx::events::eventTypeName(t);
        EXPECT_TRUE(sseEventVisibleTo(ctxFor(Role::Admin), playerEvt(t, std::nullopt, std::nullopt)));
    }
}

TEST(SseFilter, WithoutARightNoPlayerEventIsSeen) {
    for (Role r : {Role::Operator, Role::Viewer})
        for (EventType t : kPlayerEvents)
            EXPECT_FALSE(sseEventVisibleTo(ctxFor(r), playerEvt(t, 11, 1))) << liveqx::events::eventTypeName(t);
}

TEST(SseFilter, AGroupRightShowsTheEventsOfThatGroupOnly) {
    auto ctx = ctxFor(Role::Operator);
    ctx.player_grants = {groupGrant(11)};
    EXPECT_TRUE(sseEventVisibleTo(ctx, playerEvt(EventType::PlayerOnline, 11, 1)));
    EXPECT_FALSE(sseEventVisibleTo(ctx, playerEvt(EventType::PlayerOnline, 12, 1))) << "a sibling group";
    EXPECT_FALSE(sseEventVisibleTo(ctx, playerEvt(EventType::PlayerOnline, 21, 2)));
}

TEST(SseFilter, AnObjectRightShowsTheEventsOfAllItsGroups) {
    auto ctx = ctxFor(Role::Viewer);
    ctx.player_grants = {objectGrant(1)};
    EXPECT_TRUE(sseEventVisibleTo(ctx, playerEvt(EventType::PlayerOffline, 11, 1)));
    EXPECT_TRUE(sseEventVisibleTo(ctx, playerEvt(EventType::PlayerOffline, 12, 1)));
    EXPECT_FALSE(sseEventVisibleTo(ctx, playerEvt(EventType::PlayerOffline, 21, 2)));
}

TEST(SseFilter, AnEventThatDoesNotSayWhereItsPlayerSitsIsForAdministratorsOnly) {
    auto ctx = ctxFor(Role::Operator);
    ctx.player_grants = {objectGrant(1), groupGrant(11)};
    EXPECT_FALSE(sseEventVisibleTo(ctx, playerEvt(EventType::PlayerOnline, std::nullopt, std::nullopt)));
    EXPECT_FALSE(sseEventVisibleTo(ctx, playerEvt(EventType::PlayerOnline, 11, std::nullopt)));
    EXPECT_FALSE(sseEventVisibleTo(ctx, evtFor(EventType::PlayerOnline))) << "no payload at all";
}

TEST(SseFilter, AMalformedPlaceInAnEventIsRefusedNotTrusted) {
    auto ctx = ctxFor(Role::Operator);
    ctx.player_grants = {groupGrant(11)};
    Event e = evtFor(EventType::PlayerOnline);
    e.payload = {{"group_id", "11"}, {"object_id", 1}};
    EXPECT_FALSE(sseEventVisibleTo(ctx, e)) << "an id given as text does not count";
}

TEST(SseFilter, PlayerEventsNeedNoChannelGrant) {
    // Channel grants play no part: an operator with only a channel grant sees no player.
    auto ctx = ctxFor(Role::Operator);
    ctx.channel_grants = {ChannelGrant{7, ChannelPermission::Operate}};
    EXPECT_FALSE(sseEventVisibleTo(ctx, playerEvt(EventType::PlayerOnline, 11, 1)));
}

TEST(SseFilter, HeartbeatsAreNotInTheDefaultSubscription) {
    // A thousand players beating every half minute would drown the events page.
    EXPECT_FALSE(liveqx::api::sseEventInDefaultSubscription(EventType::PlayerHeartbeat));
    EXPECT_TRUE(liveqx::api::sseEventInDefaultSubscription(EventType::PlayerRegistered));
    EXPECT_TRUE(liveqx::api::sseEventInDefaultSubscription(EventType::PlayerOnline));
    EXPECT_TRUE(liveqx::api::sseEventInDefaultSubscription(EventType::PlayerOffline));
    EXPECT_TRUE(liveqx::api::sseEventInDefaultSubscription(EventType::PlayerVersionChanged));
}

TEST(SseFilter, PlayerEventNamesRoundTrip) {
    for (EventType t : {EventType::PlayerRegistered, EventType::PlayerOnline,
                        EventType::PlayerOffline, EventType::PlayerHeartbeat,
                        EventType::PlayerVersionChanged}) {
        const std::string name = liveqx::events::eventTypeName(t);
        EXPECT_EQ(name.rfind("player_", 0), 0u) << name;
        auto back = liveqx::events::parseEventType(name);
        ASSERT_TRUE(back.has_value()) << name;
        EXPECT_EQ(*back, t) << name;
    }
}

// ── audit hints by place ────────────────────────────────────────────────────
namespace {
Event auditHint(const std::string& category, std::optional<std::int64_t> object, std::optional<std::int64_t> group) {
    Event e = evtFor(EventType::AuditEvent);
    e.payload = {{"category", category}, {"action", "x"}};
    if (object) e.payload["scope_object_id"] = *object;
    if (group)  e.payload["scope_group_id"]  = *group;
    return e;
}
PlayerScopeGrant withAudit(PlayerScopeGrant g) { g.can_view_audit = true; return g; }
}  // namespace

TEST(SseFilterAudit, AdministratorsHearOfEveryRow) {
    auto ctx = ctxFor(Role::Admin);
    EXPECT_TRUE(sseEventVisibleTo(ctx, auditHint("system", std::nullopt, std::nullopt)));
    EXPECT_TRUE(sseEventVisibleTo(ctx, auditHint("player", 1, 11)));
}

TEST(SseFilterAudit, AnAuditRightOnAGroupOrObjectMeansHearingOfPlayerRowsOfThatPlaceOnly) {
    auto ctx = ctxFor(Role::Operator);
    ctx.player_grants = {withAudit(groupGrant(11))};
    EXPECT_TRUE(sseEventVisibleTo(ctx, auditHint("player", 1, 11)));
    EXPECT_FALSE(sseEventVisibleTo(ctx, auditHint("player", 1, 12))) << "the sibling group";
    EXPECT_FALSE(sseEventVisibleTo(ctx, auditHint("player", 1, std::nullopt))) << "an object is not implied by a group";
    EXPECT_FALSE(sseEventVisibleTo(ctx, auditHint("system", 1, 11))) << "right place, not a player event";

    ctx.player_grants = {withAudit(objectGrant(1))};
    EXPECT_TRUE(sseEventVisibleTo(ctx, auditHint("player", 1, 12)));
    EXPECT_TRUE(sseEventVisibleTo(ctx, auditHint("player", 1, std::nullopt)));
    EXPECT_FALSE(sseEventVisibleTo(ctx, auditHint("player", 2, 21)));
}

TEST(SseFilterAudit, WithoutTheAuditMarkOrWithoutAPlaceNothingIsHeard) {
    auto ctx = ctxFor(Role::Operator);
    ctx.player_grants = {groupGrant(11), objectGrant(1)};                   // access, but no audit mark
    EXPECT_FALSE(sseEventVisibleTo(ctx, auditHint("player", 1, 11)));
    ctx.player_grants = {withAudit(objectGrant(1))};
    EXPECT_FALSE(sseEventVisibleTo(ctx, auditHint("player", std::nullopt, std::nullopt))) << "a row about no place";
    ctx.player_grants.clear();
    EXPECT_FALSE(sseEventVisibleTo(ctx, auditHint("player", 1, 11)));
    Event odd = evtFor(EventType::AuditEvent);
    odd.payload = "not an object";
    ctx.player_grants = {withAudit(objectGrant(1))};
    EXPECT_FALSE(sseEventVisibleTo(ctx, odd));
}

TEST(SseFilterAudit, AViewerWithTheMarkHearsToo) {
    auto ctx = ctxFor(Role::Viewer);
    ctx.player_grants = {withAudit(groupGrant(11))};
    EXPECT_TRUE(sseEventVisibleTo(ctx, auditHint("player", 1, 11)));
}

TEST(SseFilterAudit, ARightOnEitherEndOfAMoveMeansHearingOfIt) {
    Event move = evtFor(EventType::AuditEvent);
    move.payload = {{"category", "player"}, {"action", "player.move"}, {"scope_object_id", 1}, {"scope_group_id", 12},
                    {"scope_from_object_id", 1}, {"scope_from_group_id", 11}};
    auto ctx = ctxFor(Role::Operator);
    ctx.player_grants = {withAudit(groupGrant(11))};
    EXPECT_TRUE(sseEventVisibleTo(ctx, move)) << "the group the screen left";
    ctx.player_grants = {withAudit(groupGrant(12))};
    EXPECT_TRUE(sseEventVisibleTo(ctx, move)) << "the group it arrived in";
    ctx.player_grants = {withAudit(groupGrant(13))};
    EXPECT_FALSE(sseEventVisibleTo(ctx, move));
    ctx.player_grants = {withAudit(objectGrant(2))};
    EXPECT_FALSE(sseEventVisibleTo(ctx, move));
}
