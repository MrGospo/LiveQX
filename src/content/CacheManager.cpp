#include "content/CacheManager.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <system_error>
#include <unistd.h>

#include "clips/MediaValidator.h"
#include "utils/Log.h"

namespace fs = std::filesystem;

namespace {
constexpr std::array kImageExts{".jpg", ".jpeg", ".png", ".bmp", ".webp"};
constexpr std::array kVideoExts{".mp4", ".avi",  ".mkv", ".mov", ".ts"};

std::string lowerExt(const std::string& path) {
    auto pos = path.rfind('.');
    if (pos == std::string::npos) return {};
    std::string e = path.substr(pos);
    std::transform(e.begin(), e.end(), e.begin(), ::tolower);
    return e;
}

bool inExtList(const std::string& path, const auto& list) {
    const auto e = lowerExt(path);
    for (auto* x : list) if (e == x) return true;
    return false;
}

// fsync the file then the parent directory so the rename is durable across
// crashes. Returns true on success. Errors logged at WARN.
bool fsyncDir(const fs::path& dir) {
    int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
    if (dfd < 0) {
        LOG_WARN("CacheManager: open({}) failed: {}", dir.string(), std::strerror(errno));
        return false;
    }
    int rc = ::fsync(dfd);
    ::close(dfd);
    if (rc != 0) {
        LOG_WARN("CacheManager: fsync({}) failed: {}", dir.string(), std::strerror(errno));
        return false;
    }
    return true;
}

// Stream-copy with periodic fsync, returns false on any error.
bool streamCopy(const fs::path& src, const fs::path& dst) {
    std::ifstream in(src, std::ios::binary);
    if (!in) return false;
    std::ofstream out(dst, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    std::array<char, 64 * 1024> buf{};
    while (in) {
        in.read(buf.data(), buf.size());
        const auto got = in.gcount();
        if (got > 0) out.write(buf.data(), got);
        if (!out) return false;
    }
    out.flush();
    if (!out) return false;
    // Best-effort fsync of the file itself.
    int fd = ::open(dst.c_str(), O_RDONLY);
    if (fd >= 0) { ::fsync(fd); ::close(fd); }
    return true;
}
}  // namespace

bool CacheManager::looksLikeImage(const std::string& path) noexcept {
    return inExtList(path, kImageExts);
}
bool CacheManager::looksLikeVideo(const std::string& path) noexcept {
    return inExtList(path, kVideoExts);
}

CacheManager::CacheManager(Config cfg) : cfg_(std::move(cfg)) {
    std::error_code ec;
    fs::create_directories(cfg_.cache_dir, ec);
    if (ec) {
        LOG_ERROR("CacheManager: cannot create cache_dir '{}': {}",
                  cfg_.cache_dir.string(), ec.message());
    }
}

std::size_t CacheManager::restoreFromDisk() {
    entries_.clear();
    std::error_code ec;
    if (!fs::exists(cfg_.cache_dir, ec) || !fs::is_directory(cfg_.cache_dir, ec))
        return 0;

    std::size_t restored = 0;
    for (const auto& de : fs::directory_iterator(cfg_.cache_dir, ec)) {
        if (ec) break;
        if (!de.is_regular_file()) continue;
        const auto path     = de.path();
        const auto filename = path.filename().string();
        const auto path_str = path.string();

        // Drop leftover .tmp from a crashed copy.
        if (filename.size() > 4 &&
            filename.compare(filename.size() - 4, 4, ".tmp") == 0) {
            fs::remove(path, ec);
            LOG_WARN("CacheManager: removed stale tmp '{}'", path_str);
            continue;
        }

        const bool is_image = looksLikeImage(path_str);
        const bool is_video = looksLikeVideo(path_str);
        if (!is_image && !is_video) {
            LOG_WARN("CacheManager: unknown extension, skipping '{}'", path_str);
            continue;
        }

        if (is_video) {
            const auto vr = MediaValidator::validate(path_str);
            if (!vr.valid) {
                LOG_WARN("CacheManager: invalid cached video '{}': {} — removing",
                         path_str, vr.error);
                fs::remove(path, ec);
                continue;
            }
        }

        CacheEntry e;
        e.filename   = filename;
        e.cache_path = path_str;
        e.size_bytes = static_cast<std::uint64_t>(fs::file_size(path, ec));
        const auto wt = fs::last_write_time(path, ec);
        if (!ec) {
            e.mtime_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             wt.time_since_epoch()).count();
        }
        entries_.emplace(filename, std::move(e));
        ++restored;
    }
    LOG_INFO("CacheManager: restored {} files from '{}'",
             restored, cfg_.cache_dir.string());
    return restored;
}

std::optional<CacheEntry> CacheManager::ingest(const fs::path& source) {
    std::error_code ec;
    if (!fs::exists(source, ec) || !fs::is_regular_file(source, ec)) {
        LOG_WARN("CacheManager: source not a regular file: '{}'", source.string());
        ++copy_errors_;
        return std::nullopt;
    }

    const auto filename = source.filename().string();
    const auto src_str  = source.string();
    const bool is_image = looksLikeImage(src_str);
    const bool is_video = looksLikeVideo(src_str);
    if (!is_image && !is_video) {
        LOG_WARN("CacheManager: unsupported extension, ignoring '{}'", src_str);
        return std::nullopt;
    }

    const auto src_size = static_cast<std::uint64_t>(fs::file_size(source, ec));
    if (ec) {
        LOG_WARN("CacheManager: stat('{}') failed: {}", src_str, ec.message());
        ++copy_errors_;
        return std::nullopt;
    }
    if (cfg_.max_file_size_bytes > 0 && src_size > cfg_.max_file_size_bytes) {
        LOG_WARN("CacheManager: '{}' size {} > limit {} — skipped",
                 src_str, src_size, cfg_.max_file_size_bytes);
        ++oversized_skipped_;
        return std::nullopt;
    }

    const auto dst_final = cfg_.cache_dir / filename;
    const auto dst_tmp   = cfg_.cache_dir / (filename + ".tmp");

    fs::remove(dst_tmp, ec);  // best-effort cleanup

    if (!streamCopy(source, dst_tmp)) {
        LOG_WARN("CacheManager: copy '{}' → '{}' failed", src_str, dst_tmp.string());
        fs::remove(dst_tmp, ec);
        ++copy_errors_;
        return std::nullopt;
    }

    fs::rename(dst_tmp, dst_final, ec);
    if (ec) {
        LOG_WARN("CacheManager: rename '{}' → '{}' failed: {}",
                 dst_tmp.string(), dst_final.string(), ec.message());
        fs::remove(dst_tmp, ec);
        ++copy_errors_;
        return std::nullopt;
    }
    fsyncDir(cfg_.cache_dir);  // best-effort durability of the rename

    if (is_video) {
        const auto vr = MediaValidator::validate(dst_final.string());
        if (!vr.valid) {
            LOG_WARN("CacheManager: validation failed for '{}': {} — removing",
                     dst_final.string(), vr.error);
            fs::remove(dst_final, ec);
            ++copy_errors_;
            return std::nullopt;
        }
    }

    CacheEntry e;
    e.filename   = filename;
    e.cache_path = dst_final.string();
    e.size_bytes = src_size;
    const auto wt = fs::last_write_time(dst_final, ec);
    if (!ec) {
        e.mtime_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         wt.time_since_epoch()).count();
    }
    entries_[filename] = e;
    LOG_INFO("CacheManager: cached '{}' ({} bytes)", dst_final.string(), src_size);
    return e;
}

bool CacheManager::evict(const std::string& filename) {
    auto it = entries_.find(filename);
    if (it == entries_.end()) return false;
    std::error_code ec;
    fs::remove(it->second.cache_path, ec);
    entries_.erase(it);
    LOG_INFO("CacheManager: evicted '{}'", filename);
    return true;
}

std::uint64_t CacheManager::cacheSizeBytes() const noexcept {
    std::uint64_t total = 0;
    for (const auto& [_, e] : entries_) total += e.size_bytes;
    return total;
}
