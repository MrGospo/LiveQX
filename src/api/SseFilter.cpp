#include "api/SseFilter.h"

#include "players/PlayerAccess.h"

namespace liveqx::api {

namespace {

bool playerEventVisible(const liveqx::auth::RequestContext& ctx, const liveqx::events::Event& e) noexcept {
    if (ctx.role == liveqx::auth::Role::Admin) return true;
    liveqx::players::Place place;
    try {
        if (e.payload.is_object()) {
            if (auto it = e.payload.find("group_id"); it != e.payload.end() && it->is_number_integer())
                place.group_id = it->get<std::int64_t>();
            if (auto it = e.payload.find("object_id"); it != e.payload.end() && it->is_number_integer())
                place.object_id = it->get<std::int64_t>();
        }
    } catch (...) {
        return false;
    }
    return liveqx::players::PlayerAccess::canViewWith(ctx.role, ctx.player_grants, place);
}

}  // namespace

bool sseEventVisibleTo(const liveqx::auth::RequestContext& ctx,
                       const liveqx::events::Event& e) noexcept {
    using EventType = liveqx::events::EventType;
    using Role      = liveqx::auth::Role;
    switch (e.type) {
        case EventType::AuditEvent:
            return ctx.role == Role::Admin;
        case EventType::PluginStatusChange:
        case EventType::StressRunStarted:
        case EventType::StressRunFinished:
        case EventType::GatewayStateChange:
            return ctx.role == Role::Admin || ctx.role == Role::Operator;
        case EventType::PlayerRegistered:
        case EventType::PlayerOnline:
        case EventType::PlayerOffline:
        case EventType::PlayerHeartbeat:
        case EventType::PlayerVersionChanged:
        case EventType::PlayerCommand:
            // A person sees the events of the players they have been given (B13).
            // The event names the player's group and object, so no lookup is
            // needed; one that does not, or names them oddly, is for
            // administrators only.
            return playerEventVisible(ctx, e);
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
        case EventType::PlayerHeartbeat:
            // A thousand players beating every half minute would drown the
            // events page; a client that wants them asks with ?types=.
            return false;
        default:
            return true;
    }
}

}  // namespace liveqx::api
