#pragma once
#include <atomic>
#include <memory>
#include <spdlog/logger.h>
#include <string>
#include <thread>

#include "output/IOutput.h"
#include "output/RtmpOutputCfg.h"
#include "utils/SpscQueue.h"

namespace liveqx::rtmp {

// RTMP push output. Encoder feeds MPEG-TS chunks via send(); the writer
// thread demuxes them on the fly into AVPackets and remuxes to FLV over
// RTMP. We don't ask the Encoder to emit FLV directly because the same
// MPEG-TS stream is consumed by SrtOutput and MulticastOutput already —
// adding an FLV path here keeps the Encoder format-agnostic and avoids
// re-encoding (the demux+remux is header-rewriting only, not transcoding).
//
// Lifecycle: build → setLogger/setChannelId/setNumaNode → start() spawns
// the writer thread → send() per Packet (non-blocking, drops on queue
// overflow) → stop() before destruction. Reconnect is handled internally
// with exponential backoff (reconnect_initial_ms doubling up to
// reconnect_max_ms). Drop-on-stale-disconnect policy lands in step 4.
class RtmpOutput : public IOutput {
public:
    explicit RtmpOutput(OutputCfg cfg);
    ~RtmpOutput() override;

    bool start() override;
    void stop() override;

    void send(const Packet& pkt) override;
    bool isHealthy() const override;
    OutputStats getStats() const override;
    nlohmann::json statusJson() const override;

    void setLogger(std::shared_ptr<spdlog::logger> lg) override { logger_ = std::move(lg); }
    void setChannelId(std::string id) override                  { channel_id_ = std::move(id); }
    void setNumaNode(int node) noexcept override                { numa_node_ = node; }

private:
    void runThread(std::stop_token st);
    spdlog::logger& lg() noexcept;

    // Backoff state advanced exclusively from the writer thread. Public so
    // a future test could pin the ladder without spinning real I/O.
    std::chrono::milliseconds nextBackoff();

    // FFmpeg AVIO read callback — the demuxer pulls MPEG-TS bytes through
    // here, we serve them from the head of the SPSC queue. Returns
    // AVERROR_EOF when stop_io_ trips so libav unwinds cleanly.
    static int readPacketCb(void* opaque, uint8_t* buf, int buf_size);
    int        readBytes(uint8_t* buf, int buf_size);

    OutputCfg cfg_;
    int       numa_node_ = -1;
    std::shared_ptr<spdlog::logger> logger_;
    std::string                     channel_id_;

    struct Impl;
    std::unique_ptr<Impl> impl_;

    std::atomic<bool>     running_   { false };
    std::atomic<bool>     connected_ { false };
    std::atomic<bool>     stop_io_   { false };

    std::atomic<uint64_t> packets_sent_   { 0 };
    std::atomic<uint64_t> bytes_sent_     { 0 };
    std::atomic<uint64_t> packets_dropped_{ 0 };
    std::atomic<uint64_t> reconnect_count_{ 0 };

    // Set by the writer thread when a relay loop exits in error; cleared
    // when avformat_write_header succeeds. Used by the conditional drop
    // policy: queued packets older than drop_after_disconnect_sec get
    // discarded so a fresh connect resumes on a fresh GOP boundary
    // instead of replaying minutes of stale TS.
    std::atomic<int64_t>  disconnect_start_ns_ { 0 };

    int current_backoff_ms_ = 0;
};

} // namespace liveqx::rtmp
