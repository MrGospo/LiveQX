#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

// Polls a share directory and emits diffs against the previous scan.
//
// Stabilization rule: a newly seen file is NOT reported as Added until two
// consecutive scans see the same (mtime, size). This lets a slow upload
// finish before the file is exposed to ContentSync. Removed files are
// reported immediately on the first scan that misses them.
//
// Threading: single-threaded — ContentSync drives scan() from its loop.
struct ShareDiff {
    std::vector<std::filesystem::path> added;       // absolute paths on share
    std::vector<std::string>           removed;     // filenames (basename)
    bool                               share_unreachable = false;
};

class ShareScanner {
public:
    explicit ShareScanner(std::filesystem::path share_dir);

    // Run one scan tick. Returns the diff vs previous internal state.
    ShareDiff scan();

    // Number of files currently considered stable (visible to ContentSync).
    std::size_t stableCount() const noexcept;

    // All filenames currently observed on the share (regardless of stability).
    // Used by ContentSync to reconcile the local cache against the share's
    // current contents on first successful scan / after reconnect.
    std::vector<std::string> currentFilenames() const;

    const std::filesystem::path& shareDir() const noexcept { return share_dir_; }

private:
    struct Entry {
        std::int64_t  mtime_ns = 0;
        std::uint64_t size     = 0;
        bool          stable   = false;   // confirmed by two equal scans
    };

    static bool isMediaFile(const std::filesystem::path& p) noexcept;

    std::filesystem::path                     share_dir_;
    std::unordered_map<std::string, Entry>    known_;   // by filename
};
