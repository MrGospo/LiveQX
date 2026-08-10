#pragma once
#include <atomic>
#include <memory>
#include <string>
#include <thread>

#include <spdlog/logger.h>

#include "output/HlsOutputCfg.h"
#include "output/IOutput.h"
#include "utils/SpscQueue.h"

namespace liveqx::hls {

// HLS push-mode output. Encoder feeds MPEG-TS Packets via send(); a writer
// thread demuxes them into AVPackets and forwards to FFmpeg's HLS muxer,
// which writes `.ts` segments + a sliding-window `.m3u8` playlist into
// `cfg.output_dir`. An external HTTP server (nginx) is responsible for
// serving those files to viewers — core never touches the wire.
//
// Lifecycle: build → setLogger/setChannelId/setNumaNode (optional) → start()
// → send() per Packet → stop() before destruction. send() is non-blocking;
// OutputManager fans packets into our per-driver SPSC queue and a dedicated
// pump thread drives this driver's send().
//
// fix14 c1 — skeleton only (no muxing, no thread).
// fix14 c2 — real muxer: SPSC + AVIO + mpegts demuxer → HLS muxer with
//            hls_flags=temp_file for atomic rename. statusJson refined in c5.
class HlsOutput : public IOutput {
public:
    explicit HlsOutput(HlsCfg cfg);
    ~HlsOutput() override;

    bool start() override;
    void stop() override;
    void send(const Packet& pkt) override;
    bool isHealthy() const override;
    OutputStats getStats() const override;
    nlohmann::json statusJson() const override;

    void setLogger(std::shared_ptr<spdlog::logger> lg) override { logger_ = std::move(lg); }
    void setChannelId(std::string id) override                  { channel_id_ = std::move(id); }
    void setNumaNode(int node) noexcept override                { numa_node_ = node; }

    const HlsCfg& cfg() const noexcept { return cfg_; }

private:
    void runThread(std::stop_token st);
    spdlog::logger& lg() noexcept;

    static int readPacketCb(void* opaque, uint8_t* buf, int buf_size);
    int        readBytes(uint8_t* buf, int buf_size);

    HlsCfg cfg_;
    int    numa_node_ = -1;
    std::shared_ptr<spdlog::logger> logger_;
    std::string                     channel_id_;

    struct Impl;
    std::unique_ptr<Impl> impl_;

    std::atomic<bool>     running_         { false };
    std::atomic<bool>     stop_io_         { false };
    std::atomic<bool>     started_         { false };
    std::atomic<bool>     healthy_         { true };

    std::atomic<uint64_t> packets_sent_    { 0 };
    std::atomic<uint64_t> bytes_sent_      { 0 };
    std::atomic<uint64_t> packets_dropped_ { 0 };

    // Steady-clock timestamp of the most recent successful
    // av_interleaved_write_frame. Drives both the staleness side of
    // isHealthy() and the last_packet_age_ms field surfaced in statusJson.
    std::atomic<uint64_t> last_packet_write_ns_ { 0 };

    std::atomic<uint64_t> segments_written_ { 0 };
    std::atomic<uint64_t> last_segment_ns_  { 0 };
    std::string           last_segment_path_;
};

} // namespace liveqx::hls
