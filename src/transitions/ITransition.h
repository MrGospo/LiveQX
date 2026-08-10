#pragma once
#include "core/Frame.h"

enum class TransitionType {
    CrossFade,
    WipeLeft,
    WipeRight,
    WipeUp,
    WipeDown,
    PushLeft,    // fix20: A slides out left, B slides in from right
    PushRight,   // fix20: A slides out right, B slides in from left
    PushUp,      // fix20: A slides out up, B slides in from bottom
    PushDown,    // fix20: A slides out down, B slides in from top
    Dissolve,
    FadeToBlack,
    HardCut,
};

// Broadcast-модель: семантика стыковки клипов (отдельно от визуального типа).
//   HardCut    — мгновенный switch, без окна перехода. duration_sec игнорируется.
//   FreezeFade — оба клипа заморожены в окне перехода (A — последний кадр,
//                B — первый кадр-seed). Контент каждого клипа играется 100%
//                в собственном content_phase. Timeline удлиняется на duration_sec.
//   LiveMix    — A заморожен, B играет вживую с t=0 в окне перехода и
//                непрерывно продолжает в свой content_phase. Slot[B] = duration
//                (без расширения), т.е. timeline = sum(durations).
enum class TransitionMode {
    HardCut,
    FreezeFade,
    LiveMix,
};

// fix20 — easing-кривая на progress. Применяется в CpuCompositor::composite()
// до вызова tr->apply(), общая для всех TransitionType. Default Linear =
// identity, поэтому существующее поведение и пиксельные тесты не меняются.
// Все четыре кривые сохраняют концевые точки (0→0, 1→1) и монотонны на [0,1].
enum class Easing {
    Linear,
    EaseIn,    // t² — резкий старт, мягкое прибытие
    EaseOut,   // 1-(1-t)² — мягкий старт, резкое прибытие
    EaseInOut, // 3t² - 2t³ — smooth-step
};

inline float applyEasing(Easing e, float t) noexcept {
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    switch (e) {
        case Easing::Linear:    return t;
        case Easing::EaseIn:    return t * t;
        case Easing::EaseOut:   { const float u = 1.0f - t; return 1.0f - u * u; }
        case Easing::EaseInOut: return t * t * (3.0f - 2.0f * t);
    }
    return t;
}

struct TransitionConfig {
    TransitionType type = TransitionType::CrossFade;
    TransitionMode mode = TransitionMode::FreezeFade;
    double duration_sec = 2.0;  // 0.0 = hard cut (mode форсируется в HardCut)
    Easing easing = Easing::Linear;  // fix20
};

class ITransition {
public:
    virtual ~ITransition() = default;

    virtual void apply(
        const Frame& a, const Frame& b,
        Frame& out, float progress  // 0.0 = fully A, 1.0 = fully B
    ) = 0;
};
