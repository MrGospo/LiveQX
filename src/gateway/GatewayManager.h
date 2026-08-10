#pragma once
#include <atomic>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <shared_mutex>
#include <string>

#include <nlohmann/json.hpp>

#include "gateway/GatewayCfg.h"
#include "gateway/IGateway.h"

namespace liveqx::events { class EventBus; }

namespace liveqx::gateway {

// CRUD over Gateway instances + on-disk persistence.
//
// Layout:
//   gateway_root_/gw{id}-{name}/config.json
//
// Lifetime: GatewayManager outlives every Gateway it owns. CRUD methods are
// thread-safe via shared_mutex (read-mostly: REST GETs and Prometheus
// scraping under shared_lock; create/remove/patch under unique_lock).
class GatewayManager {
public:
    enum class Result {
        Ok,
        NotFound,
        AlreadyExists,
        AlreadyRunning,
        AlreadyStopped,
        StartFailed,
        BadJson,
        BadPatch,
        OutputIdConflict,
        OutputNotFound,
    };

    explicit GatewayManager(std::filesystem::path gateway_root = {});
    ~GatewayManager();

    GatewayManager(const GatewayManager&)            = delete;
    GatewayManager& operator=(const GatewayManager&) = delete;

    // Create a new gateway from cfg (POST /api/gateways). cfg shape:
    //   { "id"?: int, "name"?: string, "input": {...}, "outputs": [...] }
    // If "id" is omitted, picks max(existing)+1. Sub-ids missing on outputs
    // are auto-assigned ("out0", "out1", …); duplicates → OutputIdConflict.
    // The new gateway is created in stopped state — call play() to start it.
    Result create(const nlohmann::json& cfg, int* out_id = nullptr);

    // create() + play() under one lock; used by bootstrap so default.json
    // gateways come up running. play() failures are logged but the create
    // is preserved (mirrors ChannelManager::createAndPlay).
    Result createAndPlay(const nlohmann::json& cfg, int* out_id = nullptr);

    Result remove(int id);
    Result play(int id);
    Result stop(int id);

    // PATCH /api/gateways/{id}. body may carry any subset of:
    //   - "name" — string, renames the on-disk directory
    //   - "outputs" — full replacement of outputs[] (hot-swap on a running
    //     gateway: stop() → setCfg() → start() under unique_lock)
    //   - "input" — REJECTED inline (rebind requires delete+create), returns
    //     BadPatch.
    Result patch(int id, const nlohmann::json& body);

    nlohmann::json listJson() const;
    nlohmann::json statusJson(int id) const;     // null if not found

    // Iterate registered gateways under shared_lock. fn must not call
    // mutating GatewayManager methods (would deadlock).
    void forEachGateway(
        const std::function<void(const IGateway&)>& fn) const;

    // Scan gateway_root_/ for gw{id}-*/config.json and bootstrap each as a
    // stopped gateway. Returns the number of successfully loaded entries.
    // No-op when gateway_root_ is empty.
    std::size_t loadFromRoot();

    // Stop every gateway. Used during shutdown.
    void stopAll();

    std::size_t size() const;

    // fix33 D1 — wire an EventBus so play/stop/patch publish
    // GatewayStateChange events. Optional: nullptr disables publishing
    // (tests that don't care about events leave it unset).
    void setEventBus(liveqx::events::EventBus* bus) noexcept { events_ = bus; }

private:
    int chooseIdLocked(const nlohmann::json& cfg) const;
    IGateway* findLocked(int id) const;
    std::filesystem::path gatewayDirFor(int id, const std::string& name) const;
    void persistConfigLocked(int id, const std::string& name,
                             const GatewayCfg& cfg) const;

    void publishStateChange(int id, const std::string& name,
                            const char* state) const;

    mutable std::shared_mutex                  mu_;
    std::map<int, std::unique_ptr<IGateway>>   gateways_;
    std::filesystem::path                      gateway_root_;
    liveqx::events::EventBus*          events_{nullptr};
};

const char* gatewayManagerResultName(GatewayManager::Result r) noexcept;

} // namespace liveqx::gateway
