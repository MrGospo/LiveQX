#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace spdlog { class logger; }

struct Packet {
    std::vector<uint8_t> data;
    int64_t pts        = 0;
    int64_t dts        = 0;
    bool    is_video   = true;
    bool    is_keyframe= false;
};

struct OutputStats {
    uint64_t packets_sent = 0;
    uint64_t bytes_sent   = 0;
    double   bitrate_bps  = 0.0;
    double   rtt_ms       = 0.0;
};

// fix10: unified output-driver interface. ChannelInstance::buildRuntime
// instantiates one of the concrete drivers (SrtOutput, MulticastOutput, …)
// based on cfg["output"]["type"], then drives it through this interface.
//
// Lifecycle: build → setLogger/setChannelId/setNumaNode (optional) → start()
// → send() per packet → stop() before destruction. send() is non-blocking;
// drivers must drop on overflow and surface the count via statusJson.
class IOutput {
public:
    virtual ~IOutput() = default;

    // Open sockets / spawn writer thread / etc. Returns false on
    // unrecoverable startup failure (caller logs and aborts play()).
    // Default: no-op driver that needs no async setup.
    virtual bool start() { return true; }

    // Tear down sockets / join writer thread. Idempotent.
    virtual void stop() {}

    virtual void send(const Packet& pkt) = 0;
    virtual bool isHealthy() const = 0;
    virtual OutputStats getStats() const = 0;

    // Snapshot suitable for REST/diagnostics. Default returns the basic
    // counter triple — concrete drivers extend with transport-specific
    // fields (drop counts, peer addr, jitter, etc).
    virtual nlohmann::json statusJson() const {
        const auto s = getStats();
        return nlohmann::json{
            {"packets_sent", s.packets_sent},
            {"bytes_sent",   s.bytes_sent},
            {"bitrate_bps",  s.bitrate_bps},
            {"rtt_ms",       s.rtt_ms},
            {"healthy",      isHealthy()},
        };
    }

    // Optional knobs — drivers that don't care leave the defaults. Setters
    // are called BEFORE start(); changing them on a running driver is
    // undefined behaviour (we don't need it today).
    virtual void setLogger(std::shared_ptr<spdlog::logger>) {}
    virtual void setChannelId(std::string) {}
    virtual void setNumaNode(int) noexcept {}
};
