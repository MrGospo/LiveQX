#pragma once

// fix23 — process-local event bus for SSE / future internal listeners.
//
// Single bus instance per process. Producers (ChannelInstance,
// OutputManager, ChannelHealth, AuthService) call `publish()` which
// fans out to every active Subscription's bounded queue. Each
// Subscription has its own queue + condition variable, so a slow
// consumer is isolated: when its queue overflows its `overflowed`
// flag is set and the *bus* keeps moving for everyone else.
//
// Per the SSE protocol an event has an integer monotonic id used by
// `Last-Event-ID` for re-attach replay; the bus keeps the last
// `replay_size` events in a ring buffer and serves them on subscribe.
//
// Thread-safety: publish() and subscribe() are mutex-protected
// against the subscriber list. Each Subscription queue is mutex+CV
// protected internally.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace liveqx::events {

// Discrete set of events the UI subscribes to. Adding a new type is
// a 3-line change: enum + name() + role-visibility table in
// SseSubscriber. Payload shape is documented in docs/UI_API.md.
enum class EventType {
    ChannelStateChange,    // {channel_id, state}
    OutputStateChange,     // {channel_id, output_id, state}
    ClipChange,            // {channel_id, clip_id, name}
    HealthChange,          // {channel_id, health, reason}
    ScheduleActive,        // {channel_id, entry_id}
    PluginStatusChange,    // {plugin, status}
    AuthAudit,             // {event, user, ip, ...}
    StressRunStarted,      // {started_at_ms, duration_sec, channels, scenarios}
    StressRunFinished,     // {pass, verdict, report_id, ended_at_ms}
    GatewayStateChange,    // {id, name, state}  fix33 D1
    AuditEvent,            // enterprise audit trail — one row inserted:
                           // {category, action, target_type, target_id,
                           //  actor_username, http_method, http_path,
                           //  http_status, request_id}. UI uses this
                           // as an invalidation trigger — full row is
                           // fetched via GET /api/audit/events.
};

const char* eventTypeName(EventType t) noexcept;
std::optional<EventType> parseEventType(const std::string& s) noexcept;

struct Event {
    std::uint64_t   id          = 0;
    std::int64_t    ts_unix_ms  = 0;
    EventType       type        = EventType::ChannelStateChange;
    int             channel_id  = -1;     // -1 if not channel-bound
    nlohmann::json  payload     = nlohmann::json::object();
};

// Internal queue node owned by the subscriber. Public only so EventBus
// can construct it; users get a `shared_ptr<Subscription>` from
// `subscribe()` and interact through the methods below.
class Subscription {
public:
    // Move-only style ownership; subscribers obtain a shared_ptr from
    // EventBus::subscribe() so the bus can hold a weak_ptr in its
    // fan-out list and detect dropped subscribers automatically.
    explicit Subscription(std::size_t capacity);

    // Block up to `timeout` for events; returns immediately with all
    // events currently available. Returns empty vector on timeout
    // with no events. Caller checks `overflowed()` after to detect
    // queue overflow (subscriber dropped events).
    std::vector<Event> drain(std::chrono::milliseconds timeout);

    // True if the producer-side queue overflowed at any point since
    // last drain — caller should treat as "must reconnect" and abort
    // the SSE stream.
    bool overflowed() const noexcept { return overflowed_.load(); }

    // Last seen event id — useful for clients reconnecting with
    // Last-Event-ID. Zero before first drain.
    std::uint64_t lastId() const noexcept { return last_id_.load(); }

    // Wake any thread blocked in drain() — used on shutdown to make
    // the SSE handler unblock and return promptly.
    void wakeUp();

private:
    friend class EventBus;
    void enqueue(const Event& e);   // called by EventBus::publish

    mutable std::mutex      mtx_;
    std::condition_variable cv_;
    std::deque<Event>       queue_;
    std::size_t             capacity_;
    std::atomic<bool>       overflowed_{false};
    std::atomic<bool>       closed_{false};
    std::atomic<std::uint64_t> last_id_{0};
};

class EventBus {
public:
    explicit EventBus(std::size_t replay_size            = 256,
                      std::size_t per_subscriber_capacity = 1024);

    // Publish a new event. Synchronous: invokes `enqueue()` on each
    // active subscription. Returns the assigned id.
    std::uint64_t publish(EventType type, int channel_id, nlohmann::json payload);

    // Subscribe. If `since_id` is set, replay events from the ring
    // buffer with id > since_id before the subscription receives any
    // new ones. Returned shared_ptr controls the lifetime — when it
    // drops, the bus removes the subscription on its next publish.
    std::shared_ptr<Subscription> subscribe(
        std::optional<std::uint64_t> since_id = std::nullopt,
        std::optional<std::size_t>   capacity_override = std::nullopt);

    // For diagnostics / metrics — current subscriber count.
    std::size_t subscriberCount() const noexcept;

    // Latest published id (0 before first publish).
    std::uint64_t lastId() const noexcept { return next_id_.load() - 1; }

private:
    void purgeExpiredLocked();

    mutable std::mutex                              mtx_;
    std::vector<std::weak_ptr<Subscription>>        subs_;
    std::deque<Event>                               replay_;
    std::size_t                                     replay_size_;
    std::size_t                                     per_sub_capacity_;
    std::atomic<std::uint64_t>                      next_id_{1};
};

}  // namespace liveqx::events
