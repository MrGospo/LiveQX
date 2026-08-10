#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// Local disk cache for media files mirrored from a (potentially unreliable)
// share. CacheManager is the only component that writes to cache_dir; the
// render pipeline only reads files that already passed validation here.
//
// Threading: a single ContentSync thread owns this object. Public methods
// are NOT thread-safe by themselves. Snapshots returned by listCached() are
// stable values.
struct CacheEntry {
    std::string   filename;       // basename only, e.g. "clip1.mp4"
    std::string   cache_path;     // absolute path inside cache_dir_
    std::int64_t  mtime_ns = 0;   // mtime of the source file at copy time
    std::uint64_t size_bytes = 0;
};

class CacheManager {
public:
    struct Config {
        std::filesystem::path cache_dir;
        std::uint64_t         max_file_size_bytes = 0;  // 0 = no limit
    };

    explicit CacheManager(Config cfg);

    // Scan cache_dir on startup. Files that successfully open via
    // MediaValidator (or are recognized as images by extension) are recorded
    // as known entries. Returns count restored. Logs WARN per file rejected.
    std::size_t restoreFromDisk();

    // Atomic copy: source -> cache_dir/<basename>.tmp -> fsync -> rename
    // Then validate. On failure the partial file is removed.
    // Returns the cached entry on success, std::nullopt on failure.
    //
    // Reasons for failure (each logs WARN):
    //   - source not readable
    //   - size_bytes > max_file_size_bytes (when limit > 0)
    //   - copy I/O error
    //   - validation rejected the file (only for video; images skip validate)
    std::optional<CacheEntry> ingest(const std::filesystem::path& source);

    // Remove a cache file by basename. No-op if missing. Returns true on
    // actual removal.
    bool evict(const std::string& filename);

    // Read-only view. Caller must not mutate while iterating concurrently.
    const std::unordered_map<std::string, CacheEntry>& listCached() const noexcept {
        return entries_;
    }

    // Sum of size_bytes across all entries.
    std::uint64_t cacheSizeBytes() const noexcept;

    const std::filesystem::path& cacheDir() const noexcept { return cfg_.cache_dir; }

    // Stats accumulated since construction. Read by metrics layer.
    std::uint64_t copyErrors()      const noexcept { return copy_errors_; }
    std::uint64_t oversizedSkipped()const noexcept { return oversized_skipped_; }

private:
    static bool looksLikeImage(const std::string& path) noexcept;
    static bool looksLikeVideo(const std::string& path) noexcept;

    Config cfg_;
    std::unordered_map<std::string, CacheEntry> entries_;  // by filename
    std::uint64_t copy_errors_       = 0;
    std::uint64_t oversized_skipped_ = 0;
};
