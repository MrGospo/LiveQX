#include "clips/ClipFactory.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>

#include "clips/FallbackClip.h"
#include "clips/ImageClip.h"
#include "clips/MediaValidator.h"
#include "clips/VideoClip.h"
#include "utils/Log.h"

namespace {

const char* kImageExts[] = { ".jpg", ".jpeg", ".png", ".bmp", ".webp" };
const char* kVideoExts[] = { ".mp4", ".avi",  ".mkv", ".mov", ".ts"   };

std::string ext(const std::string& path) {
    auto pos = path.rfind('.');
    if (pos == std::string::npos) return {};
    std::string e = path.substr(pos);
    std::transform(e.begin(), e.end(), e.begin(), ::tolower);
    return e;
}

bool matchExt(const std::string& path, const char** exts, size_t n) {
    const auto e = ext(path);
    for (size_t i = 0; i < n; ++i)
        if (e == exts[i]) return true;
    return false;
}

} // namespace

bool ClipFactory::isImage(const std::string& path) {
    return matchExt(path, kImageExts, std::size(kImageExts));
}

bool ClipFactory::isVideo(const std::string& path) {
    return matchExt(path, kVideoExts, std::size(kVideoExts));
}

std::unique_ptr<IClip> ClipFactory::create(const PlaylistItem& item,
                                            int out_w, int out_h,
                                            std::shared_ptr<FramePool> pool,
                                            std::shared_ptr<ChannelMetrics> metrics) {
    if (isImage(item.path))
        return std::make_unique<ImageClip>(
            item.path, item.display_duration, out_w, out_h);

    if (isVideo(item.path)) {
        const auto vr = MediaValidator::validate(item.path);
        if (!vr.valid) {
            LOG_ERROR("ClipFactory: skipping '{}': {}", item.path, vr.error);
            return nullptr;
        }
        auto clip = std::make_unique<VideoClip>(item.path, out_w, out_h, std::move(pool));
        if (metrics) clip->setMetrics(std::move(metrics));
        clip->setNumaNode(item.numa_node);
        return clip;
    }

    throw std::runtime_error(
        "ClipFactory: unsupported file type '" + item.path + "'");
}
