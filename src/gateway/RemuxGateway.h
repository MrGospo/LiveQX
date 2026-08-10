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
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

#include "gateway/GatewayCfg.h"
#include "gateway/IGateway.h"
#include "gateway/ts/PcrRestamper.h"
#include "gateway/ts/ProgramTable.h"
#include "gateway/ts/PsiParser.h"
#include "gateway/ts/TsPacket.h"

namespace spdlog { class logger; }

namespace liveqx::gateway {

namespace fec { class FecEncoder; }


// fix40 A3 — SPTS → MPTS remux gateway.
//
// Reads TS packets from N independent input sockets (one per SPTS), parses
// PSI per input, resolves cross-input PID conflicts (pinned via cfg.remux.
// pid_remap, auto-allocated otherwise), and forwards every elementary stream
// packet to a single MPTS output socket with rewritten PID and per-output-PID
// continuity counters. Self-generated PSI (combined PAT, per-input PMT, merged
// SDT) is layered on in step 3; this skeleton focuses on multi-input recv and
// ES packet routing.
//
// Threading: one IO thread does poll() across all input sockets. Per-input
// state (PsiSectionAssembler, ProgramTable) is owned exclusively by the IO
// thread. Snapshots are atomic shared_ptrs so REST status reads are lock-free.
//
// Test seam: when constructed via the public ctor and never started, callers
// can drive PSI parsing + ES forwarding via testFeedDatagram(input_idx, bytes)
// and inspect emitted output via testDrainOutput(). The IO thread is bypassed.
class RemuxGateway final : public IGateway {
public:
    RemuxGateway(int id, std::string name, GatewayCfg cfg);
    ~RemuxGateway() override;

    RemuxGateway(const RemuxGateway&) = delete;
    RemuxGateway& operator=(const RemuxGateway&) = delete;

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
    // from input N. Drives PSI parsing + ES routing without sockets.
    void testFeedDatagram(std::size_t input_idx,
                          std::span<const std::uint8_t> bytes);

    // Force emit one PAT+PMT+SDT cycle. Bypasses the timer.
    void testEmitPsi();

    // Drive the bitrate-stuffing top-up at the given fake "now". Caller
    // controls the clock so tests can synthesise underruns deterministically.
    // No-op when cfg.remux.target_bitrate_bps == 0.
    void testTopUpStuffing(std::chrono::steady_clock::time_point now);

    // Drain bytes that *would* be sent on the MPTS output. Each entry is one
    // fully-built UDP datagram (≤ 7×188 bytes).
    std::vector<std::vector<std::uint8_t>> testDrainOutput();

    // Snapshot of program table for a given input.
    std::shared_ptr<const ts::ProgramTableSnapshot>
    inputPrograms(std::size_t input_idx) const;

    // Snapshot of the synthesized output program table (PIDs after remap).
    std::shared_ptr<const ts::ProgramTableSnapshot>
    outputPrograms() const;

private:
    struct Sockets;
    struct OutputBuffer;

    void runIoThread();
    void processInputDatagram(std::size_t input_idx,
                              std::span<const std::uint8_t> bytes);
    void processInputTsPacket(std::size_t input_idx,
                              std::span<const std::uint8_t, ts::kTsPacketSize> bytes);
    void onPsiSection(std::size_t input_idx, std::uint16_t pid,
                      std::span<const std::uint8_t> sec);
    // After PAT/PMT change in any input — recompute PID remap + republish
    // synthesized output ProgramTable snapshot.
    void rebuildOutputTable();
    void emitPsi();
    void emitPsiIfDue(std::chrono::steady_clock::time_point now);
    void topUpStuffing(std::chrono::steady_clock::time_point now);
    // Account for one outgoing TS packet: PCR-restamp (when PID is registered)
    // and append to the output datagram buffer. Caller may hand a mutable
    // span — the restamper will rewrite PCR fields in place.
    void sendOrBuffer(std::span<std::uint8_t, ts::kTsPacketSize> pkt);
    void flushOutput();
    // Sync PcrRestamper PID registrations with output_program_table_'s set of
    // pcr_pids. Anchors are preserved across PMT version bumps; only added/
    // removed PCR PIDs change.
    void syncPcrPids();

    // Per-input dispatch entry. pid_table_[input_idx][input_pid]:
    // out_pid 0xFFFF = drop; else dst PID with its own CC counter.
    struct PidEntry {
        std::uint16_t out_pid = 0xFFFF;
        std::uint8_t  out_cc  = 0;
    };

    struct InputState {
        InputCfg                                          cfg;
        ts::PsiSectionAssembler                           psi_assembler;
        ts::ProgramTable                                  program_table;
        std::shared_ptr<const ts::ProgramTableSnapshot>   last_seen_snapshot;
        // pid_table[input_pid] dispatch — sized 8192 (full 13-bit PID space).
        std::vector<PidEntry>                             pid_table;
        // Service-id renumber: src_sid → effective sid in output MPTS.
        std::unordered_map<std::uint16_t, std::uint16_t>  sid_remap;
        // Output PMT PID assigned to this input's PMT (default 0x100 + idx;
        // can be overridden by cfg.remux.pid_remap targeting the PMT PID).
        std::uint16_t                                     output_pmt_pid = 0;
        // CC for the synthesized output PMT.
        std::uint8_t                                      pmt_cc         = 0;
        std::uint8_t                                      pmt_version    = 0;
    };
    std::vector<InputState> inputs_;

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

    // Output state — single MPTS.
    OutputCfg                            output_cfg_;
    std::unique_ptr<OutputBuffer>        output_buffer_;
    // Test-only retained output bytes when running_ == false.
    std::vector<std::vector<std::uint8_t>> output_test_queue_;
    // FEC encoder for the MPTS output. Constructed at start() when
    // cfg_.fec.enabled. nullptr means raw-TS UDP behaviour.
    std::unique_ptr<fec::FecEncoder>     fec_encoder_;
    // CC for self-generated PSI on output.
    std::uint8_t                         pat_cc_ = 0;
    std::uint8_t                         sdt_cc_ = 0;
    std::uint8_t                         eit_cc_ = 0;
    std::uint8_t                         pat_version_ = 0;
    std::uint8_t                         sdt_version_ = 0;
    ts::ProgramTable                     output_program_table_;

    // PCR restamper — per-output-PID anchor + byte counter. Engaged when
    // cfg.remux.target_bitrate_bps > 0; otherwise behaves as pass-through.
    ts::PcrRestamper                     pcr_restamper_;
    // Mirror of currently registered output PCR PIDs. Used by syncPcrPids()
    // to compute the diff (add/remove) without forcing reset of anchors.
    std::unordered_set<std::uint16_t>    pcr_pids_registered_;

    // PSI emission cadence.
    std::chrono::steady_clock::time_point last_pat_emit_{};
    std::chrono::steady_clock::time_point last_pmt_emit_{};
    std::chrono::steady_clock::time_point last_sdt_emit_{};

    // Bitrate stuffing — running tally + last top-up wall clock. Reset on
    // start(); only consulted when cfg.remux.target_bitrate_bps > 0.
    std::uint64_t                         bytes_in_window_  = 0;
    std::chrono::steady_clock::time_point last_top_up_{};

    // µs-precision output pacing. flushOutput() sleeps via clock_nanosleep
    // (CLOCK_MONOTONIC, TIMER_ABSTIME) until target = pacing_anchor +
    // bytes_sent_total_ * 8e9 / bitrate ns, then sendto(). pacing_anchor_ is
    // captured on the first send after start(); bytes_sent_total_ counts
    // bytes that were actually written to the socket (not enqueued, not
    // dropped). When bitrate=0, pacing is bypassed.
    std::uint64_t                         bytes_sent_total_ = 0;
    bool                                  pacing_anchor_set_ = false;
    timespec                              pacing_anchor_{};

    // Resolve cfg → inputs_ + output_cfg_; throws on cfg error.
    void resolveTopology();
};

} // namespace liveqx::gateway
