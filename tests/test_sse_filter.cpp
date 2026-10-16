// fix23 commit 4 — unit tests для sseEventVisibleTo (per-event role filter).
//
// Чёрный ящик: для каждой комбинации (Role × EventType × ACL) проверяем
// решение. Запускается без сервера/сетевого стека.

#include <gtest/gtest.h>

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
// A7: any signed-in user may list players, so any role may see their events;
// players are not channel-scoped, so channel grants play no part.
TEST(SseFilter, EveryRoleSeesPlayerEvents) {
    for (EventType t : {EventType::PlayerRegistered, EventType::PlayerOnline,
                        EventType::PlayerOffline, EventType::PlayerHeartbeat,
                        EventType::PlayerVersionChanged}) {
        EXPECT_TRUE(sseEventVisibleTo(ctxFor(Role::Admin), evtFor(t)))    << liveqx::events::eventTypeName(t);
        EXPECT_TRUE(sseEventVisibleTo(ctxFor(Role::Operator), evtFor(t))) << liveqx::events::eventTypeName(t);
        EXPECT_TRUE(sseEventVisibleTo(ctxFor(Role::Viewer), evtFor(t)))   << liveqx::events::eventTypeName(t);
    }
}

TEST(SseFilter, PlayerEventsNeedNoChannelGrant) {
    // A viewer without any grant still sees them; channel_id is -1 for these.
    EXPECT_TRUE(sseEventVisibleTo(ctxFor(Role::Viewer),
                                  evtFor(EventType::PlayerOnline, /*channel_id=*/-1)));
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
