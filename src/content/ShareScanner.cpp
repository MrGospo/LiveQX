#include "content/ShareScanner.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <system_error>

#include "utils/Log.h"

namespace fs = std::filesystem;

namespace {
constexpr std::array kExts{
    ".jpg", ".jpeg", ".png", ".bmp", ".webp",
    ".mp4", ".avi",  ".mkv", ".mov", ".ts",
};

std::string lowerExt(const fs::path& p) {
    std::string e = p.extension().string();
    std::transform(e.begin(), e.end(), e.begin(), ::tolower);
    return e;
}

std::int64_t mtimeNs(const fs::directory_entry& de, std::error_code& ec) {
    auto t = de.last_write_time(ec);
    if (ec) return 0;
    using namespace std::chrono;
    return duration_cast<nanoseconds>(t.time_since_epoch()).count();
}
}  // namespace

bool ShareScanner::isMediaFile(const fs::path& p) noexcept {
    const auto e = lowerExt(p);
    for (auto* x : kExts) if (e == x) return true;
    return false;
}

ShareScanner::ShareScanner(fs::path share_dir)
    : share_dir_(std::move(share_dir)) {}

std::size_t ShareScanner::stableCount() const noexcept {
    std::size_t n = 0;
    for (const auto& [_, e] : known_) if (e.stable) ++n;
    return n;
}

std::vector<std::string> ShareScanner::currentFilenames() const {
    std::vector<std::string> out;
    out.reserve(known_.size());
    for (const auto& [name, _] : known_) out.emplace_back(name);
    return out;
}

ShareDiff ShareScanner::scan() {
    ShareDiff diff;

    std::error_code ec;
    if (!fs::is_directory(share_dir_, ec) || ec) {
        diff.share_unreachable = true;
        return diff;
    }

    // Build current view from disk.
    std::unordered_map<std::string, Entry> seen;
    seen.reserve(known_.size() * 2);

    for (auto it = fs::directory_iterator(share_dir_, ec);
         !ec && it != fs::directory_iterator(); it.increment(ec)) {
        const auto& de = *it;
        if (!de.is_regular_file(ec) || ec) { ec.clear(); continue; }
        if (!isMediaFile(de.path()))        continue;

        Entry e;
        e.mtime_ns = mtimeNs(de, ec);
        if (ec) { ec.clear(); continue; }
        e.size     = de.file_size(ec);
        if (ec) { ec.clear(); continue; }
        seen.emplace(de.path().filename().string(), e);
    }
    if (ec) {
        // directory_iterator failed mid-walk — treat as transient.
        diff.share_unreachable = true;
        return diff;
    }

    // Diff vs previous state.
    for (auto& [name, cur] : seen) {
        auto prev_it = known_.find(name);
        if (prev_it == known_.end()) {
            // First sighting: not stable yet.
            cur.stable = false;
            continue;
        }
        const auto& prev = prev_it->second;
        const bool unchanged = (prev.mtime_ns == cur.mtime_ns) &&
                               (prev.size     == cur.size);
        if (unchanged) {
            if (prev.stable) {
                cur.stable = true;            // already exposed
            } else {
                cur.stable = true;            // promote
                diff.added.emplace_back(share_dir_ / name);
            }
        } else {
            // Still being written; reset stability, do not expose.
            cur.stable = false;
        }
    }

    for (const auto& [name, prev] : known_) {
        if (seen.find(name) == seen.end() && prev.stable) {
            diff.removed.emplace_back(name);
        }
    }

    known_ = std::move(seen);
    return diff;
}
