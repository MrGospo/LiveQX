#pragma once

// fix23 commit 4 — per-event SSE visibility filter.
//
// Pulled out of ControlApi.cpp so unit tests can pin the role/ACL matrix
// without spinning up a full httplib server. The implementation lives in
// SseFilter.cpp (single TU; production callers go through this header
// only — there is no anonymous-namespace duplicate any more).

#include "auth/AuthTypes.h"
#include "events/EventBus.h"

namespace liveqx::api {

// True if a Subject (resolved by RBAC pre-handler) is allowed to see the
// given event on its SSE stream. Visibility matrix:
//
//   AuthAudit              — Admin only
//   PluginStatusChange     — Admin + Operator
//   ChannelStateChange,
//   OutputStateChange,
//   ClipChange,
//   HealthChange,
//   ScheduleActive         — Admin/Operator: every channel.
//                            Viewer: only channels the Subject has an
//                            explicit channel_grant for.
//                            Events with channel_id < 0 are hidden from
//                            non-Admin/Operator subjects.
//
// Pure function — no allocation, no logging; suitable for the inner
// SSE provider loop.
bool sseEventVisibleTo(const liveqx::auth::RequestContext& ctx,
                       const liveqx::events::Event& e) noexcept;

}  // namespace liveqx::api
