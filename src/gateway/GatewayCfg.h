#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace liveqx::gateway {

// Gateway operating mode (fix40). `Passthrough` is fix18 behaviour (opaque UDP
// recv → sendto fan-out). `Demux` selects DemuxGateway (fix-A2: MPTS→SPTS per
// route). `Remux` selects RemuxGateway (fix-A3..A5: SPTS→MPTS). `Transcode`
// selects TranscodeGateway (fix-A6: SPTS→SPTS, decode + re-encode +
// freeze-frame / silence / fallback on input loss).
enum class GatewayMode : std::uint8_t {
    Passthrough = 0,
    Demux       = 1,
    Remux       = 2,
    Transcode   = 3,
};

const char* toString(GatewayMode m) noexcept;
GatewayMode parseMode(const std::string& s);   // throws std::invalid_argument

// Resolved input descriptor. `address` may be unicast (we just bind on the
// chosen NIC) or multicast (we bind + IP_ADD_MEMBERSHIP via interface_addr).
// Validation lives in parseInputCfg (commit 2/10) — Gateway::start trusts the
// fields are well-formed.
struct InputCfg {
    std::string address;                 // 0.0.0.0 | 239.x.y.z | 127.0.0.1 | …
    int         port = 0;                // 1..65535
    std::string interface_addr;          // optional NIC bind (IPv4 string)
    std::string interface_name;          // optional NIC bind (eth0, lo, …)
    int         recv_buffer_kb = 1024;   // SO_RCVBUF in KiB
};

// Resolved output descriptor. Always unicast or multicast UDP. `id` is the
// stable sub-id used by REST DELETE/PATCH and Prometheus labels — assigned
// by GatewayManager (not by the cfg author).
struct OutputCfg {
    std::string id;                      // e.g. "out0", "out1" — manager-assigned
    std::string address;                 // dest address
    int         port = 0;                // 1..65535
    std::string interface_addr;          // optional NIC for IP_MULTICAST_IF
    std::string interface_name;
    int         ttl = 16;                // multicast TTL
    int         send_buffer_kb = 256;
};

// One demux routing rule (fix-A2). Maps a service_id observed in the input
// MPTS to a single OutputCfg by id. Multiple rules for different service_ids
// can target the same output_id only when the operator wants to mux them
// back together — but the typical case is one route per output.
//
// preserve_* defaults are aligned with broadcast convention: keep EIT/EPG and
// teletext/subtitle PIDs unless the operator specifically wants to drop them
// (e.g. a non-DVB downstream). Dropping EIT also bypasses SDT eit_present_
// following_flag rewrite.
struct DemuxRule {
    std::uint16_t service_id          = 0;    // matches PAT program_number
    std::string   output_id;                  // refers to OutputCfg::id
    bool          preserve_eit        = true;
    bool          preserve_subtitles  = true; // descriptor 0x59 ES PIDs
    bool          preserve_teletext   = true; // descriptor 0x56 ES PIDs
    // PID remap (fix-A2 step 4). When empty the demux preserves PIDs from the
    // input PMT verbatim. Operators can pin specific PIDs (e.g. PMT=0x100,
    // video=0x101, audio=0x102) to keep downstream gear's hard-coded PID
    // assumptions happy. Map keys are *input* PIDs, values are *output* PIDs.
    std::vector<std::pair<std::uint16_t, std::uint16_t>> pid_remap;
};

// Demux-mode parameters. PSI emission periods follow ETSI TR 101 211 §4.4 —
// 100 ms for PAT/PMT (max 500 ms is allowed), 2 s for SDT. The defaults are
// safe for receivers that timeout an ungenerated PMT in 500 ms.
struct DemuxCfg {
    std::vector<DemuxRule> routes;
    bool          emit_sdt      = true;
    std::uint16_t pat_period_ms = 100;
    std::uint16_t pmt_period_ms = 100;
    std::uint16_t sdt_period_ms = 2000;
};

// PID-remap entry for remux (fix-A3). When two input SPTS streams carry
// the same elementary PID, the manager either auto-renumbers downstream or
// the operator pins a deterministic mapping. Pinned entries take precedence;
// unspecified collisions are auto-resolved at runtime in PID order.
struct PidRemapEntry {
    std::uint16_t input_idx = 0;     // 0..inputs.size()-1
    std::uint16_t src_pid   = 0;     // 0..0x1FFF — PID as seen in input SPTS
    std::uint16_t dst_pid   = 0;     // 0..0x1FFF — PID in combined MPTS
};

// Remux-mode parameters (fix-A3). The remux gateway joins N SPTS inputs into
// a single MPTS output. Programs from each SPTS are forwarded with their PIDs
// either preserved (no collision) or remapped (pinned via pid_remap, or
// auto-allocated on first-come-first-served basis). PSI is regenerated:
// the output PAT lists every input service, each input's PMT is re-emitted
// at the (possibly-remapped) PMT PID, and SDT is the union of input SDTs.
//
// transport_stream_id and original_network_id are settable so the operator
// can match a downstream's expected DVB triplet. service_id collisions are
// resolved per `service_id_policy`:
//   - AutoRenumber: subsequent collisions advance to the next free service_id
//   - Reject:       parseGatewayCfg throws on duplicate service_ids
struct RemuxCfg {
    enum class ServiceIdPolicy : std::uint8_t {
        AutoRenumber = 0,
        Reject       = 1,
    };

    std::vector<PidRemapEntry> pid_remap;          // operator pins
    std::uint32_t target_bitrate_bps   = 0;        // 0 = no stuffing
    std::uint16_t transport_stream_id  = 1;
    std::uint16_t original_network_id  = 1;
    std::uint16_t pat_period_ms        = 100;
    std::uint16_t pmt_period_ms        = 100;
    std::uint16_t sdt_period_ms        = 2000;
    bool          emit_sdt             = true;
    ServiceIdPolicy service_id_policy  = ServiceIdPolicy::AutoRenumber;
};

const char* toString(RemuxCfg::ServiceIdPolicy p) noexcept;
RemuxCfg::ServiceIdPolicy parseServiceIdPolicy(const std::string& s);

// Transcode-mode parameters (fix-A6). The transcode gateway terminates one
// SPTS input, depacketizes ES → decodes V/A → re-encodes V/A at operator-
// chosen parameters → re-PES → re-TS-packetises → emits a single SPTS on
// `outputs[0]`. PSI is regenerated from scratch (transport_stream_id /
// service_id / service_name set here, not derived from input).
//
// The encoder backend mirrors fix29 conventions: `video_encoder_mode` is
// "auto" | "cpu" | "x264" | "nvenc" | "qsv" | "vaapi" — same string set the
// channel pipeline accepts. `auto` walks the builtin ladder.
//
// Loss protection (broadcast-grade requirement):
//   - freeze_on_video_loss: hold the last decoded frame and re-encode it as
//     P/I-frames during input video starvation, until loss_grace_ms elapses.
//   - silence_on_audio_loss: emit silence-encoded AAC frames during audio
//     starvation under the same grace window.
//   - On grace-window expiry, the gateway switches to fallback_logo_path
//     (a still image; transcoded as a freeze) until input recovers.
//   - retry_backoff_ms drives recovery probing — bounded exponential back-
//     off with a hard cap to prevent runaway reconnect loops.
struct TranscodeCfg {
    // ── Video re-encode parameters ───────────────────────────────────────
    int           video_width        = 1280;
    int           video_height       = 720;
    int           video_fps          = 25;
    std::uint32_t video_bitrate_bps  = 4'000'000;
    int           video_max_b_frames = 0;     // 0 = baseline-friendly
    std::string   video_preset       = "medium";
    std::string   video_encoder_mode = "auto"; // auto|cpu|x264|nvenc|qsv|vaapi
    int           video_gpu_index    = 0;

    // ── Audio re-encode parameters ───────────────────────────────────────
    std::uint32_t audio_bitrate_bps  = 128'000;
    int           audio_sample_rate  = 48000;
    int           audio_channels     = 2;

    // ── Loss protection ──────────────────────────────────────────────────
    bool          freeze_on_video_loss  = true;
    bool          silence_on_audio_loss = true;
    std::uint32_t loss_grace_ms         = 2000;
    std::string   fallback_logo_path;          // optional; image still
    std::uint32_t retry_backoff_ms_min  = 200;
    std::uint32_t retry_backoff_ms_max  = 5000;

    // ── PSI / output transport ───────────────────────────────────────────
    std::uint16_t transport_stream_id  = 1;
    std::uint16_t original_network_id  = 1;
    std::uint16_t service_id           = 1;
    std::string   service_name         = "Transcoded";
    std::string   provider_name        = "streaming-core";
    std::uint16_t pmt_pid              = 0x100;
    std::uint16_t video_pid            = 0x101;
    std::uint16_t audio_pid            = 0x102;
    std::uint16_t pcr_pid              = 0;       // 0 = use video_pid
    std::uint16_t pat_period_ms        = 100;
    std::uint16_t pmt_period_ms        = 100;
    std::uint16_t sdt_period_ms        = 2000;
    bool          emit_sdt             = true;
    // 0 = no CBR pacing/stuffing; >0 → reuses fix-A5 PCR-restamp + clock_
    // nanosleep_abs pacing path. Recommended: 1.10–1.15× of (video+audio)
    // bitrate to leave headroom for PSI + stuffing.
    std::uint32_t target_bitrate_bps   = 0;
};

// FEC overlay (fix40 A7) — SMPTE 2022-1 (1D column XOR) and SMPTE 2022-2
// (2D row+column XOR). Orthogonal к gateway mode: when `enabled`, every
// output of the gateway emits its TS as RTP-encapsulated MP2T (RFC 2250)
// on the configured base port and additionally emits column-FEC packets
// on `port + column_port_offset` and (if mode==TwoD) row-FEC packets on
// `port + row_port_offset`. Receivers compliant with SMPTE 2022-1/-2
// reconstruct lost media packets from the FEC streams.
//
// Matrix dimensions (Pro-MPEG COP3 r2):
//   L = number of columns (= number of media packets per row).
//   D = number of rows    (= number of media packets per column).
//   L*D ≤ 100 (standard hard cap; the FEC packet header carries SNBase
//   spread as 8-bit fields, and matrices >100 packets exceed receiver
//   buffering budgets typical of broadcast set-tops).
struct FecCfg {
    enum class Mode : std::uint8_t {
        OneD = 0,    // column FEC only
        TwoD = 1,    // column + row FEC
    };

    bool          enabled            = false;
    Mode          mode               = Mode::OneD;
    std::uint8_t  L                  = 8;     // 1..20 (columns)
    std::uint8_t  D                  = 8;     // 4..20 (rows), L*D ≤ 100

    // RTP encapsulation parameters. The media stream becomes an RTP/UDP
    // flow when FEC is enabled (raw-UDP receivers won't decode it). PT 33
    // is RFC 3551 MP2T; ts_per_rtp=7 packs 1316 bytes of payload, fitting
    // a single MTU (1500 - 20 IP - 8 UDP - 12 RTP = 1460 budget).
    std::uint32_t ssrc               = 0;     // 0 = randomize at start()
    std::uint8_t  payload_type       = 33;    // 0..127 (RFC 3550)
    std::uint8_t  ts_per_rtp         = 7;     // 1..7 TS packets per RTP

    // Port offsets for FEC streams. Defaults are the SMPTE 2022-1
    // canonical offsets so off-the-shelf receivers (VLC, tvheadend's
    // FEC plugin, hardware DVB-IP gateways) interoperate.
    std::uint16_t column_port_offset = 2;     // 1..255
    std::uint16_t row_port_offset    = 4;     // 1..255
};

const char* toString(FecCfg::Mode m) noexcept;
FecCfg::Mode parseFecMode(const std::string& s);

// Top-level cfg per gateway. Persisted to gateways/gw{id}-{name}/config.json.
//
// Multi-input shape: Passthrough/Demux use the single `input` field. Remux
// uses `input` as inputs[0] plus `extra_inputs` for inputs[1..]. The wire
// format accepts either {"input": {...}} (single) or {"inputs": [...]}
// (array, required for Remux); the parser normalises both into the same
// (input, extra_inputs) pair so downstream code never branches on shape.
struct GatewayCfg {
    GatewayMode            mode = GatewayMode::Passthrough;
    InputCfg               input;
    std::vector<InputCfg>  extra_inputs;   // inputs[1..] when mode==Remux
    std::vector<OutputCfg> outputs;
    DemuxCfg               demux;          // valid when mode == Demux
    RemuxCfg               remux;          // valid when mode == Remux
    TranscodeCfg           transcode;      // valid when mode == Transcode
    FecCfg                 fec;            // overlay; valid in any mode
};

// Parsing — used by GatewayManager (fix18 step 4) to materialise cfg from
// REST POST/PATCH bodies and from on-disk config.json. Throws
// std::invalid_argument on any structural problem; GatewayManager translates
// that into HTTP 400. `parseOutputCfg` does NOT assign a sub-id when the
// caller omits "id" — id assignment is the manager's responsibility so that
// existing ids stay stable across PATCH operations.
InputCfg   parseInputCfg(const nlohmann::json& j);
OutputCfg  parseOutputCfg(const nlohmann::json& j);
GatewayCfg parseGatewayCfg(const nlohmann::json& j);

// Round-trip serializers. Defaults (recv_buffer_kb=1024, ttl=16, …) are
// elided so the on-disk config.json stays minimal — re-parsing the elided
// JSON reproduces the original struct.
nlohmann::json toJson(const InputCfg& in);
nlohmann::json toJson(const OutputCfg& out);
nlohmann::json toJson(const DemuxRule& r);
nlohmann::json toJson(const DemuxCfg& d);
nlohmann::json toJson(const PidRemapEntry& e);
nlohmann::json toJson(const RemuxCfg& r);
nlohmann::json toJson(const TranscodeCfg& t);
nlohmann::json toJson(const FecCfg& f);
FecCfg         parseFecCfg(const nlohmann::json& j);
nlohmann::json toJson(const GatewayCfg& cfg);

} // namespace liveqx::gateway
