#pragma once
#include <memory>
#include "clips/IClip.h"
#include "clips/PlaylistItem.h"
#include "core/FramePool.h"
#include "metrics/ChannelMetrics.h"

// Single point of truth for file-type detection.
// RenderLoop and Timeline must never include ImageClip/VideoClip directly —
// they receive IClip* from here.
class ClipFactory {
public:
    static std::unique_ptr<IClip> create(const PlaylistItem& item,
                                         int out_w, int out_h,
                                         std::shared_ptr<FramePool> pool = nullptr,
                                         std::shared_ptr<ChannelMetrics> metrics = nullptr);

private:
    static bool isImage(const std::string& path);
    static bool isVideo(const std::string& path);
};
