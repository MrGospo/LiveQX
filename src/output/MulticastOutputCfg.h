#pragma once
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

namespace liveqx::multicast {

// Resolved output configuration. Lives next to MulticastOutput so the parser
// can be unit-tested without dragging FFmpeg/socket headers into the test
// build.
struct OutputCfg {
    std::string address;                 // group address, e.g. 239.1.1.1
    int         port = 0;                // 1..65535
    // Optional source-NIC bind address (IPv4 string). Empty = OS picks NIC
    // from the default-route table. Drives IP_MULTICAST_IF for multicast
    // outputs (see MulticastOutput.cpp). Previously named `interface_addr`;
    // the JSON key was `interface` and is still accepted as a legacy alias.
    std::string bind_address;
    int         ttl = 16;                // multicast TTL — default enterprise broadcast
    int         send_buffer_kb = 256;    // SO_SNDBUF in KiB
    std::string container = "mpegts";    // currently only mpegts is wired
};

inline OutputCfg parseOutputCfg(const nlohmann::json& j) {
    if (!j.is_object())
        throw std::invalid_argument("multicast output: JSON object expected");

    OutputCfg out;

    if (!j.contains("address") || !j["address"].is_string()
            || j["address"].get<std::string>().empty())
        throw std::invalid_argument("multicast output: 'address' (non-empty string) required");
    out.address = j["address"].get<std::string>();

    if (!j.contains("port") || !j["port"].is_number_integer())
        throw std::invalid_argument("multicast output: 'port' (integer) required");
    out.port = j["port"].get<int>();
    if (out.port < 1 || out.port > 65535)
        throw std::invalid_argument("multicast output: 'port' out of range (1..65535)");

    // Accept both the new canonical name (`bind_address`) and the legacy
    // alias (`interface`). If both are present the new name wins — drop the
    // alias from future writes but tolerate it on read for one release cycle.
    if (j.contains("bind_address")) {
        if (!j["bind_address"].is_string())
            throw std::invalid_argument("multicast output: 'bind_address' must be a string");
        out.bind_address = j["bind_address"].get<std::string>();
    } else if (j.contains("interface")) {
        if (!j["interface"].is_string())
            throw std::invalid_argument("multicast output: 'interface' must be a string");
        out.bind_address = j["interface"].get<std::string>();
    }
    if (j.contains("ttl")) {
        if (!j["ttl"].is_number_integer())
            throw std::invalid_argument("multicast output: 'ttl' must be integer");
        out.ttl = j["ttl"].get<int>();
        if (out.ttl < 1 || out.ttl > 255)
            throw std::invalid_argument("multicast output: 'ttl' out of range (1..255)");
    }
    if (j.contains("send_buffer_kb")) {
        if (!j["send_buffer_kb"].is_number_integer())
            throw std::invalid_argument("multicast output: 'send_buffer_kb' must be integer");
        out.send_buffer_kb = j["send_buffer_kb"].get<int>();
        if (out.send_buffer_kb < 16 || out.send_buffer_kb > 16384)
            throw std::invalid_argument("multicast output: 'send_buffer_kb' out of range (16..16384)");
    }
    if (j.contains("container")) {
        if (!j["container"].is_string())
            throw std::invalid_argument("multicast output: 'container' must be a string");
        out.container = j["container"].get<std::string>();
        if (out.container != "mpegts")
            throw std::invalid_argument("multicast output: only 'mpegts' container supported");
    }

    return out;
}

} // namespace liveqx::multicast
