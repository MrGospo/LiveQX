#include "metrics/ChannelHealth.h"
#include "events/EventBus.h"
#include "utils/Log.h"

namespace {
constexpr std::int64_t kHeartbeatFailNs    = 5LL  * 1'000'000'000LL;  // 5s
constexpr std::int64_t kRecoverWindowNs    = 30LL * 1'000'000'000LL;  // 30s clean → step down
constexpr std::int64_t kPromoteToFailNs    = 30LL * 1'000'000'000LL;  // 30s degraded → fail
constexpr std::int64_t kFpsGraceNs         = 10LL * 1'000'000'000LL;  // skip fps check until rolling window full
constexpr std::int64_t kShareUnreachableNs = 30LL * 1'000'000'000LL;  // 30s no successful share scan → fail
constexpr std::uint64_t kDropsDegradedThr  = 10;                      // > 10 drops / 10s
constexpr double        kFpsFailRatio      = 0.5;                     // < 0.5 × target
}  // namespace

const char* healthStateName(HealthState s) noexcept {
    switch (s) {
        case HealthState::Running:  return "running";
        case HealthState::Degraded: return "degraded";
        case HealthState::Failed:   return "failed";
    }
    return "unknown";
}

ChannelHealth::ChannelHealth(std::string channel_id, int target_fps)
    : channel_id_(std::move(channel_id)), target_fps_(target_fps) {}

void ChannelHealth::transitionTo(HealthState target,
                                 std::int64_t now_ns,
                                 const char* reason) noexcept {
    const HealthState prev = state_.load(std::memory_order_acquire);
    if (prev == target) return;
    state_.store(target, std::memory_order_release);
    entered_at_ns_.store(now_ns, std::memory_order_relaxed);

    if (target == HealthState::Failed)
        LOG_ERROR("channel={} health={} → {} reason={}",
                  channel_id_, healthStateName(prev), healthStateName(target), reason);
    else if (target == HealthState::Degraded)
        LOG_WARN ("channel={} health={} → {} reason={}",
                  channel_id_, healthStateName(prev), healthStateName(target), reason);
    else
        LOG_INFO ("channel={} health={} → {} reason={}",
                  channel_id_, healthStateName(prev), healthStateName(target), reason);

    // fix23 — surface transition to subscribers (UI). channel_id_ is a
    // string ("ch{id}-{name}"), so emit -1 here; SSE consumers correlate
    // by the payload's channel_id field.
    if (event_bus_) {
        event_bus_->publish(
            liveqx::events::EventType::HealthChange,
            channel_id_int_,
            {{"channel_id", channel_id_int_},
             {"channel",    channel_id_},
             {"health",     healthStateName(target)},
             {"previous",   healthStateName(prev)},
             {"reason",     reason}});
    }
}

void ChannelHealth::reset() noexcept {
    state_.store(HealthState::Running, std::memory_order_release);
    entered_at_ns_.store(0, std::memory_order_relaxed);
    last_clean_ns_ = 0;
    first_eval_ns_ = -1;
}

void ChannelHealth::evaluate(const ChannelMetricsSnapshot& s,
                             std::int64_t now_ns,
                             std::int64_t heartbeat_age_ns,
                             OutputHealthSummary outputs) noexcept {
    if (first_eval_ns_ < 0) first_eval_ns_ = now_ns;

    // Grace period: rolling_rendered window needs ~10s to fill. Before that
    // rendered_10s is naturally small and would trigger false fps_starved.
    const bool window_warm = (now_ns - first_eval_ns_) >= kFpsGraceNs;
    const bool fps_starved =
        window_warm && target_fps_ > 0 &&
        static_cast<double>(s.rendered_10s) <
            kFpsFailRatio * static_cast<double>(target_fps_) * 10.0;
    const bool heartbeat_lost = heartbeat_age_ns >= kHeartbeatFailNs;
    // share_unreachable: only meaningful once ContentSync has produced at least
    // one successful scan (last_share_ok_ns > 0). Otherwise the channel is not
    // share-backed (legacy playlist) and this signal is silent.
    const bool share_unreachable =
        s.last_share_ok_ns > 0 &&
        (now_ns - s.last_share_ok_ns) >= kShareUnreachableNs;
    // Per-output fan-out signals (fix12 c7). With multiple outputs the
    // channel may be partially up: at least one driver healthy ⇒ Degraded,
    // every driver unhealthy ⇒ Failed. Single-output channels collapse to
    // the binary case (healthy=1/total=1 or healthy=0/total=1). total=0
    // means "no probe data" and is silent.
    const bool outputs_all_down = outputs.total > 0 && outputs.healthy == 0;
    const bool outputs_partial  = outputs.total > 0 && outputs.healthy > 0
                                  && outputs.healthy < outputs.total;

    const bool fail_signal    = heartbeat_lost || fps_starved
                                 || share_unreachable || outputs_all_down;
    const bool degrade_signal = s.drops_10s > kDropsDegradedThr
                                 || s.underruns_10s > 0
                                 || outputs_partial;
    const bool any_bad        = fail_signal || degrade_signal;

    // Track length of the current uninterrupted clean streak.
    if (any_bad) {
        last_clean_ns_ = 0;
    } else if (last_clean_ns_ == 0) {
        last_clean_ns_ = now_ns;
    }
    const std::int64_t clean_for_ns =
        (last_clean_ns_ == 0) ? 0 : (now_ns - last_clean_ns_);

    const HealthState cur     = state_.load(std::memory_order_acquire);
    const std::int64_t entered = entered_at_ns_.load(std::memory_order_relaxed);

    switch (cur) {
        case HealthState::Running:
            if (fail_signal) {
                transitionTo(HealthState::Failed, now_ns,
                             heartbeat_lost ? "heartbeat" :
                             share_unreachable ? "share_unreachable" :
                             outputs_all_down ? "all_outputs_down" : "fps_starved");
            } else if (degrade_signal) {
                transitionTo(HealthState::Degraded, now_ns,
                             outputs_partial ? "outputs_partial" :
                             s.underruns_10s > 0 ? "audio_underruns" : "video_drops");
            }
            break;

        case HealthState::Degraded:
            if (fail_signal) {
                transitionTo(HealthState::Failed, now_ns,
                             heartbeat_lost ? "heartbeat" :
                             share_unreachable ? "share_unreachable" :
                             outputs_all_down ? "all_outputs_down" : "fps_starved");
            } else if (degrade_signal && (now_ns - entered) >= kPromoteToFailNs) {
                transitionTo(HealthState::Failed, now_ns, "degraded_too_long");
            } else if (!any_bad && clean_for_ns >= kRecoverWindowNs) {
                transitionTo(HealthState::Running, now_ns, "recovered");
            }
            break;

        case HealthState::Failed:
            if (!any_bad && clean_for_ns >= kRecoverWindowNs) {
                transitionTo(HealthState::Degraded, now_ns, "recovering");
            }
            break;
    }
}
