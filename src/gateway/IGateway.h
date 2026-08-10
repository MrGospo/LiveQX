#pragma once
#include <cstdint>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "gateway/GatewayCfg.h"

namespace spdlog { class logger; }

namespace liveqx::gateway {

// Per-output runtime counters surfaced to /api/gateways/{id} and Prometheus.
// pkt_in/bytes_in are gateway-global; the per-output equivalents reflect
// successful sendto() operations on each fan-out leg.
struct GatewayStats {
    uint64_t pkt_in     = 0;
    uint64_t bytes_in   = 0;
    uint64_t pkt_out    = 0;
    uint64_t bytes_out  = 0;
    uint64_t drops      = 0;
};

// fix18: packet-level UDP/multicast forwarder. One gateway = 1 input → N
// outputs. No FFmpeg, no FramePool, no encoder — just raw recv()/sendto().
//
// Lifecycle: build(cfg) → setLogger/setNumaNode → start() → stop() before
// destruction. start() spawns the io_thread; stop() joins it. Implementations
// are in `Gateway` (the concrete class); this interface exists so that future
// transports (TCP, SRT) can be slotted in without touching GatewayManager.
class IGateway {
public:
    virtual ~IGateway() = default;

    virtual bool start() = 0;
    virtual void stop()  = 0;

    virtual bool isRunning() const = 0;
    virtual GatewayStats getStats() const = 0;

    // Shape: {id, name, state, input:{...}, outputs:[{id, ...}, ...]}
    virtual nlohmann::json statusJson() const = 0;

    // Identity + configuration accessors. The manager owns IGateway instances
    // generically (passthrough Gateway, demux DemuxGateway, future remux/
    // transcode flavours) and dispatches CRUD against this interface.
    virtual int                id()   const noexcept = 0;
    virtual const std::string& name() const noexcept = 0;
    virtual const GatewayCfg&  cfg()  const noexcept = 0;

    // Hot-swap cfg. Caller must stop() before calling (the manager wraps
    // stop() → setCfg() → start() under unique_lock for hot-patch).
    virtual void setCfg(GatewayCfg cfg) = 0;

    virtual void setLogger(std::shared_ptr<spdlog::logger>) {}
    virtual void setNumaNode(int) noexcept {}
};

// Factory: dispatch on cfg.mode to construct the right concrete gateway.
// Throws std::invalid_argument for modes not yet implemented (Remux,
// Transcode reserved for fix-A3 / fix-A6).
std::unique_ptr<IGateway>
makeGateway(int id, std::string name, GatewayCfg cfg);

} // namespace liveqx::gateway
