#pragma once
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

namespace liveqx::rtmp {

// Resolved input configuration for an RTMP source (client-mode in fix11
// step 1; server-mode added in step 2). Lives next to RtmpInput because
// REST validators, the channel factory and the unit-test build all need
// to interpret the same JSON shape, and keeping the parser FFmpeg-free
// lets it ride in the cheap unit-test target.
struct InputCfg {
    std::string mode = "client";          // "client" | "server" (fix11 s1: only client)
    std::string url;                      // rtmp:// or rtmps://
    std::string stream_key;               // server-mode only (added in s2)

    int         reconnect_initial_ms = 500;
    int         reconnect_max_ms     = 30000;
    int         buffer_ms            = 200;

    // TLS knobs — only consulted for rtmps:// URLs. Default verify=true so
    // ingest from a typo'd CDN host fails the handshake instead of silently
    // accepting any cert. Set false for self-signed test rigs. tls_ca_file
    // overrides the system CA bundle for environments where the deploy
    // ships its own trust roots.
    bool        tls_verify   = true;
    std::string tls_ca_file;
};

// Parses the cfg["input"] subtree assuming type=="rtmp". Throws
// std::invalid_argument on any rejected field; ChannelInstance catches
// and decides whether to abort startup or fall back to a safe default.
inline InputCfg parseInputCfg(const nlohmann::json& j) {
    if (!j.is_object())
        throw std::invalid_argument("rtmp input: JSON object expected");

    InputCfg out;

    if (j.contains("mode")) {
        if (!j["mode"].is_string())
            throw std::invalid_argument("rtmp input: 'mode' must be a string");
        out.mode = j["mode"].get<std::string>();
        // Server mode is parser-accepted from day one so a future commit can
        // wire it up without touching upstream validators; the InputClip
        // implementation refuses to start in server mode until step 2 lands.
        if (out.mode != "client" && out.mode != "server")
            throw std::invalid_argument("rtmp input: 'mode' must be 'client' or 'server'");
    }

    if (!j.contains("url") || !j["url"].is_string()
            || j["url"].get<std::string>().empty())
        throw std::invalid_argument("rtmp input: 'url' (non-empty string) required");
    out.url = j["url"].get<std::string>();

    // Both rtmp:// and rtmps:// are valid — TLS is delegated to FFmpeg's
    // openssl backend (see fix/fix11-rtmp-io.md "TLS" risk). Anything else is
    // a config typo and we fail loudly rather than silently hand FFmpeg a
    // URL it would mis-handle.
    const auto& u = out.url;
    const bool is_rtmp  = u.rfind("rtmp://",  0) == 0;
    const bool is_rtmps = u.rfind("rtmps://", 0) == 0;
    if (!is_rtmp && !is_rtmps)
        throw std::invalid_argument("rtmp input: 'url' must start with rtmp:// or rtmps://");

    if (j.contains("stream_key")) {
        if (!j["stream_key"].is_string())
            throw std::invalid_argument("rtmp input: 'stream_key' must be a string");
        out.stream_key = j["stream_key"].get<std::string>();
    }

    // Server-mode security: the stream key in the listen URL is the only
    // authentication primitive available to libavformat's RTMP server.
    // Allowing an empty key would let any publisher on the same network
    // push to /live and clobber our channel — refuse rather than ship
    // an open relay by accident.
    if (out.mode == "server" && out.stream_key.empty())
        throw std::invalid_argument("rtmp input: server-mode requires non-empty 'stream_key'");

    if (j.contains("reconnect_initial_ms")) {
        if (!j["reconnect_initial_ms"].is_number_integer())
            throw std::invalid_argument("rtmp input: 'reconnect_initial_ms' must be integer");
        out.reconnect_initial_ms = j["reconnect_initial_ms"].get<int>();
        if (out.reconnect_initial_ms < 50 || out.reconnect_initial_ms > 60000)
            throw std::invalid_argument("rtmp input: 'reconnect_initial_ms' out of range (50..60000)");
    }
    if (j.contains("reconnect_max_ms")) {
        if (!j["reconnect_max_ms"].is_number_integer())
            throw std::invalid_argument("rtmp input: 'reconnect_max_ms' must be integer");
        out.reconnect_max_ms = j["reconnect_max_ms"].get<int>();
        if (out.reconnect_max_ms < 100 || out.reconnect_max_ms > 600000)
            throw std::invalid_argument("rtmp input: 'reconnect_max_ms' out of range (100..600000)");
    }
    if (out.reconnect_max_ms < out.reconnect_initial_ms)
        throw std::invalid_argument("rtmp input: 'reconnect_max_ms' must be >= 'reconnect_initial_ms'");

    if (j.contains("buffer_ms")) {
        if (!j["buffer_ms"].is_number_integer())
            throw std::invalid_argument("rtmp input: 'buffer_ms' must be integer");
        out.buffer_ms = j["buffer_ms"].get<int>();
        if (out.buffer_ms < 0 || out.buffer_ms > 10000)
            throw std::invalid_argument("rtmp input: 'buffer_ms' out of range (0..10000)");
    }

    if (j.contains("tls_verify")) {
        if (!j["tls_verify"].is_boolean())
            throw std::invalid_argument("rtmp input: 'tls_verify' must be boolean");
        out.tls_verify = j["tls_verify"].get<bool>();
    }
    if (j.contains("tls_ca_file")) {
        if (!j["tls_ca_file"].is_string())
            throw std::invalid_argument("rtmp input: 'tls_ca_file' must be a string");
        out.tls_ca_file = j["tls_ca_file"].get<std::string>();
    }

    return out;
}

// Returns the URL passed to avformat_open_input. For client-mode this is
// the configured URL verbatim (FFmpeg already understands the RTMP
// handshake). For server-mode we append the stream key to the path so
// libavformat's RTMP server matches incoming /live/{key} publishes.
//
// The actual `listen=1` flag goes via AVDictionary — encoding it in the
// URL as a query string makes libavformat treat "?listen=1" as part of
// the stream name and reject any publisher whose path doesn't carry the
// same suffix ("Unexpected stream X, expecting X?listen=1"). Keeping the
// URL free of options also keeps the path comparable to what publishers
// see.
inline std::string buildFfmpegUrl(const InputCfg& c) {
    if (c.mode == "server") {
        std::string url = c.url;
        if (!c.stream_key.empty()) {
            if (!url.empty() && url.back() != '/') url += '/';
            url += c.stream_key;
        }
        return url;
    }
    return c.url;
}

} // namespace liveqx::rtmp
