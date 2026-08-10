#include "render/CpuCompositor.h"
#include <memory>

ITransition* CpuCompositor::pick(TransitionType type) noexcept {
    switch (type) {
    case TransitionType::CrossFade:   return &crossfade_;
    case TransitionType::Dissolve:    return &dissolve_;
    case TransitionType::FadeToBlack: return &fade_black_;
    case TransitionType::WipeLeft:    return &wipe_left_;
    case TransitionType::WipeRight:  return &wipe_right_;
    case TransitionType::WipeUp:     return &wipe_up_;
    case TransitionType::WipeDown:   return &wipe_down_;
    case TransitionType::PushLeft:   return &push_left_;
    case TransitionType::PushRight:  return &push_right_;
    case TransitionType::PushUp:     return &push_up_;
    case TransitionType::PushDown:   return &push_down_;
    default:                         return nullptr;
    }
}

Frame CpuCompositor::composite(const Frame& a, const Frame& b,
                                TransitionType type, float progress,
                                Easing easing) {
    // fix20: shape `progress` through the easing curve before any other
    // logic. Linear is identity (existing behaviour); the other curves
    // preserve endpoints (0→0, 1→1) so the early-return short-circuits
    // below remain correct.
    progress = applyEasing(easing, progress);

    // FadeToBlack synthesises the black mid-frame internally, so it
    // must be allowed to run even when one side is invalid (e.g.
    // LiveClip Lost with no fallback). All other transitions need
    // valid pixels on both sides.
    const bool is_fade_to_black = (type == TransitionType::FadeToBlack);
    if (!a.valid() && !is_fade_to_black) return b;
    if (!b.valid() && !is_fade_to_black) return a;
    if (type == TransitionType::HardCut || progress <= 0.0f) return a.valid() ? a : b;
    if (progress >= 1.0f) return b.valid() ? b : a;

    ITransition* tr = pick(type);
    if (!tr) return a;

    Frame out;
    if (render_pool_) out = render_pool_->acquire();
    if (!out.valid()) {
        const Frame& shape = a.valid() ? a : b;
        if (!shape.valid()) return Frame{};
        out.data = std::make_shared<uint8_t[]>(shape.sizeBytes());
        out.width  = shape.width;
        out.height = shape.height;
    }
    out.pts = a.valid() ? a.pts : b.pts;

    tr->apply(a, b, out, progress);
    return out;
}
