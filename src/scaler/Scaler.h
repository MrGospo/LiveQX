#pragma once
#include <memory>
#include <string>
#include "core/Frame.h"

enum class ScaleMode { Fit, Fill, Stretch };

ScaleMode scaleModeFromString(const std::string& s);

class Scaler {
public:
    Scaler(int out_width, int out_height, ScaleMode mode);
    ~Scaler();

    // Scale input RGBA frame to (out_width, out_height) using mode:
    //   Stretch — distort to fill exactly
    //   Fit     — letterbox/pillarbox with black padding, aspect preserved
    //   Fill    — crop to fill, aspect preserved, no black bars
    Frame scale(const Frame& input);

private:
    int       out_width_;
    int       out_height_;
    ScaleMode mode_;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};
