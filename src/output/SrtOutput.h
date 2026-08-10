#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <spdlog/logger.h>
#include <string>
#include <thread>
#include "output/IOutput.h"
#include "utils/SpscQueue.h"

// SRT server output: listens for one incoming client, sends MPEG-TS chunks
// via srt_sendmsg() from a dedicated I/O thread.
// The caller thread calls send() which is non-blocking (drops on queue full).
class SrtOutput : public IOutput {
public:
    // bind_address — optional IPv4 string for the source NIC. Empty = OS
    // default route. Pinned via srt_bind() before listen().
    SrtOutput(int port, int latency_ms, std::string bind_address = "");
    ~SrtOutput() override;

    // Start the listener thread (bind, listen, accept loop).
    // Returns false if the socket cannot be bound.
    bool start() override;
    void stop() override;

    // Thread-safe: pushes into SPSC queue; returns immediately.
    void send(const Packet& pkt) override;

    // True when a client is connected and the send thread is running.
    bool isHealthy() const override;

    // Returns a snapshot of cumulative counters.
    OutputStats getStats() const override;

    // Extended status: cumulative counters + transport-specific fields
    // (port, latency_ms, peer connection state).
    nlohmann::json statusJson() const override;

    // Called from the I/O thread on each new client connection.
    void onClientConnected(std::function<void()> cb);

    // NUMA hint applied at the start of the I/O thread. -1 = OS scheduler.
    void setNumaNode(int node) noexcept override { numa_node_ = node; }

    // Per-channel logger. If unset, falls back to spdlog::default_logger().
    void setLogger(std::shared_ptr<spdlog::logger> lg) override { logger_ = std::move(lg); }

    // Channel id label used for thread-name prefix (e.g. "ch7-srt").
    void setChannelId(std::string id) override { channel_id_ = std::move(id); }

private:
    void runThread(std::stop_token st);
    spdlog::logger& lg() noexcept;

    int         port_;
    int         latency_ms_;
    std::string bind_address_;

    struct Impl;
    std::unique_ptr<Impl> impl_;
    int numa_node_ = -1;
    std::shared_ptr<spdlog::logger> logger_;
    std::string channel_id_;
};
