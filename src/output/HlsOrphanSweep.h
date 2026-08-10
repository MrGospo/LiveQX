#pragma once
#include <cctype>
#include <chrono>
#include <filesystem>
#include <string>
#include <system_error>

#include <spdlog/logger.h>

#include "output/HlsOutputCfg.h"

namespace liveqx::hls {

// Segment-file maintenance helpers. Pulled into their own header so tests
// can exercise them without dragging libav into the cheap unit-test target,
// and so HlsOutput.cpp doesn't grow another inline namespace block.
//
// The match is intentionally conservative: anything not matching
// `cfg.segment_filename_pattern` is left untouched. Two HlsOutputs sharing
// a directory is a misconfiguration (validated against in HlsCfg), but
// even when it happens we never delete a non-matching file by accident.

struct PatternParts {
    std::string prefix;
    std::string suffix;
};

// Splits e.g. "seg_%05d.ts" into {"seg_", ".ts"}. Recognises any FFmpeg
// integer placeholder (`%d`, `%05d`, `%5d`, `%-d`); a literal `%%` is
// stepped over. If no placeholder is present, returns {pattern, ""} —
// callers MUST check `prefix.empty() && suffix.empty()` is not the case
// before sweeping (otherwise every file in the directory would match).
inline PatternParts splitPattern(const std::string& pattern) {
    for (size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i] != '%') continue;
        if (i + 1 < pattern.size() && pattern[i + 1] == '%') { ++i; continue; }
        size_t j = i + 1;
        while (j < pattern.size()
               && (std::isdigit(static_cast<unsigned char>(pattern[j]))
                   || pattern[j] == '-')) ++j;
        if (j < pattern.size() && pattern[j] == 'd') {
            return { pattern.substr(0, i), pattern.substr(j + 1) };
        }
    }
    return { pattern, std::string{} };
}

// True iff `name` matches `prefix + <one-or-more-digits> + suffix`. We don't
// pin the digit count to %0Nd's N because FFmpeg only treats N as a *minimum*
// width — once segment numbers grow past 10^N the file names get longer and
// older segments would otherwise stop matching.
inline bool matchesSegmentName(const std::string& name, const PatternParts& parts) {
    if (name.size() <= parts.prefix.size() + parts.suffix.size()) return false;
    if (name.compare(0, parts.prefix.size(), parts.prefix) != 0) return false;
    if (parts.suffix.size() > 0
        && name.compare(name.size() - parts.suffix.size(),
                        parts.suffix.size(), parts.suffix) != 0) return false;
    const size_t mid_start = parts.prefix.size();
    const size_t mid_end   = name.size() - parts.suffix.size();
    if (mid_end <= mid_start) return false;
    for (size_t k = mid_start; k < mid_end; ++k)
        if (!std::isdigit(static_cast<unsigned char>(name[k]))) return false;
    return true;
}

// Result of a sweep — exposed mainly for tests. Production callers can
// ignore the return value; the optional logger argument records the same
// counts at info level.
struct OrphanSweepResult {
    int tmp_removed   = 0;
    int stale_removed = 0;
    int errors        = 0;
};

// Sweeps `cfg.output_dir` for:
//   - any `<name>.tmp` whose stem matches the segment pattern (FFmpeg
//     writes <name>.tmp then renames; SIGKILL between the two leaves
//     the .tmp behind);
//   - any segment older than `cfg.delete_segments_after_sec`. FFmpeg's
//     `delete_segments` flag only sweeps segments produced *this* run,
//     so stale ones from previous runs would otherwise accumulate.
//
// `logger` may be nullptr for tests; production paths always pass one.
inline OrphanSweepResult sweepOrphans(const HlsCfg& cfg,
                                      spdlog::logger* logger = nullptr) {
    namespace cfs = std::filesystem;
    OrphanSweepResult r{};

    const auto parts = splitPattern(cfg.segment_filename_pattern);
    if (parts.prefix.empty() && parts.suffix.empty()) {
        // Defensive: validation in HlsCfg already requires exactly one
        // placeholder, but a directly-constructed HlsCfg could skip that.
        return r;
    }
    const auto stale_ttl =
        std::chrono::seconds(cfg.delete_segments_after_sec);

    std::error_code ec;
    cfs::directory_iterator it(cfg.output_dir, ec);
    if (ec) {
        if (logger) logger->warn("HlsOutput: cleanup scan failed for {}: {}",
                                 cfg.output_dir, ec.message());
        r.errors = 1;
        return r;
    }
    for (const auto& entry : it) {
        std::error_code lec;
        if (!entry.is_regular_file(lec)) continue;
        const auto name = entry.path().filename().string();

        if (name.size() > 4
            && name.compare(name.size() - 4, 4, ".tmp") == 0) {
            const std::string stem = name.substr(0, name.size() - 4);
            if (matchesSegmentName(stem, parts)) {
                cfs::remove(entry.path(), lec);
                if (!lec) ++r.tmp_removed; else ++r.errors;
            }
            continue;
        }

        if (matchesSegmentName(name, parts)) {
            const auto ftime = cfs::last_write_time(entry.path(), lec);
            if (lec) { ++r.errors; continue; }
            const auto age = std::chrono::duration_cast<std::chrono::seconds>(
                decltype(ftime)::clock::now() - ftime);
            if (age > stale_ttl) {
                cfs::remove(entry.path(), lec);
                if (!lec) ++r.stale_removed; else ++r.errors;
            }
        }
    }
    if (logger && (r.tmp_removed || r.stale_removed))
        logger->info(
            "HlsOutput: cleanup removed {} .tmp orphan(s), {} stale segment(s) in {}",
            r.tmp_removed, r.stale_removed, cfg.output_dir);
    return r;
}

} // namespace liveqx::hls
