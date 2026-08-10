#include "gateway/ts/PcrRestamper.h"

namespace liveqx::gateway::ts {

namespace {

// Convert a byte offset at a given bitrate into 27-MHz PCR ticks.
// 8 bits/byte * 27_000_000 ticks/sec / bitrate_bps gives ticks per byte.
// We arrange the multiplication so a 64-bit numerator survives even at
// 100 Mbps and >2 hours of stream (~ 4.5e15 << 2^63).
std::uint64_t bytesToPcrTicks(std::uint64_t bytes,
                              std::uint32_t bitrate_bps) noexcept {
    if (bitrate_bps == 0) return 0;
    return (bytes * 8ull * 27'000'000ull) / static_cast<std::uint64_t>(bitrate_bps);
}

}  // namespace

void PcrRestamper::registerPid(std::uint16_t pcr_pid) noexcept {
    per_pid_.try_emplace(pcr_pid);
}

void PcrRestamper::unregisterPid(std::uint16_t pcr_pid) noexcept {
    per_pid_.erase(pcr_pid);
}

std::uint64_t PcrRestamper::currentPcr(std::uint16_t pid) const noexcept {
    auto it = per_pid_.find(pid);
    if (it == per_pid_.end() || !it->second.anchor_set) return 0;
    if (bitrate_bps_ == 0) return it->second.output_pcr_at_anchor;
    const std::uint64_t delta_bytes = bytes_emitted_ - it->second.bytes_at_anchor;
    return it->second.output_pcr_at_anchor + bytesToPcrTicks(delta_bytes, bitrate_bps_);
}

bool PcrRestamper::onPacketOut(TsPacketMut pkt, std::uint16_t pid) noexcept {
    bool rewritten = false;
    auto it = per_pid_.find(pid);
    if (it != per_pid_.end()) {
        const auto in_pcr = pkt.view().pcr27Mhz();
        if (in_pcr != TsPacketView::kPcrNone) {
            // Re-anchor on discontinuity (source signals stream restart) or
            // on the very first PCR we see for this PID.
            const bool disc = pkt.view().discontinuityIndicator();
            if (!it->second.anchor_set || disc) {
                it->second.anchor_set           = true;
                it->second.output_pcr_at_anchor = in_pcr;
                it->second.bytes_at_anchor      = bytes_emitted_;
                // Anchor packet keeps its input PCR — no rewrite needed.
                rewritten = true;                         // anchor counted as a "stamp" event
            } else if (bitrate_bps_ != 0) {
                const std::uint64_t delta_bytes = bytes_emitted_ - it->second.bytes_at_anchor;
                const std::uint64_t pcr_out =
                    it->second.output_pcr_at_anchor + bytesToPcrTicks(delta_bytes, bitrate_bps_);
                pkt.setPcr27Mhz(pcr_out);
                rewritten = true;
            }
            // bitrate=0: pass through the input PCR — same byte but no rewrite.
        }
    }
    bytes_emitted_ += kTsPacketSize;
    return rewritten;
}

}  // namespace liveqx::gateway::ts
