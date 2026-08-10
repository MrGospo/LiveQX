#include "events/EventBus.h"

#include <algorithm>
#include <chrono>

namespace liveqx::events {

const char* eventTypeName(EventType t) noexcept {
    switch (t) {
        case EventType::ChannelStateChange:  return "channel_state_change";
        case EventType::OutputStateChange:   return "output_state_change";
        case EventType::ClipChange:          return "clip_change";
        case EventType::HealthChange:        return "health_change";
        case EventType::ScheduleActive:      return "schedule_active";
        case EventType::PluginStatusChange:  return "plugin_status_change";
        case EventType::AuthAudit:           return "auth_audit";
        case EventType::StressRunStarted:    return "stress_run_started";
        case EventType::StressRunFinished:   return "stress_run_finished";
        case EventType::GatewayStateChange:  return "gateway_state_change";
    }
    return "unknown";
}

std::optional<EventType> parseEventType(const std::string& s) noexcept {
    if (s == "channel_state_change")  return EventType::ChannelStateChange;
    if (s == "output_state_change")   return EventType::OutputStateChange;
    if (s == "clip_change")           return EventType::ClipChange;
    if (s == "health_change")         return EventType::HealthChange;
    if (s == "schedule_active")       return EventType::ScheduleActive;
    if (s == "plugin_status_change")  return EventType::PluginStatusChange;
    if (s == "auth_audit")            return EventType::AuthAudit;
    if (s == "stress_run_started")    return EventType::StressRunStarted;
    if (s == "stress_run_finished")   return EventType::StressRunFinished;
    if (s == "gateway_state_change")  return EventType::GatewayStateChange;
    return std::nullopt;
}

// ─── Subscription ────────────────────────────────────────────────────────────

Subscription::Subscription(std::size_t capacity) : capacity_(capacity) {}

void Subscription::enqueue(const Event& e) {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (closed_.load()) return;
        if (queue_.size() >= capacity_) {
            // Slow consumer — keep queue bounded by the capacity, but
            // mark overflow so the SSE handler aborts the stream.
            overflowed_.store(true);
            // Drop the oldest event to make room: the freshest events
            // are the most useful for a reader that is anyway about
            // to be told to reconnect.
            queue_.pop_front();
        }
        queue_.push_back(e);
        last_id_.store(e.id);
    }
    cv_.notify_one();
}

std::vector<Event> Subscription::drain(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lk(mtx_);
    if (queue_.empty() && !closed_.load()) {
        cv_.wait_for(lk, timeout, [this] {
            return !queue_.empty() || closed_.load();
        });
    }
    std::vector<Event> out;
    out.reserve(queue_.size());
    while (!queue_.empty()) {
        out.push_back(std::move(queue_.front()));
        queue_.pop_front();
    }
    return out;
}

void Subscription::wakeUp() {
    closed_.store(true);
    cv_.notify_all();
}

// ─── EventBus ────────────────────────────────────────────────────────────────

EventBus::EventBus(std::size_t replay_size, std::size_t per_subscriber_capacity)
    : replay_size_(replay_size), per_sub_capacity_(per_subscriber_capacity) {}

std::uint64_t EventBus::publish(EventType type, int channel_id, nlohmann::json payload) {
    using clock = std::chrono::system_clock;
    Event e;
    e.id         = next_id_.fetch_add(1, std::memory_order_relaxed);
    e.ts_unix_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                       clock::now().time_since_epoch()).count();
    e.type       = type;
    e.channel_id = channel_id;
    e.payload    = std::move(payload);

    std::vector<std::shared_ptr<Subscription>> live;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        // Append to ring buffer.
        replay_.push_back(e);
        while (replay_.size() > replay_size_) replay_.pop_front();

        // Snapshot live subscribers and prune dead ones in one pass.
        live.reserve(subs_.size());
        std::erase_if(subs_, [&](const std::weak_ptr<Subscription>& w) {
            if (auto sp = w.lock()) {
                live.push_back(std::move(sp));
                return false;
            }
            return true;
        });
    }
    for (auto& s : live) s->enqueue(e);
    return e.id;
}

std::shared_ptr<Subscription> EventBus::subscribe(
    std::optional<std::uint64_t> since_id,
    std::optional<std::size_t>   capacity_override)
{
    auto sub = std::make_shared<Subscription>(
        capacity_override.value_or(per_sub_capacity_));

    std::lock_guard<std::mutex> lk(mtx_);
    // Replay older events first so they queue ahead of any future
    // publish() — keeps id ordering monotonic for the reader.
    if (since_id) {
        for (const auto& e : replay_) {
            if (e.id > *since_id) sub->enqueue(e);
        }
    }
    subs_.push_back(sub);
    return sub;
}

std::size_t EventBus::subscriberCount() const noexcept {
    std::lock_guard<std::mutex> lk(mtx_);
    std::size_t n = 0;
    for (const auto& w : subs_) if (!w.expired()) ++n;
    return n;
}

}  // namespace liveqx::events
