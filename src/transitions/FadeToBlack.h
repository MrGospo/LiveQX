#pragma once
#include "transitions/ITransition.h"

// fix13 c7 — fade-to-black transition.
//
// Designed for cuts at live-clip boundaries: even when the LiveClip's
// OnLossProvider keeps producing fallback frames, a feed switch is
// what broadcast operators expect to see go through black. The
// transition splits its window in two:
//
//   progress ∈ [0.0, 0.5]  →  A fades to black     (alpha 1.0 → 0.0)
//   progress ∈ [0.5, 1.0]  →  B fades from black   (alpha 0.0 → 1.0)
//
// Both halves run linearly. CrossFade-shaped pacing (i.e. ease) is
// out of scope — channels can still pick a different TransitionType
// for non-live cuts.
class FadeToBlack : public ITransition {
public:
    void apply(const Frame& a, const Frame& b,
               Frame& out, float progress) override;
};
