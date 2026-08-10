#pragma once
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>
#include <spdlog/logger.h>

#include "clips/IClip.h"
#include "core/AudioFrame.h"
#include "core/Frame.h"

// fix10: base class for live (network-driven) sources — multicast input,
// future RTMP/RTSP/SDI ingest. They share the same shape:
//   - infinite duration: Timeline never crosses a "natural boundary";
//     transitions only happen via skipToNext / schedule swap.
//   - no tail mode: a frozen frame from a live source quickly grows
//     stale; getTailFrame returns the most recently decoded frame
//     instead of a captured snapshot.
//   - reset() is a no-op: re-entering a live clip just keeps consuming
//     the network stream — there is nothing to rewind.
//
// fix13 c1 formalizes the live-input surface: every concrete input
// (MulticastInput, RtmpInput, future RtspInput) must answer
// isHealthy()/lastPacketNs()/statusJson()/setNumaNode(). LiveClip
// (fix13 c3) drives its state machine — Idle → WarmingUp → Live → Lost
// — off this surface, and ILiveInputFactory (c2) dispatches by cfg type
// to a concrete LiveInputClip. Subclasses still override
// prepare()/release()/getFrame()/getAudio()/hasAudio()/isPrepared().
class LiveInputClip : public IClip {
public:
    // Every live source by definition has no fixed length. Timeline
    // checks isinf() to skip "boundary by duration" handling.
    double getDuration() const override final { return std::numeric_limits<double>::infinity(); }

    // Tail accessors echo the most recent decoded frame/audio. Concrete
    // implementations update last_frame_ / last_audio_ from their
    // decode threads so the broadcast crossfade has *something* to fade
    // to even on an unstable feed.
    Frame      getTailFrame() override                  { return last_frame_; }
    AudioFrame getTailAudio(int num_samples) override {
        AudioFrame silence;
        silence.num_samples = num_samples;
        silence.samples.assign(static_cast<size_t>(num_samples) * silence.channels, 0.0f);
        silence.valid = true;
        return silence;
    }

    void reset() override { /* live sources have nothing to rewind */ }

    void setLogger(std::shared_ptr<spdlog::logger> lg) override { logger_ = std::move(lg); }
    void setChannelId(std::string id) override                  { channel_id_ = std::move(id); }

    std::string clipType() const noexcept override { return "live"; }

    // ---- fix13 c1: live-input surface ----
    // True iff the source is currently producing fresh packets. Concrete
    // inputs combine their own readiness flags (prepared / connected /
    // stalled) into a single answer. LiveClip uses this to flip into
    // Lost state when the upstream goes silent.
    virtual bool isHealthy() const = 0;

    // Monotonic ns timestamp of the most recent successful packet read.
    // Returns 0 before the first packet. LiveClip computes a packet-age
    // gauge off this for the loss-threshold heuristic and surfaces it in
    // /live-status.
    virtual std::int64_t lastPacketNs() const noexcept = 0;

    // NUMA pin for decode/watchdog threads. -1 = OS scheduler.
    // ChannelInstance calls this right after construction so the input's
    // threads are pinned to the channel's node before prepare() spawns
    // them. Concrete inputs that don't yet honor NUMA may store and
    // ignore the value.
    virtual void setNumaNode(int node) noexcept = 0;

    // Diagnostics surfaced via REST /live-status (fix13 c8). Each
    // concrete input adds its transport-specific fields (e.g. RTMP
    // sanitizes the URL); the common ones — packets_recv, reconnect_count,
    // stalled, last_packet_age_ms — should always be present.
    virtual nlohmann::json statusJson() const = 0;

protected:
    // Subclasses store the most recently decoded frame here so the
    // tail-mode accessors can serve it without holding a decoder lock.
    Frame                              last_frame_;
    std::shared_ptr<spdlog::logger>    logger_;
    std::string                        channel_id_;
};
