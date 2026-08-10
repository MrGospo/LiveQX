#pragma once
#include <string>

namespace liveqx::util {

// RTMP and RTMPS URLs carry the stream key in the path (e.g.
// `rtmp://ingest.example.com/live/SECRETKEY`). The key is the only
// authentication primitive on most CDN ingests, so it MUST NOT appear in
// log lines or status JSON. This helper returns `scheme://host[:port]`
// only, dropping anything from the first `/` after the authority onwards.
//
// Edge cases:
//   - missing scheme → returns "rtmp://?"; callers prefer that to the
//     original because they're using the result purely for log identity.
//   - no path component at all → returns the URL verbatim.
//
// Header-only because it has no dependencies and ships with both the
// FFmpeg-bound RtmpInput/RtmpOutput translation units and the
// FFmpeg-free unit-test target.
inline std::string rtmpUrlForLogs(const std::string& url) {
    const auto sep = url.find("://");
    if (sep == std::string::npos) return "rtmp://?";
    const auto path_start = url.find('/', sep + 3);
    if (path_start == std::string::npos) return url;
    return url.substr(0, path_start);
}

// RTSP/RTSPS sanitization for logs and status JSON (fix15 c4).
//
// Unlike RTMP, RTSP cameras put the stream identifier in the path
// (e.g. /h264Preview_01_main, /Streaming/Channels/101) — the operator
// needs to see that to debug "wrong stream selected" issues, so we
// keep the path. The sensitive bits are the URL userinfo:
// `rtsp://user:pass@host/path` → `rtsp://user:***@host/path`. If the
// password is the only thing missing (`rtsp://user@host/path`) we still
// strip — `user@` alone is a fingerprint we don't need in logs.
//
// `cfg_.url` in RtspInput is creds-free by convention (auth lives in
// separate user/password fields), but this helper is a defence in
// depth: an operator may still embed creds in the URL, and we want
// them masked everywhere a URL leaks (per-channel logger, /live-status,
// audit trail).
inline std::string rtspUrlForLogs(const std::string& url) {
    const auto sep = url.find("://");
    if (sep == std::string::npos) return "rtsp://?";
    const auto authority_start = sep + 3;
    // userinfo, if present, ends at the first '@' before any '/'.
    const auto path_start = url.find('/', authority_start);
    const auto authority_end = path_start == std::string::npos ? url.size() : path_start;
    const auto at = url.find('@', authority_start);
    if (at == std::string::npos || at >= authority_end) {
        return url;   // no userinfo → nothing to mask
    }
    // Split userinfo into user[:pass]; mask pass (or whole userinfo if
    // colon missing — uncommon but treat as a single secret).
    const auto colon = url.find(':', authority_start);
    std::string masked;
    masked.reserve(url.size());
    masked.append(url, 0, authority_start);
    if (colon != std::string::npos && colon < at) {
        masked.append(url, authority_start, colon - authority_start + 1);  // "user:"
        masked.append("***");
    } else {
        masked.append("***");
    }
    masked.append(url, at, std::string::npos);   // "@host/path..."
    return masked;
}

} // namespace liveqx::util
