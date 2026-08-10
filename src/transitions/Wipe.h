#pragma once
#include "transitions/ITransition.h"

class Wipe : public ITransition {
public:
    explicit Wipe(TransitionType direction);

    void apply(const Frame& a, const Frame& b,
               Frame& out, float progress) override;

private:
    TransitionType direction_;
};
