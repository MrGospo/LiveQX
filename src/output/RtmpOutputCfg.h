#pragma once
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

namespace liveqx::rtmp {

// Resolved output configuration for an RTMP push target. Lives in its own
// header (FFmpeg-free) so the unit-test build can exercise the parser
// without dragging libavformat into the cheap test target.
struct OutputCfg {
    std::string url;                       // rtmp:// or rtmps://
    std::string container = "flv";         // FLV is the only RTMP wire format

    int         reconnect_initial_ms      = 1000;   // 1 s
    int         reconnect_max_ms          = 60000;  // 1 min cap
    // Drop the SPSC backlog accumulated during a disconnect once the gap
    // exceeds this many seconds. 0 disables the drop entirely (queue will
    // still overflow at SPSC capacity, ~64 packets ≈ tens of ms). Default
    // 5 s — short blips replay; long blips reset to fresh GOP boundary.
    int         drop_after_disconnect_sec = 5;

    // TLS knobs — only consulted for rtmps:// URLs. Default verify=true so
    // a typo'd CDN host fails the handshake rather than silently trusting
    // any cert. Set false for self-signed test rigs. tls_ca_file overrides
    // the system CA bundle for environments shipping their own trust
    // roots.
    bool        tls_verify   = true;
    std::string tls_ca_file;

    // Optional source-NIC bind address (IPv4 string). Empty = OS picks NIC
    // from the default-route table. Wired into FFmpeg's `localaddr` muxer
    // option, which binds the outgoing TCP socket to this interface before
    // connect(). Useful on multi-NIC servers where the public-uplink NIC is
    // not the default-route NIC.
    std::string bind_address;
};

// Parses cfg["output"] subtree assuming type=="rtmp". Throws
// std::invalid_argument on rejected input — ChannelInstance catches and
// logs the diagnostic, falling back to a safe default driver.
inline OutputCfg parseOutputCfg(const nlohmann::json& j) {
    if (!j.is_object())
        throw std::invalid_argument("rtmp output: JSON object expected");

    OutputCfg out;

    if (!j.contains("url") || !j["url"].is_string()
            || j["url"].get<std::string>().empty())
        throw std::invalid_argument("rtmp output: 'url' (non-empty string) required");
    out.url = j["url"].get<std::string>();

    const auto& u = out.url;
    const bool is_rtmp  = u.rfind("rtmp://",  0) == 0;
    const bool is_rtmps = u.rfind("rtmps://", 0) == 0;
    if (!is_rtmp && !is_rtmps)
        throw std::invalid_argument("rtmp output: 'url' must start with rtmp:// or rtmps://");

    if (j.contains("container")) {
        if (!j["container"].is_string())
            throw std::invalid_argument("rtmp output: 'container' must be a string");
        out.container = j["container"].get<std::string>();
        // FLV is the de-facto wire format for RTMP. Some platforms negotiate
        // "fflv" or other variants but mainline FFmpeg always speaks "flv".
        if (out.container != "flv")
            throw std::invalid_argument("rtmp output: only 'flv' container is supported");
    }

    if (j.contains("reconnect_initial_ms")) {
        if (!j["reconnect_initial_ms"].is_number_integer())
            throw std::invalid_argument("rtmp output: 'reconnect_initial_ms' must be integer");
        out.reconnect_initial_ms = j["reconnect_initial_ms"].get<int>();
        if (out.reconnect_initial_ms < 100 || out.reconnect_initial_ms > 60000)
            throw std::invalid_argument("rtmp output: 'reconnect_initial_ms' out of range (100..60000)");
    }
    if (j.contains("reconnect_max_ms")) {
        if (!j["reconnect_max_ms"].is_number_integer())
            throw std::invalid_argument("rtmp output: 'reconnect_max_ms' must be integer");
        out.reconnect_max_ms = j["reconnect_max_ms"].get<int>();
        if (out.reconnect_max_ms < 1000 || out.reconnect_max_ms > 600000)
            throw std::invalid_argument("rtmp output: 'reconnect_max_ms' out of range (1000..600000)");
    }
    if (out.reconnect_max_ms < out.reconnect_initial_ms)
        throw std::invalid_argument("rtmp output: 'reconnect_max_ms' must be >= 'reconnect_initial_ms'");

    if (j.contains("drop_after_disconnect_sec")) {
        if (!j["drop_after_disconnect_sec"].is_number_integer())
            throw std::invalid_argument("rtmp output: 'drop_after_disconnect_sec' must be integer");
        out.drop_after_disconnect_sec = j["drop_after_disconnect_sec"].get<int>();
        if (out.drop_after_disconnect_sec < 0 || out.drop_after_disconnect_sec > 86400)
            throw std::invalid_argument("rtmp output: 'drop_after_disconnect_sec' out of range (0..86400)");
    }

    if (j.contains("tls_verify")) {
        if (!j["tls_verify"].is_boolean())
            throw std::invalid_argument("rtmp output: 'tls_verify' must be boolean");
        out.tls_verify = j["tls_verify"].get<bool>();
    }
    if (j.contains("tls_ca_file")) {
        if (!j["tls_ca_file"].is_string())
            throw std::invalid_argument("rtmp output: 'tls_ca_file' must be a string");
        out.tls_ca_file = j["tls_ca_file"].get<std::string>();
    }
    if (j.contains("bind_address")) {
        if (!j["bind_address"].is_string())
            throw std::invalid_argument("rtmp output: 'bind_address' must be a string");
        out.bind_address = j["bind_address"].get<std::string>();
    }

    return out;
}

} // namespace liveqx::rtmp
