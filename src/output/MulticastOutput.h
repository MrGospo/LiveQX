#pragma once
#include <atomic>
#include <memory>
#include <string>
#include <thread>

#include <spdlog/logger.h>

#include "output/IOutput.h"
#include "output/MulticastOutputCfg.h"
#include "utils/SpscQueue.h"

// UDP multicast output. Encoder already produces MPEG-TS Packets; this
// driver fragments each Packet into 1316-byte UDP datagrams and sends them
// to the configured multicast group via a dedicated I/O thread.
//
// Caller path: send() pushes into the SPSC queue and returns immediately.
// I/O path:  thread pops, fragments, sendto(). On overflow we drop and
// bump packets_dropped — multicast is real-time, lag is worse than loss.
class MulticastOutput : public IOutput {
public:
    explicit MulticastOutput(liveqx::multicast::OutputCfg cfg);
    ~MulticastOutput() override;

    bool start() override;
    void stop() override;
    void send(const Packet& pkt) override;
    bool isHealthy() const override;
    OutputStats getStats() const override;
    nlohmann::json statusJson() const override;

    void setNumaNode(int node) noexcept override { numa_node_ = node; }
    void setLogger(std::shared_ptr<spdlog::logger> lg) override { logger_ = std::move(lg); }
    void setChannelId(std::string id) override { channel_id_ = std::move(id); }

private:
    void runThread(std::stop_token st);
    spdlog::logger& lg() noexcept;

    liveqx::multicast::OutputCfg cfg_;
    int numa_node_ = -1;

    struct Impl;
    std::unique_ptr<Impl> impl_;

    std::shared_ptr<spdlog::logger> logger_;
    std::string channel_id_;
};
