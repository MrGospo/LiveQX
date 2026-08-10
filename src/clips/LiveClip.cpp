#include "clips/LiveClip.h"

#include <stdexcept>
#include <utility>

#include <spdlog/spdlog.h>

namespace liveqx {

const char* toString(LiveClip::State s) noexcept {
    switch (s) {
        case LiveClip::State::Idle:       return "Idle";
        case LiveClip::State::WarmingUp:  return "WarmingUp";
        case LiveClip::State::Live:       return "Live";
        case LiveClip::State::Lost:       return "Lost";
        case LiveClip::State::Finished:   return "Finished";
    }
    return "Unknown";
}

LiveClip::LiveClip(Cfg cfg, std::unique_ptr<LiveInputClip> input)
    : cfg_(std::move(cfg)), input_(std::move(input)) {
    if (!input_) {
        throw std::invalid_argument("LiveClip: input must be non-null");
    }
    if (cfg_.duration_ns == 0) {
        throw std::invalid_argument("LiveClip: duration_ns must be > 0");
    }
}

LiveClip::~LiveClip() = default;

double LiveClip::getDuration() const {
    return static_cast<double>(cfg_.duration_ns) / 1e9;
}

bool LiveClip::hasAudio() const {
    return input_ && input_->hasAudio();
}

bool LiveClip::isPrepared() const {
    return prepared_.load(std::memory_order_acquire);
}

void LiveClip::prepare() {
    if (prepared_.exchange(true, std::memory_order_acq_rel)) {
        return;  // idempotent — warm-up window may overlap explicit prepare()
    }
    input_->prepare();
}

void LiveClip::release() {
    if (!prepared_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    input_->release();
}

Frame LiveClip::getFrame() {
    if (state_.load(std::memory_order_acquire) == State::Live) {
        return input_->getFrame();
    }
    // Outside Live: ask the channel-supplied OnLossProvider for the
    // configured fallback. Snapshot the function under loss_mu_ — REST
    // PATCH may swap the provider concurrently — and call it after
    // releasing the lock so a slow provider doesn't block PATCH.
    std::function<Frame()> video;
    {
        std::lock_guard<std::mutex> lk(loss_mu_);
        video = loss_provider_.video;
    }
    if (video) return video();
    return Frame{};
}

AudioFrame LiveClip::getAudio(int num_samples) {
    if (state_.load(std::memory_order_acquire) == State::Live) {
        return input_->getAudio(num_samples);
    }
    std::function<AudioFrame(int)> audio;
    {
        std::lock_guard<std::mutex> lk(loss_mu_);
        audio = loss_provider_.audio;
    }
    if (audio) return audio(num_samples);
    return input_->getTailAudio(num_samples);  // silence default
}

void LiveClip::setOnLossProvider(OnLossProvider provider) {
    std::lock_guard<std::mutex> lk(loss_mu_);
    loss_provider_ = std::move(provider);
}

Frame LiveClip::getTailFrame() {
    // fix13 c10 — during a transition the outgoing or incoming live clip
    // is rendered via getTailFrame. While the clip is Live we want the
    // upstream's last decoded frame (the natural freeze). Outside Live —
    // Idle/WarmingUp before the cut, Lost mid-stream, Finished on the
    // way out — we want the OnLossProvider's frame so the crossfade
    // shows the configured fallback rather than an empty/stale tail.
    if (state_.load(std::memory_order_acquire) == State::Live) {
        return input_->getTailFrame();
    }
    std::function<Frame()> video;
    {
        std::lock_guard<std::mutex> lk(loss_mu_);
        video = loss_provider_.video;
    }
    if (video) return video();
    return input_->getTailFrame();
}

AudioFrame LiveClip::getTailAudio(int num_samples) {
    // Tail audio: silence in non-Live states is still silence — the
    // input's getTailAudio also returns silence — so the provider only
    // matters when we want the configured FallbackClip's audio. Mirror
    // getTailFrame for symmetry; falls through to silence otherwise.
    if (state_.load(std::memory_order_acquire) == State::Live) {
        return input_->getTailAudio(num_samples);
    }
    std::function<AudioFrame(int)> audio;
    {
        std::lock_guard<std::mutex> lk(loss_mu_);
        audio = loss_provider_.audio;
    }
    if (audio) return audio(num_samples);
    return input_->getTailAudio(num_samples);
}

void       LiveClip::reset()                       { /* live: no rewind */ }

void LiveClip::setLogger(std::shared_ptr<spdlog::logger> lg) {
    {
        std::lock_guard<std::mutex> lk(logger_mu_);
        logger_ = lg;
    }
    if (input_) input_->setLogger(std::move(lg));
}

void LiveClip::setChannelId(std::string id) {
    channel_id_ = id;
    if (input_) input_->setChannelId(std::move(id));
}

void LiveClip::setScheduledStart(std::uint64_t start_ns) noexcept {
    scheduled_start_ns_.store(start_ns, std::memory_order_release);
}

bool LiveClip::isFinished(std::uint64_t now_ns) const noexcept {
    const auto start = scheduled_start_ns_.load(std::memory_order_acquire);
    if (start == 0) return false;
    return now_ns >= start + cfg_.duration_ns;
}

void LiveClip::transitionTo(State next, std::uint64_t now_ns) {
    const auto prev = state_.exchange(next, std::memory_order_acq_rel);
    if (prev == next) return;

    std::shared_ptr<spdlog::logger> lg;
    {
        std::lock_guard<std::mutex> lk(logger_mu_);
        lg = logger_;
    }
    auto& l = lg ? *lg : *spdlog::default_logger();
    l.info("[live id={}] {} -> {} at t={}ns",
           cfg_.id.empty() ? "?" : cfg_.id,
           toString(prev), toString(next), now_ns);
}

void LiveClip::onTick(std::uint64_t now_ns) noexcept {
    // Defensively swallow every exception — the render loop's IClip::onTick
    // contract is noexcept (so a misbehaving input can never abort()).
    try {
        const auto state = state_.load(std::memory_order_acquire);
        if (state == State::Finished) return;

        const auto start = scheduled_start_ns_.load(std::memory_order_acquire);
        if (start == 0) return;  // nothing scheduled yet

        // Hard finish overrides every other transition — schedule precision
        // (and the next clip cueing on the boundary) wins over signal state.
        if (now_ns >= start + cfg_.duration_ns) {
            if (prepared_.load(std::memory_order_acquire)) {
                release();
            }
            transitionTo(State::Finished, now_ns);
            return;
        }

        switch (state) {
            case State::Idle: {
                const auto warm_at = start > cfg_.warm_up_ns
                                   ? start - cfg_.warm_up_ns
                                   : std::uint64_t{0};
                if (now_ns >= warm_at) {
                    transitionTo(State::WarmingUp, now_ns);
                    if (!prepared_.load(std::memory_order_acquire)) {
                        prepare();
                    }
                }
                break;
            }
            case State::WarmingUp: {
                if (now_ns >= start) {
                    const bool healthy = input_->isHealthy();
                    transitionTo(healthy ? State::Live : State::Lost, now_ns);
                    if (!healthy) {
                        loss_detected_at_.store(now_ns, std::memory_order_release);
                        loss_count_.fetch_add(1, std::memory_order_acq_rel);
                    }
                }
                break;
            }
            case State::Live: {
                const auto last_pkt = static_cast<std::uint64_t>(input_->lastPacketNs());
                const bool silent = (last_pkt == 0)
                                  || (now_ns > last_pkt
                                      && now_ns - last_pkt > cfg_.loss_threshold_ns);
                if (silent || !input_->isHealthy()) {
                    loss_detected_at_.store(now_ns, std::memory_order_release);
                    loss_count_.fetch_add(1, std::memory_order_acq_rel);
                    transitionTo(State::Lost, now_ns);
                }
                break;
            }
            case State::Lost: {
                if (input_->isHealthy()) {
                    recovered_count_.fetch_add(1, std::memory_order_acq_rel);
                    transitionTo(State::Live, now_ns);
                }
                break;
            }
            default: break;
        }
    } catch (...) {
        // No-op: the render loop must keep producing frames even if a
        // live input throws during prepare/release. Subsequent ticks
        // will retry; a permanently-failing input stays in Lost and the
        // OnLossProvider keeps the channel on air.
    }
}

nlohmann::json LiveClip::statusJson() const {
    const auto last_pkt_ns = input_ ? input_->lastPacketNs() : 0;
    nlohmann::json j = {
        {"id",                  cfg_.id},
        {"state",               toString(state_.load(std::memory_order_acquire))},
        {"loss_count",          loss_count_.load(std::memory_order_acquire)},
        {"recovered_count",     recovered_count_.load(std::memory_order_acquire)},
        {"loss_detected_at_ns", loss_detected_at_.load(std::memory_order_acquire)},
        {"last_packet_ns",      last_pkt_ns},
        {"duration_ns",         cfg_.duration_ns},
        {"warm_up_ns",          cfg_.warm_up_ns},
        {"loss_threshold_ns",   cfg_.loss_threshold_ns},
    };
    if (input_) {
        j["input"] = input_->statusJson();
    }
    return j;
}

} // namespace liveqx
