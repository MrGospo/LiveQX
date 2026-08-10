#include "clips/OnLossProvider.h"

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace liveqx {

namespace {

AudioFrame silentAudio(int num_samples) {
    AudioFrame af;
    af.sample_rate  = 48000;
    af.channels     = 2;
    af.num_samples  = num_samples > 0 ? num_samples : 0;
    af.samples.assign(static_cast<size_t>(af.num_samples) * 2, 0.0f);
    af.valid = true;
    return af;
}

Frame blackFrame(int w, int h) {
    Frame f;
    f.width  = w;
    f.height = h;
    const size_t n = static_cast<size_t>(w) * h * 4;
    f.data = std::shared_ptr<uint8_t[]>(new uint8_t[n]());
    // RGBA zero == opaque-zero; alpha=0 is fine for compositor (background).
    // Compositor blits behind background fill, so leave A=0 too.
    return f;
}

} // namespace

LiveClip::OnLossProvider makeOnLossProvider(const std::string& mode,
                                            OnLossSources sources) {
    LiveClip::OnLossProvider out;

    if (mode == "fallback_clip") {
        if (!sources.fallback_clip) {
            throw std::invalid_argument(
                "makeOnLossProvider: mode=fallback_clip requires fallback_clip");
        }
        auto clip = sources.fallback_clip;
        out.video = [clip]() { return clip->getFrame(); };
        out.audio = [clip](int n) { return clip->getAudio(n); };
        return out;
    }

    if (mode == "freeze") {
        if (!sources.tail_video) {
            throw std::invalid_argument(
                "makeOnLossProvider: mode=freeze requires tail_video");
        }
        auto tail = std::move(sources.tail_video);
        out.video = [tail]() { return tail(); };
        // Audio: silence. Freezing the last buffer would tile a 20ms loop
        // and produce an audible artifact; silence is the conservative call.
        out.audio = [](int n) { return silentAudio(n); };
        return out;
    }

    if (mode == "black") {
        const int w = sources.black_width  > 0 ? sources.black_width  : 1280;
        const int h = sources.black_height > 0 ? sources.black_height : 720;
        // Cache one black frame and hand out copies — Frame is shared-buffer.
        auto cached = std::make_shared<Frame>(blackFrame(w, h));
        out.video = [cached]() { return *cached; };
        out.audio = [](int n) { return silentAudio(n); };
        return out;
    }

    throw std::invalid_argument(
        "makeOnLossProvider: unknown mode \"" + mode +
        "\" (expected fallback_clip|freeze|black)");
}

} // namespace liveqx
