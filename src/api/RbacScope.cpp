#include "api/RbacScope.h"

#include <algorithm>

namespace liveqx::api {

bool ChannelScope::allows(std::int64_t channel_id) const noexcept {
    if (show_all) return true;
    return std::binary_search(allowed_channels.begin(),
                              allowed_channels.end(), channel_id);
}

ChannelScope makeChannelScope(const liveqx::auth::RequestContext& ctx) {
    using Role = liveqx::auth::Role;
    ChannelScope s;
    if (ctx.role == Role::Admin || ctx.role == Role::Operator) {
        s.show_all = true;
        return s;
    }
    s.show_all = false;
    s.allowed_channels.reserve(ctx.channel_grants.size());
    for (const auto& g : ctx.channel_grants) {
        s.allowed_channels.push_back(g.channel_id);
    }
    std::sort(s.allowed_channels.begin(), s.allowed_channels.end());
    s.allowed_channels.erase(
        std::unique(s.allowed_channels.begin(), s.allowed_channels.end()),
        s.allowed_channels.end());
    return s;
}

}  // namespace liveqx::api
