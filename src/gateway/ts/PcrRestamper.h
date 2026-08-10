#pragma once

#include <cstdint>
#include <span>
#include <unordered_map>

#include "gateway/ts/TsPacket.h"

namespace liveqx::gateway::ts {

// PCR restamping for CBR-paced output (fix40 A5).
//
// Receivers model the PCR clock as monotonic at the output transport rate.
// When we forward packets from a bursty input (multicast retransmits, jitter
// from upstream encoders) the *input* PCR values are correctly spaced for the
// input's clock — but on our output they arrive in tight bursts followed by
// gaps, which violates the receiver's PCR_jitter bound (500 ns end-to-end per
// ISO 13818-1 Annex E).
//
// PcrRestamper tracks two quantities:
//   1. bytes_emitted_ — running byte counter for everything sent on the wire,
//      including PSI, ES, and stuffing.
//   2. per-PID anchor — the first PCR seen on each PCR PID becomes the output
//      anchor; subsequent PCRs on that PID are rewritten as
//          pcr_out = output_anchor_pcr + (bytes_emitted_ - bytes_at_anchor)
//                    * 8 * 27_000_000 / bitrate_bps
//      so receivers see PCRs spaced exactly at the output transport rate.
//
// The discontinuity_indicator (AF flag bit 7) re-anchors the PID: the next
// input PCR becomes the new output anchor at the current byte position. This
// preserves stream restart semantics (signalled by the source) without
// pretending the gap didn't happen.
//
// bitrate_bps == 0 is a sentinel for "pass-through": currentPcr() returns the
// last seen input PCR unchanged, and onPacketOut() does not rewrite anything.
// This keeps PcrRestamper safe to instantiate even when the gateway has no
// CBR target configured.
//
// Threading: instance is owned by the IO thread; not safe for concurrent use.
class PcrRestamper {
public:
    explicit PcrRestamper(std::uint32_t bitrate_bps) noexcept
        : bitrate_bps_(bitrate_bps) {}

    void reset() noexcept {
        bytes_emitted_ = 0;
        per_pid_.clear();
    }

    void setBitrate(std::uint32_t bps) noexcept { bitrate_bps_ = bps; }
    std::uint32_t bitrate() const noexcept { return bitrate_bps_; }
    std::uint64_t bytesEmitted() const noexcept { return bytes_emitted_; }

    // Account for one outgoing 188-byte packet at PID `pid`. If `pid` is a
    // registered PCR PID and the packet carries PCR, the PCR is rewritten in
    // place (caller must hold a mutable reference). Always advances the byte
    // counter by kTsPacketSize.
    //
    // Returns true when a PCR was rewritten (or set as anchor). False when
    // the packet didn't carry PCR or the PID isn't tracked. Used by tests
    // to assert restamping happened on the expected packets.
    bool onPacketOut(TsPacketMut pkt, std::uint16_t pid) noexcept;

    // Mark `pid` as a PCR-carrying PID. Idempotent. Caller registers each
    // program's pcr_pid (after PID remap) at rebuildOutputTable() time.
    void registerPid(std::uint16_t pcr_pid) noexcept;
    void unregisterPid(std::uint16_t pcr_pid) noexcept;
    bool isTracked(std::uint16_t pcr_pid) const noexcept {
        return per_pid_.find(pcr_pid) != per_pid_.end();
    }

    // Compute the 27-MHz PCR value that *should* appear in a packet at PID
    // `pid` whose first byte begins at the current byte position. Only valid
    // when the PID is tracked and an anchor has been set; returns 0
    // otherwise. Exposed for unit tests.
    std::uint64_t currentPcr(std::uint16_t pid) const noexcept;

private:
    struct PidState {
        bool          anchor_set       = false;
        std::uint64_t output_pcr_at_anchor = 0;
        std::uint64_t bytes_at_anchor      = 0;
    };

    std::uint32_t                                 bitrate_bps_;
    std::uint64_t                                 bytes_emitted_ = 0;
    std::unordered_map<std::uint16_t, PidState>   per_pid_;
};

}  // namespace liveqx::gateway::ts
