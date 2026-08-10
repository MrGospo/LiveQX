#pragma once
#include <cstdint>
#include <vector>

#include "auth/AuthTypes.h"

namespace liveqx::api {

// Visibility scope for cross-channel responses (status, metrics, SSE).
//
// The convention mirrors SseFilter: Admin and Operator are global roles —
// they see every channel and every cross-channel resource (gateways,
// stress, plugins). Viewer is per-channel — they see only the channels
// they have an explicit grant for.
//
// `show_all == true`  → renderers must skip filtering.
// `show_all == false` → renderers must drop entries whose channel_id is
//                       not in `allowed_channels`. Cross-channel summaries
//                       must be recomputed from the visible subset.
struct ChannelScope {
    bool                       show_all = true;
    std::vector<std::int64_t>  allowed_channels;  // sorted for binary_search

    // O(log N) — `allowed_channels` is sorted at construction time.
    bool allows(std::int64_t channel_id) const noexcept;

    // Operator/Admin see gateways; Viewer does not. The latter is
    // consistent with SseFilter, where gateway events are admin/op only.
    bool allowsCrossChannel() const noexcept { return show_all; }
};

// Build a scope from an authenticated request. Empty/Viewer-with-no-grants
// produces an empty scope (`show_all == false`, allowed_channels empty),
// which is the correct "see nothing" outcome.
ChannelScope makeChannelScope(const liveqx::auth::RequestContext& ctx);

}  // namespace liveqx::api
