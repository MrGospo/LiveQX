#pragma once
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

namespace liveqx::rtsp {

// Resolved input configuration for an RTSP source. The fix15 design doc
// (fix/fix15-rtsp.md) lists more knobs (rw_timeout_ms, reorder_queue_size,
// user_agent, TLS settings) — they ride in incrementally over c2..c8 to
// keep diffs reviewable. This c1 surface is what's needed for
// LiveInputFactory dispatch + the no-op skeleton ctor.
//
// Keeping the parser FFmpeg-free means the cheap unit-test target can
// validate cfg-shape regressions without linking libav*.
struct InputCfg {
    std::string url;          // rtsp:// or rtsps:// (TLS lands in c7)
    std::string transport;    // "tcp" (default) | "udp"
    std::string user;         // optional separate-field auth
    std::string password;     // optional separate-field auth

    // Reconnect backoff cap (fix15 c5). Backoff starts at 1s and doubles
    // on each failed open attempt up to this cap; reset to 1s on a
    // successful connect. Default 10s matches the plan in
    // fix/fix15-rtsp.md step 5 — long enough that we don't hammer a router
    // under network flap, short enough that a transient blip recovers
    // within seconds.
    int reconnect_max_backoff_sec = 10;

    // TLS settings for rtsps:// (fix15 c7). Same shape as RtmpInputCfg:
    // tls_verify defaults to ON — flipping it OFF skips peer-cert
    // validation and is logged as a warning so the operator can grep
    // for "insecure rtsps" in production. tls_ca_file is empty by
    // default (system trust store via FFmpeg/openssl) — set it to
    // pin a private CA bundle for self-signed camera certs.
    bool        tls_verify   = true;
    std::string tls_ca_file;

    // FFmpeg I/O knobs (fix15 c8). Defaults are sane for "good LAN
    // camera"; raise rw_timeout_ms for transcontinental links and
    // reorder_queue_size for noisy UDP. user_agent matters because some
    // cameras (Hikvision/TP-Link) refuse non-VLC user agents — see
    // docs/RTSP.md for the per-vendor table that lands in c10.
    int         rw_timeout_ms      = 5000;
    int         reorder_queue_size = 2048;
    std::string user_agent         = "LiveQX/1.0";
};

// Parses cfg["input"] assuming type=="rtsp". c1 enforces the bare
// minimum (url scheme, transport whitelist) so a malformed cfg still
// fails loudly at the factory boundary. Full validation (timeouts,
// reorder queue, TLS, user_agent) lands per-commit.
inline InputCfg parseInputCfg(const nlohmann::json& j) {
    if (!j.is_object())
        throw std::invalid_argument("rtsp input: JSON object expected");

    InputCfg out;

    if (!j.contains("url") || !j["url"].is_string()
            || j["url"].get<std::string>().empty())
        throw std::invalid_argument("rtsp input: 'url' (non-empty string) required");
    out.url = j["url"].get<std::string>();

    // rtsp:// (cleartext) and rtsps:// (TLS, FFmpeg backends it via openssl).
    // Anything else — http://, rtmp://, bare hostname — is a config typo
    // and we fail loudly. TLS-specific cfg fields are accepted from cfg
    // shape day one but only consumed once c7 wires them into the muxer
    // options.
    const auto& u = out.url;
    const bool is_rtsp  = u.rfind("rtsp://",  0) == 0;
    const bool is_rtsps = u.rfind("rtsps://", 0) == 0;
    if (!is_rtsp && !is_rtsps)
        throw std::invalid_argument(
            "rtsp input: 'url' must start with rtsp:// or rtsps://");

    // FFmpeg's `rtsp_transport` accepts tcp / udp / udp_multicast /
    // http; in production for cameras we restrict to tcp/udp — http is
    // for some legacy CDN scenarios that don't apply here, udp_multicast
    // would belong on MulticastInput. Default tcp because that's the
    // recommendation for cameras (no jitter/loss, single socket).
    if (j.contains("transport")) {
        if (!j["transport"].is_string())
            throw std::invalid_argument("rtsp input: 'transport' must be a string");
        out.transport = j["transport"].get<std::string>();
        if (out.transport != "tcp" && out.transport != "udp")
            throw std::invalid_argument(
                "rtsp input: 'transport' must be 'tcp' or 'udp'");
    } else {
        out.transport = "tcp";
    }

    if (j.contains("user")) {
        if (!j["user"].is_string())
            throw std::invalid_argument("rtsp input: 'user' must be a string");
        out.user = j["user"].get<std::string>();
    }
    if (j.contains("password")) {
        if (!j["password"].is_string())
            throw std::invalid_argument("rtsp input: 'password' must be a string");
        out.password = j["password"].get<std::string>();
    }
    // Both-or-neither — half-set credentials are almost always a typo,
    // and FFmpeg silently ignores `user` without `password` which is
    // the worst possible failure mode (camera challenges, we don't
    // answer, operator stares at an unreachable URL).
    if (out.user.empty() != out.password.empty())
        throw std::invalid_argument(
            "rtsp input: 'user' and 'password' must be set together");

    if (j.contains("tls_verify")) {
        if (!j["tls_verify"].is_boolean())
            throw std::invalid_argument("rtsp input: 'tls_verify' must be boolean");
        out.tls_verify = j["tls_verify"].get<bool>();
    }
    if (j.contains("tls_ca_file")) {
        if (!j["tls_ca_file"].is_string())
            throw std::invalid_argument("rtsp input: 'tls_ca_file' must be a string");
        out.tls_ca_file = j["tls_ca_file"].get<std::string>();
    }

    if (j.contains("rw_timeout_ms")) {
        if (!j["rw_timeout_ms"].is_number_integer())
            throw std::invalid_argument(
                "rtsp input: 'rw_timeout_ms' must be an integer");
        const int v = j["rw_timeout_ms"].get<int>();
        // 500 ms is the smallest value that's not a footgun on a real
        // LAN — anything below and a single retransmit timeout trips it.
        // 60 s is a soft upper bound; longer waits should use the
        // reconnect ladder, not a single huge open timeout.
        if (v < 500 || v > 60'000)
            throw std::invalid_argument(
                "rtsp input: 'rw_timeout_ms' must be in [500,60000]");
        out.rw_timeout_ms = v;
    }
    if (j.contains("reorder_queue_size")) {
        if (!j["reorder_queue_size"].is_number_integer())
            throw std::invalid_argument(
                "rtsp input: 'reorder_queue_size' must be an integer");
        const int v = j["reorder_queue_size"].get<int>();
        // 0 = disabled (caller wants strict packet order); 16k is the
        // largest value FFmpeg accepts before the buffer becomes a
        // memory hog on a fan-out of many cameras.
        if (v < 0 || v > 16'384)
            throw std::invalid_argument(
                "rtsp input: 'reorder_queue_size' must be in [0,16384]");
        out.reorder_queue_size = v;
    }
    if (j.contains("user_agent")) {
        if (!j["user_agent"].is_string())
            throw std::invalid_argument(
                "rtsp input: 'user_agent' must be a string");
        const auto v = j["user_agent"].get<std::string>();
        if (v.empty())
            throw std::invalid_argument(
                "rtsp input: 'user_agent' must be non-empty");
        out.user_agent = v;
    }

    if (j.contains("reconnect_max_backoff_sec")) {
        if (!j["reconnect_max_backoff_sec"].is_number_integer())
            throw std::invalid_argument(
                "rtsp input: 'reconnect_max_backoff_sec' must be an integer");
        const int v = j["reconnect_max_backoff_sec"].get<int>();
        if (v < 1 || v > 600)   // 10 min cap — anything longer is operator error
            throw std::invalid_argument(
                "rtsp input: 'reconnect_max_backoff_sec' must be in [1,600]");
        out.reconnect_max_backoff_sec = v;
    }

    return out;
}

} // namespace liveqx::rtsp
