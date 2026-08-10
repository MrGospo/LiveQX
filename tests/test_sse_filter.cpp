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
    EXPECT_TRUE(sseEventVisibleTo(ctx, evtFor(EventType::AuthAudit)));
    EXPECT_TRUE(sseEventVisibleTo(ctx, evtFor(EventType::PluginStatusChange)));
    EXPECT_TRUE(sseEventVisibleTo(ctx, evtFor(EventType::ChannelStateChange, 7)));
    EXPECT_TRUE(sseEventVisibleTo(ctx, evtFor(EventType::ChannelStateChange, -1)));
    EXPECT_TRUE(sseEventVisibleTo(ctx, evtFor(EventType::HealthChange, 99)));
}

TEST(SseFilter, OperatorBlockedFromAuthAudit) {
    auto ctx = ctxFor(Role::Operator);
    EXPECT_FALSE(sseEventVisibleTo(ctx, evtFor(EventType::AuthAudit)));
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

TEST(SseFilter, ViewerBlockedFromAuthAuditAndPlugin) {
    auto ctx = ctxFor(Role::Viewer);
    EXPECT_FALSE(sseEventVisibleTo(ctx, evtFor(EventType::AuthAudit)));
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
