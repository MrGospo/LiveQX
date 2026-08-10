#pragma once
#include "core/Frame.h"
#include "transitions/ITransition.h"

class ICompositor {
public:
    virtual ~ICompositor() = default;

    virtual Frame composite(
        const Frame& a, const Frame& b,
        TransitionType type,
        float progress,                 // 0.0 = fully A, 1.0 = fully B
        Easing easing = Easing::Linear  // fix20: applied before transition
    ) = 0;
};
