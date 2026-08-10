#pragma once
#include "transitions/ITransition.h"

class Dissolve : public ITransition {
public:
    void apply(const Frame& a, const Frame& b,
               Frame& out, float progress) override;
};
