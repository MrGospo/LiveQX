#pragma once
#include <string>
#include "clips/IClip.h"

class FallbackClip : public IClip {
public:
    explicit FallbackClip(const std::string& image_path,
                          int out_width = 1280, int out_height = 720);

    Frame getFrame() override;
    AudioFrame getAudio(int num_samples) override;
    double getDuration() const override;  // returns infinity
    bool hasAudio()     const override;
    bool isPrepared()   const override;
    void prepare()            override;
    void release()            override;

    Frame      getTailFrame()                 override;
    AudioFrame getTailAudio(int num_samples)  override;
    void       reset()                        override;

    std::string clipType() const noexcept override { return "fallback"; }

private:
    std::string image_path_;
    int out_width_;
    int out_height_;
    Frame cached_frame_;
};
