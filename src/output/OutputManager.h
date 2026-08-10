#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <shared_mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "output/IOutput.h"
#include "utils/SpscQueue.h"

// Fan-out for the encoder: every Packet pushed via send() reaches every
// registered driver. Drivers are looked up by a per-channel id so REST
// add/remove and statusJson can address them individually.
//
// Each driver gets its own SPSC queue + pump thread (fix12 c2). The
// encoder's send() is therefore isolated from any single driver: a slow
// RTMP driver can fill its own queue without blocking SRT or multicast,
// and an overflowed queue drops to that driver only. Drops are counted
// per-driver and surfaced via statusJson().
class OutputManager {
public:
    OutputManager() = default;
    ~OutputManager();

    OutputManager(const OutputManager&)            = delete;
    OutputManager& operator=(const OutputManager&) = delete;

    // Default per-driver queue memory budget (4 MiB). Overridable per-output
    // via the queue_bytes_limit config field — see addDriver().
    static constexpr std::uint64_t kDefaultQueueBytesLimit = 4ull * 1024ull * 1024ull;

    // Register a driver under a channel-unique id. Spawns the per-driver
    // pump thread that consumes the SPSC queue and forwards to driver->send().
    // queue_bytes_limit caps the in-flight bytes per driver — when a push
    // would exceed it the packet is dropped to that driver only and the
    // drops counter is incremented. The 1024-slot ring is still the hard
    // upper bound regardless of the byte budget.
    // Returns false if the id is already in use or driver is null.
    bool addDriver(std::string id, std::shared_ptr<IOutput> driver,
                   std::uint64_t queue_bytes_limit = kDefaultQueueBytesLimit);

    // Drop the driver with the given id and stop+join its pump. Returns
    // false if not found. Does NOT call driver->stop() — lifetime of the
    // driver itself stays with the caller.
    bool removeDriver(const std::string& id);

    // Lookup, returns nullptr if unknown. Used to fetch the driver back
    // for transport-specific hooks (e.g. SrtOutput::onClientConnected).
    std::shared_ptr<IOutput> getDriver(const std::string& id) const;

    // Drop all drivers — used at channel teardown. Stops and joins every
    // pump thread before returning.
    void clear();

    // Iterate the registered drivers and call start() on each. Returns
    // false on the first start() failure (caller should call stopAll()
    // and abort play()). Drivers added later via REST hot-add (fix12 c4)
    // are started by the REST handler itself, not here.
    bool startAll();

    // Call stop() on every registered driver. Idempotent. Does NOT remove
    // entries — clear() does that. Use this on channel stop to release
    // sockets before the encoder/loop teardown.
    void stopAll();

    // Encoder-side entrypoint. Non-blocking: tries to push a copy of the
    // packet into every driver's SPSC queue and returns. On overflow the
    // packet is dropped to that driver only (drops counter incremented).
    void send(const Packet& pkt);

    // True iff every registered driver reports isHealthy(). With zero
    // drivers returns true (a channel with no outputs is "trivially
    // healthy" — odd but consistent with vacuous-truth semantics).
    bool allHealthy() const;

    // Snapshot of (healthy, total) driver counts (fix12 c7). Used by
    // Watchdog to feed OutputHealthSummary into ChannelHealth so a
    // partially-up multi-output channel transitions to Degraded.
    struct HealthCounts { int healthy = 0; int total = 0; };
    HealthCounts healthCounts() const;

    // Array of per-driver statusJson() with extra "id" and "queue_drops"
    // fields. Stable ordering: insertion order. Suitable for REST
    // /channels/{id}/outputs and aggregate /channels/{id}/status.
    nlohmann::json statusJson() const;

private:
    static constexpr size_t kQueueDepth = 1024;

    struct Entry {
        std::string                       id;
        std::shared_ptr<IOutput>          driver;
        SpscQueue<Packet, kQueueDepth>    queue;
        std::atomic<uint64_t>             drops{0};
        std::atomic<uint64_t>             queue_bytes{0};
        std::uint64_t                     queue_bytes_limit = kDefaultQueueBytesLimit;
        std::jthread                      pump;
    };

    void pumpLoop(Entry* e, std::stop_token st);
    static void stopAndJoin(Entry& e);

    mutable std::shared_mutex            mu_;
    std::vector<std::unique_ptr<Entry>>  entries_;
};
