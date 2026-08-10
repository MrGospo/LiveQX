#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <spdlog/logger.h>
#include "core/AudioFrame.h"
#include "core/Frame.h"

class IClip {
public:
    virtual ~IClip() = default;

    virtual Frame getFrame() = 0;
    virtual AudioFrame getAudio(int num_samples) = 0;
    virtual double getDuration() const = 0;
    virtual bool hasAudio() const = 0;
    virtual bool isPrepared() const = 0;
    virtual void prepare() = 0;
    virtual void release() = 0;

    // ── Broadcast model ──────────────────────────────────────────────────────
    // During a transition window the outgoing clip is rendered via these
    // tail-mode accessors. They must NEVER advance the clip's internal
    // playback position — semantically they expose a "frozen" snapshot of
    // the last valid output the clip produced.
    //
    //   getTailFrame()  — typically the last decoded video frame, frozen.
    //   getTailAudio()  — silence (or fade-out of last decoded audio).
    //
    // reset() returns the clip to t=0 (head). Called by Preloader when the
    // timeline is about to enter the clip again on a new playlist cycle.
    virtual Frame      getTailFrame() = 0;
    virtual AudioFrame getTailAudio(int num_samples) = 0;
    virtual void       reset() = 0;

    // Freezes the tail snapshot. While true, getFrame() must not overwrite the
    // tail with newly served frames. Used for single-clip transition loops:
    // the same VideoClip serves as both clipA (frozen tail) and clipB (live
    // decoder from t=0) inside one transition window. Default is no-op.
    virtual void       setFreezeTail(bool /*freeze*/) {}

    // Configures pre-decoded head buffer length. MUST be called before
    // prepare() — the buffer is captured during prepare(). Default 0 disables.
    virtual void       setHeadBufferSeconds(double /*sec*/) {}

    // Switches getFrame()/getAudio() to serve from the pre-decoded head
    // buffer instead of the live decoder. Cursor resets to 0 on enter.
    // Used together with setFreezeTail(true) in the single-clip self-loop:
    // clipA renders frozen tail, clipB pulls fresh frames from head buffer
    // while the live decoder is independently rewound for the next cycle.
    virtual void       setHeadPlayback(bool /*on*/) {}

    // Per-channel logger / id tag. Default no-ops; VideoClip overrides.
    virtual void setLogger(std::shared_ptr<spdlog::logger> /*lg*/) {}
    virtual void setChannelId(std::string /*id*/) {}

    // Clip-type tag for proof-of-play logging: "image" | "video" | "fallback".
    // Default "unknown" so a forgotten override never crashes the logger.
    virtual std::string clipType() const noexcept { return "unknown"; }

    // Lookahead window before the slot boundary at which Timeline must
    // notify the channel that this clip is becoming the next active —
    // gives a slow source (RTMP handshake, multicast IGMP join) a head
    // start so the cut isn't dead air. Default 0 means "no warm-up
    // needed"; ImageClip / VideoClip keep that default. LiveClip
    // returns its configured warm_up_sec so Timeline fires the prepare
    // callback at the right moment.
    virtual double warmUpSec() const noexcept { return 0.0; }

    // Called once per render-loop tick with a wall-clock now. Default
    // no-op; LiveClip overrides to drive its Idle/WarmingUp/Live/Lost/
    // Finished state machine without owning a clock of its own.
    virtual void onTick(std::uint64_t /*now_ns*/) noexcept {}
};
