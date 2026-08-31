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
//   AuditEvent             — Admin only (enterprise audit trail signal)
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

// True if the event type is part of the *default* /api/events/stream
// subscription (i.e. what the operator sees on the general "События" page
// without narrowing via ?types=). Loud, per-clip traffic (ClipChange) is
// excluded from the default set — such events belong in the per-channel
// log tab which subscribes explicitly with ?types=clip_change. Rationale:
// on a busy install with dozens of channels ClipChange fires several times
// per minute per channel and drowns operational events (health / output
// state / audit) that actually need operator attention. Excluding it from
// the default set does not change RBAC or EventBus publish behaviour —
// clients that ask for it get it.
bool sseEventInDefaultSubscription(liveqx::events::EventType t) noexcept;

}  // namespace liveqx::api
