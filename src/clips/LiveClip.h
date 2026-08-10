#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include <nlohmann/json.hpp>
#include <spdlog/logger.h>

#include "clips/IClip.h"
#include "clips/LiveInputClip.h"

namespace liveqx {

// fix13 c3 — IClip adapter that drives a LiveInputClip through a
// scheduled lifecycle (Idle → WarmingUp → Live → Lost → Finished).
//
// LiveClip is the playlist-facing surface for a "type": "live" entry:
// Timeline still treats it as an IClip with a finite duration, but
// behind the scenes the clip
//   - opens the underlying input warm_up_ns BEFORE its scheduled start
//     so a slow RTMP handshake / multicast join doesn't stall the cut,
//   - flips into Lost state when the upstream goes silent for longer
//     than loss_threshold_ns and back into Live on the first fresh
//     packet (the input's pump thread keeps trying — recovery is
//     automatic),
//   - hard-finishes at scheduled_start + duration even if the upstream
//     is still streaming, preserving schedule precision.
//
// The state machine is driven by external onTick(now_ns) calls (the
// render loop owns the wall clock); tests pin every transition this
// way without touching std::chrono.
//
// While in Live, getFrame()/getAudio() forward to the input. Outside
// Live (Idle / WarmingUp / Lost / Finished) the clip consults its
// OnLossProvider — a pair of std::function the channel injects to
// supply the configured fallback for that channel: pull a frame
// from the channel-level fallback clip, freeze on the upstream's
// last decoded frame, or paint solid black. Without a provider the
// clip returns an invalid Frame / silence (caller is expected to
// notice and substitute its own black).
class LiveClip : public IClip {
public:
    enum class State { Idle, WarmingUp, Live, Lost, Finished };

    // fix13 c6 — channel-supplied fallback for non-Live frames.
    // Either function may be null; LiveClip falls back to an invalid
    // Frame (video) or silence (audio) when the provider is absent.
    struct OnLossProvider {
        std::function<Frame()>          video;
        std::function<AudioFrame(int)>  audio;
    };

    struct Cfg {
        std::string   id;
        std::uint64_t duration_ns       = 0;
        std::uint64_t warm_up_ns        = 5'000'000'000ULL;     // 5s
        std::uint64_t loss_threshold_ns = 2'000'000'000ULL;     // 2s
    };

    LiveClip(Cfg cfg, std::unique_ptr<LiveInputClip> input);
    ~LiveClip() override;

    // ── IClip ───────────────────────────────────────────────────────
    Frame      getFrame()                  override;
    AudioFrame getAudio(int num_samples)   override;
    double     getDuration()        const override;
    bool       hasAudio()           const override;
    bool       isPrepared()         const override;
    void       prepare()                  override;
    void       release()                  override;

    Frame      getTailFrame()                  override;
    AudioFrame getTailAudio(int num_samples)   override;
    void       reset()                         override;

    void        setLogger(std::shared_ptr<spdlog::logger> lg) override;
    void        setChannelId(std::string id) override;
    std::string clipType() const noexcept override { return "live"; }
    double      warmUpSec() const noexcept override {
        return static_cast<double>(cfg_.warm_up_ns) / 1e9;
    }

    // ── State machine ──────────────────────────────────────────────
    // Pin the planned start instant. Idempotent — a later REST PATCH
    // that shifts the schedule simply replaces it; onTick re-evaluates
    // against the new value on the next call.
    void  setScheduledStart(std::uint64_t start_ns) noexcept;

    // Advance the state machine against a wall clock the caller owns.
    // Called from the render loop / Timeline; cheap on the hot path
    // (single atomic load + a few comparisons in the steady state).
    void  onTick(std::uint64_t now_ns) noexcept override;

    State state() const noexcept { return state_.load(std::memory_order_acquire); }
    bool  isFinished(std::uint64_t now_ns) const noexcept;

    const std::string& id()  const noexcept { return cfg_.id; }
    const Cfg&         cfg() const noexcept { return cfg_; }

    // /live-status payload — surfaced by REST (c8) and useful in tests.
    // Wraps the underlying input's statusJson() under "input".
    nlohmann::json statusJson() const;

    // Install the fallback to use whenever the clip is not Live. Hot
    // path on the render thread reads the provider under a brief
    // mutex so a REST PATCH that swaps the channel fallback doesn't
    // race the decode loop.
    void setOnLossProvider(OnLossProvider provider);

private:
    void transitionTo(State next, std::uint64_t now_ns);

    Cfg                            cfg_;
    std::unique_ptr<LiveInputClip> input_;
    std::atomic<State>             state_{State::Idle};
    std::atomic<std::uint64_t>     scheduled_start_ns_{0};
    std::atomic<std::uint64_t>     loss_detected_at_{0};
    std::atomic<std::uint32_t>     loss_count_{0};
    std::atomic<std::uint32_t>     recovered_count_{0};
    std::atomic<bool>              prepared_{false};

    mutable std::mutex              logger_mu_;
    std::shared_ptr<spdlog::logger> logger_;
    std::string                     channel_id_;

    mutable std::mutex              loss_mu_;
    OnLossProvider                  loss_provider_;
};

const char* toString(LiveClip::State s) noexcept;

} // namespace liveqx
