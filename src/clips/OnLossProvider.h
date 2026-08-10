#pragma once
#include <functional>
#include <memory>
#include <string>

#include "clips/IClip.h"
#include "clips/LiveClip.h"

namespace liveqx {

// fix13 c6 — channel-side helper that turns a "fallback_on_loss" mode
// string into the LiveClip::OnLossProvider the clip will call whenever
// it is not in the Live state.
//
// Three modes are supported, matching the playlist entry config:
//   "fallback_clip" — pull frames from the channel-level FallbackClip
//                     (a still image or a loop). Requires sources.fallback_clip.
//   "freeze"        — render the upstream's last decoded frame, frozen.
//                     The channel binds sources.tail_video to LiveClip::
//                     getTailFrame() (which forwards to the underlying input)
//                     and audio is silence — actually freezing audio just
//                     glitches, so we don't bother.
//   "black"         — solid black at sources.black_width × sources.black_height
//                     plus silence.
//
// Unknown modes throw std::invalid_argument; required-but-missing
// sources also throw, so a misconfigured channel fails loudly at
// build time rather than producing invalid frames at airtime.
struct OnLossSources {
    std::shared_ptr<IClip>           fallback_clip;   // for "fallback_clip"
    std::function<Frame()>           tail_video;      // for "freeze"
    int                              black_width  = 1280;
    int                              black_height = 720;
};

LiveClip::OnLossProvider makeOnLossProvider(const std::string& mode,
                                            OnLossSources sources);

} // namespace liveqx
