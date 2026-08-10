#pragma once
#include "core/FramePool.h"
#include "render/ICompositor.h"
#include "transitions/CrossFade.h"
#include "transitions/Dissolve.h"
#include "transitions/FadeToBlack.h"
#include "transitions/Push.h"
#include "transitions/Wipe.h"

class CpuCompositor : public ICompositor {
public:
    CpuCompositor() = default;
    explicit CpuCompositor(FramePool& render_pool) : render_pool_(&render_pool) {}

    Frame composite(const Frame& a, const Frame& b,
                    TransitionType type, float progress,
                    Easing easing = Easing::Linear) override;

private:
    ITransition* pick(TransitionType type) noexcept;

    FramePool* render_pool_ = nullptr;

    CrossFade   crossfade_;
    Dissolve    dissolve_;
    FadeToBlack fade_black_;
    Wipe        wipe_left_  { TransitionType::WipeLeft  };
    Wipe        wipe_right_ { TransitionType::WipeRight };
    Wipe        wipe_up_    { TransitionType::WipeUp    };
    Wipe        wipe_down_  { TransitionType::WipeDown  };
    Push        push_left_  { TransitionType::PushLeft  };
    Push        push_right_ { TransitionType::PushRight };
    Push        push_up_    { TransitionType::PushUp    };
    Push        push_down_  { TransitionType::PushDown  };
};
