#pragma once
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <sys/stat.h>

#include <nlohmann/json.hpp>

namespace liveqx::hls {

// Resolved HLS push-mode configuration. Lives alongside HlsOutput because
// REST validators, the channel factory and the unit-test build all need
// to interpret the same JSON shape, and keeping the parser FFmpeg-free
// lets it ride in the cheap unit-test target.
//
// Push-mode: core writes .ts segments + .m3u8 to a local directory; an
// external HTTP server (nginx/CDN/S3-mirror) handles distribution. The
// playlist runs as a sliding window of `playlist_size` segments.
struct HlsCfg {
    std::string output_dir;                          // required, abs path, writable
    int         segment_duration_sec      = 6;       // [2..30]
    int         playlist_size             = 5;       // >= 3
    int         delete_segments_after_sec = 60;      // >= segment_duration_sec * playlist_size
    std::string playlist_filename         = "stream.m3u8";
    std::string segment_filename_pattern  = "seg_%05d.ts";
};

namespace detail {

// Counts FFmpeg-style integer placeholders in the segment filename pattern.
// Recognises `%d`, `%05d`, `%5d`, `%-d` etc. — anything matching `%[0-9-]*d`
// past a literal `%%` escape. We need *exactly one* so FFmpeg's HLS muxer
// produces deterministic, monotonically-numbered segments without name
// collisions or unexpanded literals.
inline int countIntPlaceholders(const std::string& s) {
    int n = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '%') continue;
        if (i + 1 < s.size() && s[i + 1] == '%') { ++i; continue; }   // %% literal
        size_t j = i + 1;
        while (j < s.size() && (std::isdigit(static_cast<unsigned char>(s[j])) || s[j] == '-')) ++j;
        if (j < s.size() && s[j] == 'd') {
            ++n;
            i = j;
        }
    }
    return n;
}

inline bool isWritableDir(const std::string& path) {
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) return false;
    if (!S_ISDIR(st.st_mode)) return false;
    return ::access(path.c_str(), W_OK) == 0;
}

} // namespace detail

// Parses cfg["output"] subtree assuming type=="hls". Throws
// std::invalid_argument on any rejected field; ChannelInstance catches and
// decides whether to abort startup or fall back to a safe default.
inline HlsCfg parseOutputCfg(const nlohmann::json& j) {
    if (!j.is_object())
        throw std::invalid_argument("hls output: JSON object expected");

    HlsCfg out;

    if (!j.contains("output_dir") || !j["output_dir"].is_string()
            || j["output_dir"].get<std::string>().empty())
        throw std::invalid_argument("hls output: 'output_dir' (non-empty string) required");
    out.output_dir = j["output_dir"].get<std::string>();

    // Absolute path required so the muxer's segment_filename_pattern (which
    // is concatenated to output_dir as-is) doesn't surprise operators by
    // resolving against the daemon's CWD.
    if (out.output_dir.front() != '/')
        throw std::invalid_argument("hls output: 'output_dir' must be an absolute path");

    // Writability is checked here so a typo'd path or a permissions miss
    // surfaces at config-validation time, not minutes later when the writer
    // thread tries to create the first .ts.tmp.
    if (!detail::isWritableDir(out.output_dir))
        throw std::invalid_argument(
            "hls output: 'output_dir' must exist, be a directory, and be writable");

    if (j.contains("segment_duration_sec")) {
        if (!j["segment_duration_sec"].is_number_integer())
            throw std::invalid_argument("hls output: 'segment_duration_sec' must be integer");
        out.segment_duration_sec = j["segment_duration_sec"].get<int>();
        if (out.segment_duration_sec < 2 || out.segment_duration_sec > 30)
            throw std::invalid_argument(
                "hls output: 'segment_duration_sec' out of range [2..30]");
    }

    if (j.contains("playlist_size")) {
        if (!j["playlist_size"].is_number_integer())
            throw std::invalid_argument("hls output: 'playlist_size' must be integer");
        out.playlist_size = j["playlist_size"].get<int>();
        if (out.playlist_size < 3)
            throw std::invalid_argument("hls output: 'playlist_size' must be >= 3");
    }

    if (j.contains("delete_segments_after_sec")) {
        if (!j["delete_segments_after_sec"].is_number_integer())
            throw std::invalid_argument(
                "hls output: 'delete_segments_after_sec' must be integer");
        out.delete_segments_after_sec = j["delete_segments_after_sec"].get<int>();
    }
    // Window-coverage guard: a slow client may still be reading a segment
    // that just fell out of the playlist. delete-after must extend at least
    // one full window past the last sliding-window slot, otherwise we'd
    // 404 mid-fetch.
    if (out.delete_segments_after_sec
            < out.segment_duration_sec * out.playlist_size)
        throw std::invalid_argument(
            "hls output: 'delete_segments_after_sec' must be >= "
            "segment_duration_sec * playlist_size");

    if (j.contains("playlist_filename")) {
        if (!j["playlist_filename"].is_string()
                || j["playlist_filename"].get<std::string>().empty())
            throw std::invalid_argument(
                "hls output: 'playlist_filename' must be a non-empty string");
        out.playlist_filename = j["playlist_filename"].get<std::string>();
        // Anchor the playlist inside output_dir; embedded slashes would
        // let an operator accidentally write the manifest into a sibling
        // directory and break our "one channel = one folder" invariant.
        if (out.playlist_filename.find('/') != std::string::npos)
            throw std::invalid_argument(
                "hls output: 'playlist_filename' must not contain '/'");
    }

    if (j.contains("segment_filename_pattern")) {
        if (!j["segment_filename_pattern"].is_string()
                || j["segment_filename_pattern"].get<std::string>().empty())
            throw std::invalid_argument(
                "hls output: 'segment_filename_pattern' must be a non-empty string");
        out.segment_filename_pattern = j["segment_filename_pattern"].get<std::string>();
        if (out.segment_filename_pattern.find('/') != std::string::npos)
            throw std::invalid_argument(
                "hls output: 'segment_filename_pattern' must not contain '/'");
    }
    // FFmpeg expands exactly one numeric placeholder per segment. Zero
    // means every segment would clobber the previous; >1 means undefined
    // muxer behaviour (some FFmpeg versions throw, some pick the first).
    const int ph = detail::countIntPlaceholders(out.segment_filename_pattern);
    if (ph != 1)
        throw std::invalid_argument(
            "hls output: 'segment_filename_pattern' must contain exactly one "
            "%d / %0Nd placeholder");

    return out;
}

} // namespace liveqx::hls
