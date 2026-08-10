#pragma once
#include <algorithm>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

namespace liveqx::multicast {

// Resolved input configuration for a multicast source. Lives next to
// MulticastInput because multiple subsystems (validator, REST, factory)
// need to interpret the same JSON shape; keeping the parser FFmpeg-free
// lets it ride in the unit-test build.
struct InputCfg {
    std::string address;                  // group address, e.g. 239.0.0.1
    int         port = 0;                 // 1..65535
    std::string interface_addr;           // optional NIC bind address (IPv4 string)
    std::string container = "mpegts";     // "mpegts" | "rtp"
    int         jitter_buffer_ms = 100;   // 0..5000
    int         reconnect_on_silence_sec = 10;  // 0 disables watchdog re-join
};

// Parses the cfg["input"] subtree. Throws std::invalid_argument with a
// human-readable diagnostic on any rejected field — caller decides whether
// to abort startup or log and skip.
inline InputCfg parseInputCfg(const nlohmann::json& j) {
    if (!j.is_object())
        throw std::invalid_argument("multicast input: JSON object expected");

    InputCfg out;

    if (!j.contains("address") || !j["address"].is_string()
            || j["address"].get<std::string>().empty())
        throw std::invalid_argument("multicast input: 'address' (non-empty string) required");
    out.address = j["address"].get<std::string>();

    if (!j.contains("port") || !j["port"].is_number_integer())
        throw std::invalid_argument("multicast input: 'port' (integer) required");
    out.port = j["port"].get<int>();
    if (out.port < 1 || out.port > 65535)
        throw std::invalid_argument("multicast input: 'port' out of range (1..65535)");

    if (j.contains("interface")) {
        if (!j["interface"].is_string())
            throw std::invalid_argument("multicast input: 'interface' must be a string");
        out.interface_addr = j["interface"].get<std::string>();
    }

    if (j.contains("container")) {
        if (!j["container"].is_string())
            throw std::invalid_argument("multicast input: 'container' must be a string");
        out.container = j["container"].get<std::string>();
        if (out.container != "mpegts" && out.container != "rtp")
            throw std::invalid_argument("multicast input: unsupported 'container' (mpegts|rtp)");
    }

    if (j.contains("jitter_buffer_ms")) {
        if (!j["jitter_buffer_ms"].is_number_integer())
            throw std::invalid_argument("multicast input: 'jitter_buffer_ms' must be integer");
        out.jitter_buffer_ms = j["jitter_buffer_ms"].get<int>();
        if (out.jitter_buffer_ms < 0 || out.jitter_buffer_ms > 5000)
            throw std::invalid_argument("multicast input: 'jitter_buffer_ms' out of range (0..5000)");
    }

    if (j.contains("reconnect_on_silence_sec")) {
        if (!j["reconnect_on_silence_sec"].is_number_integer())
            throw std::invalid_argument("multicast input: 'reconnect_on_silence_sec' must be integer");
        out.reconnect_on_silence_sec = j["reconnect_on_silence_sec"].get<int>();
        if (out.reconnect_on_silence_sec < 0 || out.reconnect_on_silence_sec > 3600)
            throw std::invalid_argument("multicast input: 'reconnect_on_silence_sec' out of range (0..3600)");
    }

    return out;
}

// Builds the FFmpeg `udp://` URL plus query params used by avformat_open_input.
// Pulled out so MulticastInput.cpp and tests share the exact same string.
inline std::string buildFfmpegUrl(const InputCfg& c) {
    // fifo_size is FFmpeg's UDP receive ring (units = packets, default 7).
    // We size it from jitter_buffer_ms assuming worst case ~1316 B/packet at
    // 8 Mbps (~750 packets/s). 100ms → ~75 packets, but FFmpeg multiplies by
    // 188 internally so we pass a bytes-style budget ≥ 7*188 to keep older
    // builds happy — set fifo_size in bytes via udp options.
    const int fifo_bytes = std::max(188 * 7,
                                    c.jitter_buffer_ms * 1316 * 8);  // ~B at 8 Mbps
    std::string url = "udp://" + c.address + ":" + std::to_string(c.port);
    url += "?reuse=1";
    url += "&fifo_size=" + std::to_string(fifo_bytes);
    if (!c.interface_addr.empty())
        url += "&localaddr=" + c.interface_addr;
    return url;
}

} // namespace liveqx::multicast
