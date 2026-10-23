#include "api/SseFilter.h"

#include "players/PlayerAccess.h"

#include <algorithm>
#include <vector>

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

// A hint that an audit row was written. Beyond administrators, only those who were
// given the audit of the place it is about hear of it — and only player events.
bool auditEventVisible(const liveqx::auth::RequestContext& ctx, const liveqx::events::Event& e) noexcept {
    if (ctx.role == liveqx::auth::Role::Admin) return true;
    try {
        if (!e.payload.is_object()) return false;
        auto cat = e.payload.find("category");
        if (cat == e.payload.end() || !cat->is_string() || cat->get<std::string>() != "player") return false;
        // The place it happened in and, for a move, the place it came from: a right
        // on either end means hearing of it.
        std::vector<std::int64_t> objects, groups;
        for (const char* key : {"scope_object_id", "scope_from_object_id"})
            if (auto it = e.payload.find(key); it != e.payload.end() && it->is_number_integer())
                objects.push_back(it->get<std::int64_t>());
        for (const char* key : {"scope_group_id", "scope_from_group_id"})
            if (auto it = e.payload.find(key); it != e.payload.end() && it->is_number_integer())
                groups.push_back(it->get<std::int64_t>());
        auto has = [](const std::vector<std::int64_t>& v, std::int64_t id) {
            return std::find(v.begin(), v.end(), id) != v.end();
        };
        for (const auto& g : ctx.player_grants) {
            if (!g.can_view_audit) continue;
            if (g.scope == liveqx::auth::PlayerScopeGrant::Scope::Object && has(objects, g.scope_id)) return true;
            if (g.scope == liveqx::auth::PlayerScopeGrant::Scope::Group && has(groups, g.scope_id)) return true;
        }
    } catch (...) {
    }
    return false;
}

}  // namespace

bool sseEventVisibleTo(const liveqx::auth::RequestContext& ctx,
                       const liveqx::events::Event& e) noexcept {
    using EventType = liveqx::events::EventType;
    using Role      = liveqx::auth::Role;
    switch (e.type) {
        case EventType::AuditEvent:
            return auditEventVisible(ctx, e);
        case EventType::PlayerCommandJob: {
            // A job is shown to whoever started it and to administrators, so its news goes the same way.
            if (ctx.role == Role::Admin) return true;
            if (ctx.role != Role::Operator) return false;
            auto by = e.payload.find("created_by");
            return by != e.payload.end() && by->is_number_integer() && by->get<std::int64_t>() == ctx.user_id;
        }
        case EventType::PlayersBrokerStatus:           // the state of the broker is an administrator's page
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
