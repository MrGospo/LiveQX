#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
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
#include "gateway/ts/ProgramTable.h"
#include "gateway/ts/PsiParser.h"
#include "gateway/ts/TsPacket.h"

namespace spdlog { class logger; }

namespace liveqx::gateway {

namespace fec { class FecEncoder; }


// fix40 A2: MPTS → SPTS demux gateway.
//
// Reads TS packets from a single UDP/multicast input (one MPTS), parses PSI
// (PAT/PMT/SDT), and routes each program to its own UDP/multicast output as
// an SPTS. Each output gets a freshly generated PAT (single program), PMT
// (with optional PID remap), and optionally SDT (subset for that service).
// EIT events for routed services pass through filtered (per-service mask);
// unrouted EIT events are dropped.
//
// The IO thread does everything: recv → parse PSI → forward ES → emit PSI on
// timer. No worker threads; broadcast-grade hardware can do single-thread TS
// processing at >100 Mb/s without breaking a sweat.
//
// Test seam: when constructed via the public ctor and never started, callers
// can drive PSI parsing + ES forwarding via testFeedDatagram() and inspect
// emitted output via testDrainOutput(). The IO thread is bypassed so unit
// tests do not need real sockets or timing.
class DemuxGateway final : public IGateway {
public:
    DemuxGateway(int id, std::string name, GatewayCfg cfg);
    ~DemuxGateway() override;

    DemuxGateway(const DemuxGateway&) = delete;
    DemuxGateway& operator=(const DemuxGateway&) = delete;

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
    void               setCfg(GatewayCfg cfg);

    // ── Test seam ──────────────────────────────────────────────────────────
    // Feed an in-memory UDP datagram (1..7 stacked TS packets) as if it came
    // from the input socket. Drives PSI parsing + ES routing without sockets.
    void testFeedDatagram(std::span<const std::uint8_t> bytes);

    // Force emit one PAT+PMT+SDT cycle to all outputs. Bypasses the timer.
    void testEmitPsi();

    // Drain the per-output buffer of bytes that *would* be sent. Each entry
    // is one fully-built UDP datagram (≤ 7×188 bytes).
    std::vector<std::vector<std::uint8_t>> testDrainOutput(std::size_t output_idx);

    // Snapshot of the discovered ProgramTable, for tests & /api/gateways/{id}.
    std::shared_ptr<const ts::ProgramTableSnapshot> programs() const noexcept {
        return program_table_.current();
    }

private:
    struct Sockets;
    struct OutputBuffer;            // packs ≤7 TS packets into one UDP datagram

    void runIoThread();
    void processUdpDatagram(std::span<const std::uint8_t> bytes);
    void processTsPacket(std::span<const std::uint8_t, ts::kTsPacketSize> bytes);
    void onPsiSection(std::uint16_t pid, std::span<const std::uint8_t> sec);
    void rebuildPidTable();         // after PAT/PMT change
    void emitPsi();                 // PAT + PMT(s) + SDT(s) to all outputs
    void emitPsiIfDue(std::chrono::steady_clock::time_point now);
    void sendOrBuffer(std::size_t out_idx,
                      std::span<const std::uint8_t, ts::kTsPacketSize> pkt);
    void flushOutput(std::size_t out_idx);

    // Per-PID dispatch entry. Built from the current ProgramTable each time
    // PAT or PMT changes. PIDs not in the table fall through to "drop".
    struct PidEntry {
        std::uint8_t  out_idx = 0xFF; // 0xFF = drop
        std::uint16_t out_pid = 0;
        // Output continuity counter. We rewrite CC per-output so each SPTS has
        // monotonic CC even though we drop packets from the input MPTS.
        std::uint8_t  out_cc  = 0;
    };
    std::vector<PidEntry> pid_table_;   // size = 8192 (13-bit PID space)

    // Per-output state. Indices align with cfg_.outputs (a route's output_id
    // is resolved to an index here at start()/setCfg() time).
    struct Output {
        std::string                                 id;
        std::uint16_t                               service_id = 0;
        DemuxRule                                   rule;
        // CC counters for our self-generated PSI PIDs (0x0000 PAT, pmt_pid PMT,
        // 0x0011 SDT). Independent from per-ES CC in pid_table_.
        std::uint8_t                                pat_cc = 0;
        std::uint8_t                                pmt_cc = 0;
        std::uint8_t                                sdt_cc = 0;
        std::uint8_t                                eit_cc = 0;
        std::uint8_t                                pmt_version = 0;
        std::uint8_t                                pat_version = 0;
        std::uint8_t                                sdt_version = 0;
        // PMT PID assigned to this output. By default we allocate 0x0100 +
        // output_idx; a route's pid_remap may override.
        std::uint16_t                               output_pmt_pid = 0;
        // Bytes ready to send (test path) or already sent (live path).
        std::unique_ptr<OutputBuffer>               buffer;
        // Test-only retained output (when running_==false): each entry is one
        // UDP datagram worth of bytes.
        std::vector<std::vector<std::uint8_t>>      testQueue;
        // Per-output FEC encoder. Constructed at start() when cfg_.fec.enabled
        // is true. nullptr means "send raw TS over UDP" (legacy path). When
        // present, the live path routes every TS packet through the encoder,
        // which fans out to media RTP + column FEC (+ row FEC, in 2D mode)
        // sockaddrs on the same fd.
        std::unique_ptr<fec::FecEncoder>            fec_encoder;
    };
    std::vector<Output> outputs_;

    void resolveOutputs();           // build outputs_ from cfg_; throws on cfg error

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

    // PSI state — owned exclusively by the IO thread (or by the test thread
    // when running_ is false). No locking required.
    ts::PsiSectionAssembler         psi_assembler_;
    ts::ProgramTable                program_table_;
    // Snapshot pointer captured at the last rebuildPidTable() — if it equals
    // the current snapshot, nothing changed since the last build. Snapshot
    // identity is preserved across no-op republishes by ProgramTable.
    std::shared_ptr<const ts::ProgramTableSnapshot> last_routed_snapshot_;

    // PSI emission cadence.
    std::chrono::steady_clock::time_point last_pat_emit_{};
    std::chrono::steady_clock::time_point last_pmt_emit_{};
    std::chrono::steady_clock::time_point last_sdt_emit_{};
};

} // namespace liveqx::gateway
