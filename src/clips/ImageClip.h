#pragma once
#include <string>
#include "clips/IClip.h"

class ImageClip : public IClip {
public:
    ImageClip(const std::string& path, double duration_sec,
              int out_width, int out_height);

    Frame getFrame() override;
    AudioFrame getAudio(int num_samples) override;
    double getDuration() const override;
    bool hasAudio()     const override;
    bool isPrepared()   const override;
    void prepare()            override;
    void release()            override;

    Frame      getTailFrame()                 override;
    AudioFrame getTailAudio(int num_samples)  override;
    void       reset()                        override;

    std::string clipType() const noexcept override { return "image"; }

private:
    std::string path_;
    double duration_sec_;
    int out_width_;
    int out_height_;
    Frame cached_frame_;
};
