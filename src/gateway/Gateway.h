#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "gateway/GatewayCfg.h"
#include "gateway/IGateway.h"

namespace spdlog { class logger; }

namespace liveqx::gateway {

// Concrete UDP/multicast forwarder. recv()'s on a single input socket, then
// sendto() fan-out to every configured output in the io_thread (synchronous
// fan-out — see fix/fix18-gateway-manager.md for rationale).
//
// Sockets are opened lazily inside start() so that build()/CRUD code never
// blocks on getaddrinfo() or bind() — failures surface as start()==false and
// the gateway stays in `failed` state until reconfigured.
class Gateway final : public IGateway {
public:
    Gateway(int id, std::string name, GatewayCfg cfg);
    ~Gateway() override;

    Gateway(const Gateway&) = delete;
    Gateway& operator=(const Gateway&) = delete;

    bool start() override;
    void stop()  override;

    bool isRunning() const override { return running_.load(std::memory_order_acquire); }
    GatewayStats getStats() const override;
    nlohmann::json statusJson() const override;

    void setLogger(std::shared_ptr<spdlog::logger> log) override { logger_ = std::move(log); }
    void setNumaNode(int n) noexcept override { numa_node_ = n; }

    int                id()   const noexcept { return id_; }
    const std::string& name() const noexcept { return name_; }
    const GatewayCfg&  cfg()  const noexcept { return cfg_; }

    // Hot-replace cfg (PATCH path). Caller must have stopped the gateway
    // before calling; the manager wraps stop() + setCfg() + start().
    void setCfg(GatewayCfg cfg);

private:
    struct SocketSet;
    void runIoThread();

    const int                       id_;
    const std::string               name_;
    GatewayCfg                      cfg_;

    std::shared_ptr<spdlog::logger> logger_;
    int                             numa_node_ = -1;

    mutable std::mutex              stats_mu_;
    GatewayStats                    stats_;

    std::atomic<bool>               running_{false};
    std::atomic<bool>               stop_flag_{false};
    std::unique_ptr<SocketSet>      sockets_;
    std::thread                     io_thread_;
};

} // namespace liveqx::gateway
