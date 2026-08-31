#include "api/SseFilter.h"

namespace liveqx::api {

bool sseEventVisibleTo(const liveqx::auth::RequestContext& ctx,
                       const liveqx::events::Event& e) noexcept {
    using EventType = liveqx::events::EventType;
    using Role      = liveqx::auth::Role;
    switch (e.type) {
        case EventType::AuthAudit:
        case EventType::AuditEvent:
            return ctx.role == Role::Admin;
        case EventType::PluginStatusChange:
        case EventType::StressRunStarted:
        case EventType::StressRunFinished:
        case EventType::GatewayStateChange:
            return ctx.role == Role::Admin || ctx.role == Role::Operator;
        case EventType::ChannelStateChange:
        case EventType::OutputStateChange:
        case EventType::ClipChange:
        case EventType::HealthChange:
        case EventType::ScheduleActive:
            if (ctx.role == Role::Admin || ctx.role == Role::Operator) return true;
            if (e.channel_id < 0) return false;
            for (const auto& g : ctx.channel_grants) {
                if (g.channel_id == e.channel_id) return true;
            }
            return false;
    }
    return false;
}

bool sseEventInDefaultSubscription(liveqx::events::EventType t) noexcept {
    using EventType = liveqx::events::EventType;
    switch (t) {
        case EventType::ClipChange:
            // Per-clip traffic — subscribed explicitly by the channel Log tab.
            return false;
        default:
            return true;
    }
}

}  // namespace liveqx::api
