#pragma once
#include "transitions/ITransition.h"

// fix20 — Push transition. Both clips slide in lockstep by `progress*D`
// pixels (D = width for left/right, height for up/down). Distinct from
// Wipe: there is no pixel reveal, both halves of the output are real
// content. Direction is set once in the ctor; CpuCompositor keeps four
// instances (one per direction).
class Push : public ITransition {
public:
    explicit Push(TransitionType direction);

    void apply(const Frame& a, const Frame& b,
               Frame& out, float progress) override;

private:
    TransitionType direction_;
};
