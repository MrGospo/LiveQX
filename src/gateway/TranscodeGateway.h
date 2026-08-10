#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "gateway/GatewayCfg.h"
#include "gateway/IGateway.h"
#include "gateway/ts/PesAssembler.h"
#include "gateway/ts/ProgramTable.h"
#include "gateway/ts/PsiParser.h"
#include "gateway/ts/TsPacket.h"

namespace spdlog { class logger; }

namespace liveqx::gateway::fec { class FecEncoder; }

namespace liveqx::gateway {

namespace transcode { class ITranscodePipeline; }

// fix40 A6 — SPTS → SPTS transcode gateway.
//
// Reads TS packets from a single SPTS input, parses PSI to discover the input
// video/audio elementary streams, then re-encodes them at operator-chosen
// parameters (resolution, bitrate, codec backend) and emits a fresh SPTS on
// `cfg.outputs[0]`. Self-generated PAT/PMT/SDT replace the input PSI; ES PIDs
// are normalised to the configured pmt/video/audio_pid space.
//
// Step 2 (this commit) lays down the skeleton: IO thread + PSI parsing +
// ES pass-through with PID/CC remap + self-generated PAT/PMT/SDT. The actual
// decode→encode pipeline lands in steps 3..5; loss-protection
// (freeze-frame / silence / fallback) in steps 6..8.
//
// Threading: one IO thread does poll() on the input socket and emits to the
// output socket. Per-stream state (PsiSectionAssembler, ProgramTable, encoder
// pipeline once wired) is owned exclusively by the IO thread. Snapshots are
// atomic shared_ptrs so REST status reads are lock-free.
//
// Test seam: when constructed via the public ctor and never started, callers
// can drive PSI parsing + ES forwarding via testFeedDatagram(bytes) and inspect
// emitted output via testDrainOutput(). The IO thread is bypassed.
class TranscodeGateway final : public IGateway {
public:
    TranscodeGateway(int id, std::string name, GatewayCfg cfg);
    ~TranscodeGateway() override;

    TranscodeGateway(const TranscodeGateway&) = delete;
    TranscodeGateway& operator=(const TranscodeGateway&) = delete;

    bool start() override;
    void stop()  override;

    bool isRunning() const override { return running_.load(std::memory_order_acquire); }
    GatewayStats getStats() const override;
    nlohmann::json statusJson() const override;

    void setLogger(std::shared_ptr<spdlog::logger> log) override { logger_ = std::move(log); }
    void setNumaNode(int n) noexcept override { numa_node_ = n; }

    int                id()   const noexcept override { return id_; }
    const std::string& name() const noexcept override { return name_; }
    const GatewayCfg&  cfg()  const noexcept override { return cfg_; }
    void               setCfg(GatewayCfg cfg) override;

    // ── Test seam ──────────────────────────────────────────────────────────
    // Feed an in-memory UDP datagram (1..7 stacked TS packets) as if it came
    // from the input socket. Drives PSI parsing + ES routing without sockets.
    void testFeedDatagram(std::span<const std::uint8_t> bytes);

    // Force emit one PAT+PMT+SDT cycle. Bypasses the timer.
    void testEmitPsi();

    // Drain bytes that *would* be sent on the SPTS output. Each entry is one
    // fully-built UDP datagram (≤ 7×188 bytes).
    std::vector<std::vector<std::uint8_t>> testDrainOutput();

    // Snapshot of input ProgramTable (last seen on the wire). Used by tests
    // and /api/gateways/{id} to surface the discovered service.
    std::shared_ptr<const ts::ProgramTableSnapshot> inputPrograms() const noexcept {
        return input_program_table_.current();
    }

    // ── Pipeline injection (fix40 A6 step 9) ───────────────────────────────
    // The gateway runs in pass-through mode by default (input ES→output with
    // PID/CC remap, no decode). When a transcode pipeline is injected, ES
    // packets are routed through PesAssemblers and into the pipeline; the
    // pipeline's TsPacketSink feeds finished output packets back into the
    // gateway's output buffer. setPipeline() may be called only while the
    // gateway is stopped — runtime swap is not supported because the loss
    // FSM and encoder state would have to be re-bootstrapped from the input.
    void setPipeline(std::unique_ptr<transcode::ITranscodePipeline> pipeline);
    bool hasPipeline() const noexcept { return pipeline_ != nullptr; }

    // Process-wide registration of the pipeline factory. main.cpp calls this
    // once at startup so subsequent makeGateway(GatewayMode::Transcode) calls
    // automatically attach a pipeline. Tests that link FFmpeg can register
    // a real factory; the unit-test target leaves it unset and the gateway
    // runs pass-through. The factory is invoked by the GatewayManager after
    // construction (see Gateway lifecycle).
    using PipelineFactory =
        std::function<std::unique_ptr<transcode::ITranscodePipeline>(
            const TranscodeCfg&)>;
    static void registerPipelineFactory(PipelineFactory factory);
    static bool hasRegisteredPipelineFactory() noexcept;

private:
    struct Sockets;
    struct OutputBuffer;

    void runIoThread();
    void processInputDatagram(std::span<const std::uint8_t> bytes);
    void processInputTsPacket(std::span<const std::uint8_t, ts::kTsPacketSize> bytes);
    void onPsiSection(std::uint16_t pid, std::span<const std::uint8_t> sec);
    // After PAT/PMT change — recompute input video/audio PIDs + republish a
    // synthesized SDT/EIT seed. Self-generated PSI is rebuilt from cfg_ +
    // discovered stream_types.
    void refreshInputBindings();
    void emitPsi();
    void emitPsiIfDue(std::chrono::steady_clock::time_point now);
    // Append a TS packet to the output datagram buffer with PID/CC remap.
    // out_pid is already chosen by the caller (caller knows whether this is
    // video/audio/PMT/PSI). cc is the per-output-PID counter.
    void emitOutputPacket(std::span<const std::uint8_t, ts::kTsPacketSize> in_pkt,
                          std::uint16_t out_pid,
                          std::uint8_t& cc);
    // Append a fully-formed TS packet (PID + CC already set by the caller —
    // typically the transcode pipeline's PesPacketizer). No rewriting.
    void appendPreparedPacket(std::span<const std::uint8_t, ts::kTsPacketSize> pkt);
    void flushOutput();

    // ── Members ────────────────────────────────────────────────────────────
    const int                       id_;
    const std::string               name_;
    GatewayCfg                      cfg_;

    std::shared_ptr<spdlog::logger> logger_;
    int                             numa_node_ = -1;

    mutable std::mutex              stats_mu_;
    GatewayStats                    stats_;

    std::atomic<bool>               running_{false};
    std::atomic<bool>               stop_flag_{false};
    std::unique_ptr<Sockets>        sockets_;
    std::thread                     io_thread_;

    // ── Input PSI state (IO-thread owned) ──────────────────────────────────
    ts::PsiSectionAssembler                                 psi_assembler_;
    ts::ProgramTable                                        input_program_table_;
    std::shared_ptr<const ts::ProgramTableSnapshot>         last_input_snapshot_;
    // Discovered input PIDs after the most recent PAT+PMT cycle. 0xFFFF = not
    // yet discovered. video/audio_pid drive the ES pass-through (step 2) and
    // later the depacketizer→decoder feed (step 3+).
    std::uint16_t                                           input_video_pid_ = 0xFFFF;
    std::uint16_t                                           input_audio_pid_ = 0xFFFF;
    std::uint8_t                                            input_video_stream_type_ = 0;
    std::uint8_t                                            input_audio_stream_type_ = 0;

    // ── Pipeline (set when transcoding; null = pass-through) ───────────────
    std::unique_ptr<transcode::ITranscodePipeline>  pipeline_;
    // PesAssemblers are constructed lazily when a pipeline is attached. They
    // own per-PID state (CC tracking + incomplete-PES buffer) so the input
    // ES PIDs change is handled by reset() without re-allocation.
    ts::PesAssembler                                video_pes_;
    ts::PesAssembler                                audio_pes_;

    // ── Output state — single SPTS ─────────────────────────────────────────
    OutputCfg                            output_cfg_;
    std::unique_ptr<OutputBuffer>        output_buffer_;
    // Test-only retained output bytes when running_ == false.
    std::vector<std::vector<std::uint8_t>> output_test_queue_;
    // FEC encoder for the SPTS output. Constructed at start() when
    // cfg_.fec.enabled. nullptr means raw-TS UDP behaviour.
    std::unique_ptr<fec::FecEncoder>     fec_encoder_;

    // CC for self-generated PSI on output.
    std::uint8_t                         pat_cc_      = 0;
    std::uint8_t                         pmt_cc_      = 0;
    std::uint8_t                         sdt_cc_      = 0;
    std::uint8_t                         pat_version_ = 0;
    std::uint8_t                         pmt_version_ = 0;
    std::uint8_t                         sdt_version_ = 0;
    // Per-ES output CC counters. Independent from PSI cc fields above.
    std::uint8_t                         out_video_cc_ = 0;
    std::uint8_t                         out_audio_cc_ = 0;

    // PSI emission cadence.
    std::chrono::steady_clock::time_point last_pat_emit_{};
    std::chrono::steady_clock::time_point last_pmt_emit_{};
    std::chrono::steady_clock::time_point last_sdt_emit_{};
};

} // namespace liveqx::gateway
