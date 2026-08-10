#include "gateway/TranscodeGateway.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <utility>

#include <spdlog/spdlog.h>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "gateway/transcode/ITranscodePipeline.h"
#include "gateway/ts/Descriptors.h"
#include "gateway/fec/FecEncoder.h"
#include "gateway/ts/PsiBuilder.h"
#include "utils/CpuAffinity.h"

namespace liveqx::gateway {

namespace {
// Process-wide pipeline factory. Set by main.cpp at startup (and by itests
// that want pipeline-mode coverage). Unit-test target leaves it null →
// makeGateway returns a TranscodeGateway running in pass-through.
std::mutex                                          g_pipeline_factory_mu;
TranscodeGateway::PipelineFactory                   g_pipeline_factory;
}  // namespace

namespace {

using ts::kTsPacketSize;
using ts::kPidPat;
using ts::kPidSdt;
using ts::kPidEit;
using ts::kPidNull;
using ts::TsPacketView;

constexpr std::size_t kPacketsPerDatagram = 7;
constexpr std::size_t kRecvBufBytes       = 8192;

// ─── RAII fd ─────────────────────────────────────────────────────────────────

class FdGuard {
public:
    FdGuard() = default;
    explicit FdGuard(int fd) noexcept : fd_(fd) {}
    FdGuard(const FdGuard&) = delete;
    FdGuard& operator=(const FdGuard&) = delete;
    FdGuard(FdGuard&& o) noexcept : fd_(std::exchange(o.fd_, -1)) {}
    FdGuard& operator=(FdGuard&& o) noexcept {
        reset(); fd_ = std::exchange(o.fd_, -1); return *this;
    }
    ~FdGuard() { reset(); }

    int  get()    const noexcept { return fd_; }
    bool valid()  const noexcept { return fd_ >= 0; }
    int  release() noexcept { return std::exchange(fd_, -1); }
    void reset(int fd = -1) noexcept {
        if (fd_ >= 0) ::close(fd_);
        fd_ = fd;
    }
private:
    int fd_ = -1;
};

bool isMulticast(const in_addr& a) noexcept {
    return (ntohl(a.s_addr) & 0xF0000000u) == 0xE0000000u;
}

bool nicNameToIPv4(const std::string& name, in_addr& out) {
    ifaddrs* ifa = nullptr;
    if (::getifaddrs(&ifa) != 0) return false;
    bool found = false;
    for (ifaddrs* p = ifa; p; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
        if (name != p->ifa_name) continue;
        out = reinterpret_cast<sockaddr_in*>(p->ifa_addr)->sin_addr;
        found = true; break;
    }
    ::freeifaddrs(ifa);
    return found;
}

int openInputSocket(const InputCfg& cfg, spdlog::logger& lg) {
    in_addr group{};
    if (::inet_pton(AF_INET, cfg.address.c_str(), &group) != 1) {
        lg.error("TranscodeGateway: invalid input address '{}'", cfg.address);
        return -1;
    }
    FdGuard fd(::socket(AF_INET, SOCK_DGRAM, 0));
    if (!fd.valid()) {
        lg.error("TranscodeGateway: input socket(): {}", std::strerror(errno));
        return -1;
    }
    const int reuse = 1;
    ::setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    const int rcvbuf = cfg.recv_buffer_kb * 1024;
    ::setsockopt(fd.get(), SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    timeval tv{}; tv.tv_usec = 200 * 1000;
    ::setsockopt(fd.get(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    sockaddr_in bind_addr{};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port   = htons(static_cast<std::uint16_t>(cfg.port));
    bind_addr.sin_addr.s_addr = isMulticast(group) ? htonl(INADDR_ANY) : group.s_addr;
    if (::bind(fd.get(), reinterpret_cast<sockaddr*>(&bind_addr), sizeof(bind_addr)) < 0) {
        lg.error("TranscodeGateway: bind({}:{}) failed: {}",
                 cfg.address, cfg.port, std::strerror(errno));
        return -1;
    }
    if (!cfg.interface_name.empty()) {
        ::setsockopt(fd.get(), SOL_SOCKET, SO_BINDTODEVICE,
                     cfg.interface_name.c_str(),
                     static_cast<socklen_t>(cfg.interface_name.size()));
    }
    if (isMulticast(group)) {
        ip_mreq mreq{};
        mreq.imr_multiaddr        = group;
        mreq.imr_interface.s_addr = htonl(INADDR_ANY);
        if (!cfg.interface_addr.empty()) {
            ::inet_pton(AF_INET, cfg.interface_addr.c_str(), &mreq.imr_interface);
        } else if (!cfg.interface_name.empty()) {
            in_addr nic{}; if (nicNameToIPv4(cfg.interface_name, nic)) mreq.imr_interface = nic;
        }
        if (::setsockopt(fd.get(), IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
            lg.error("TranscodeGateway: IP_ADD_MEMBERSHIP({}) failed: {}",
                     cfg.address, std::strerror(errno));
            return -1;
        }
    }
    return fd.release();
}

int openOutputSocket(const OutputCfg& cfg, sockaddr_in& dst_out,
                     spdlog::logger& lg) {
    in_addr dst{};
    if (::inet_pton(AF_INET, cfg.address.c_str(), &dst) != 1) {
        lg.error("TranscodeGateway[{}]: invalid output address '{}'",
                 cfg.id, cfg.address);
        return -1;
    }
    FdGuard fd(::socket(AF_INET, SOCK_DGRAM, 0));
    if (!fd.valid()) {
        lg.error("TranscodeGateway[{}]: output socket(): {}",
                 cfg.id, std::strerror(errno));
        return -1;
    }
    const int sndbuf = cfg.send_buffer_kb * 1024;
    ::setsockopt(fd.get(), SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    if (isMulticast(dst)) {
        const int ttl = cfg.ttl;
        ::setsockopt(fd.get(), IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
        in_addr ifaddr{}; bool have_if = false;
        if (!cfg.interface_addr.empty()) {
            ::inet_pton(AF_INET, cfg.interface_addr.c_str(), &ifaddr);
            have_if = true;
        } else if (!cfg.interface_name.empty()) {
            have_if = nicNameToIPv4(cfg.interface_name, ifaddr);
        }
        if (have_if) ::setsockopt(fd.get(), IPPROTO_IP, IP_MULTICAST_IF, &ifaddr, sizeof(ifaddr));
    } else if (!cfg.interface_name.empty()) {
        ::setsockopt(fd.get(), SOL_SOCKET, SO_BINDTODEVICE,
                     cfg.interface_name.c_str(),
                     static_cast<socklen_t>(cfg.interface_name.size()));
    }
    dst_out = sockaddr_in{};
    dst_out.sin_family = AF_INET;
    dst_out.sin_port   = htons(static_cast<std::uint16_t>(cfg.port));
    dst_out.sin_addr   = dst;
    return fd.release();
}

// EN 300 468 §6.2.33 service_type lookup. Mirrors DemuxGateway's helper but
// works against the (stream_type, has_audio) we already have on the gateway.
std::uint8_t serviceTypeForStreamTypes(std::uint8_t v_stream_type,
                                       std::uint8_t a_stream_type) noexcept {
    if (v_stream_type == 0x24) return 0x1F;          // HEVC digital TV
    if (v_stream_type == 0x1B) return 0x19;          // H.264 HD
    if (v_stream_type != 0)    return 0x01;          // SD digital TV
    if (a_stream_type != 0)    return 0x02;          // digital radio
    return 0x01;
}

} // namespace

// ─── Output buffer ───────────────────────────────────────────────────────────

struct TranscodeGateway::OutputBuffer {
    std::array<std::uint8_t, kPacketsPerDatagram * kTsPacketSize> bytes{};
    std::size_t fill = 0;

    bool full() const noexcept { return fill == bytes.size(); }
    void append(std::span<const std::uint8_t, kTsPacketSize> pkt) noexcept {
        std::memcpy(bytes.data() + fill, pkt.data(), kTsPacketSize);
        fill += kTsPacketSize;
    }
    void reset() noexcept { fill = 0; }
};

// ─── Sockets ─────────────────────────────────────────────────────────────────

struct TranscodeGateway::Sockets {
    FdGuard      in_fd;
    FdGuard      out_fd;
    sockaddr_in  out_addr{};
    sockaddr_in  out_addr_col_fec{};
    sockaddr_in  out_addr_row_fec{};
};

// ─── Lifecycle ───────────────────────────────────────────────────────────────

TranscodeGateway::TranscodeGateway(int id, std::string name, GatewayCfg cfg)
    : id_(id), name_(std::move(name)), cfg_(std::move(cfg)) {
    if (cfg_.outputs.size() != 1)
        throw std::invalid_argument(
            "TranscodeGateway: exactly one output required");
    output_cfg_ = cfg_.outputs.front();
    output_buffer_ = std::make_unique<OutputBuffer>();

    // If main.cpp registered a pipeline factory at startup, attach one.
    PipelineFactory factory;
    {
        std::lock_guard<std::mutex> lk(g_pipeline_factory_mu);
        factory = g_pipeline_factory;
    }
    if (factory) {
        try {
            auto pipeline = factory(cfg_.transcode);
            if (pipeline) setPipeline(std::move(pipeline));
        } catch (const std::exception&) {
            // The pipeline factory failing is not fatal — the gateway can
            // still run in pass-through. Log when a logger is attached.
        }
    }
}

TranscodeGateway::~TranscodeGateway() { TranscodeGateway::stop(); }

bool TranscodeGateway::start() {
    if (running_.load(std::memory_order_acquire)) return false;

    auto& lg = logger_ ? *logger_ : *spdlog::default_logger();

    auto set = std::make_unique<Sockets>();
    const int in_fd = openInputSocket(cfg_.input, lg);
    if (in_fd < 0) return false;
    set->in_fd.reset(in_fd);

    sockaddr_in dst{};
    const int out_fd = openOutputSocket(output_cfg_, dst, lg);
    if (out_fd < 0) return false;
    set->out_fd.reset(out_fd);
    set->out_addr = dst;
    if (cfg_.fec.enabled) {
        set->out_addr_col_fec = dst;
        set->out_addr_col_fec.sin_port = htons(static_cast<std::uint16_t>(
            output_cfg_.port + cfg_.fec.column_port_offset));
        set->out_addr_row_fec = dst;
        set->out_addr_row_fec.sin_port = htons(static_cast<std::uint16_t>(
            output_cfg_.port + cfg_.fec.row_port_offset));
    }
    sockets_ = std::move(set);

    if (cfg_.fec.enabled) {
        const int        fd    = sockets_->out_fd.get();
        const sockaddr_in media = sockets_->out_addr;
        const sockaddr_in col   = sockets_->out_addr_col_fec;
        const sockaddr_in row   = sockets_->out_addr_row_fec;
        auto sendOn = [fd, this](const sockaddr_in& addr,
                                 std::span<const std::uint8_t> bytes) {
            const ssize_t s = ::sendto(
                fd, bytes.data(), bytes.size(), MSG_DONTWAIT,
                reinterpret_cast<const sockaddr*>(&addr),
                sizeof(sockaddr_in));
            std::lock_guard<std::mutex> lk(stats_mu_);
            if (s == static_cast<ssize_t>(bytes.size())) {
                stats_.bytes_out += bytes.size();
            } else {
                stats_.drops += 1;
            }
        };
        const std::uint32_t base_ssrc =
            cfg_.fec.ssrc != 0
              ? cfg_.fec.ssrc
              : static_cast<std::uint32_t>(
                    std::chrono::steady_clock::now().time_since_epoch().count());
        fec_encoder_ = std::make_unique<fec::FecEncoder>(
            cfg_.fec,
            base_ssrc,
            base_ssrc + 1u,
            base_ssrc + 2u,
            [media, sendOn](std::span<const std::uint8_t> p) { sendOn(media, p); },
            [col,   sendOn](std::span<const std::uint8_t> p) { sendOn(col,   p); },
            cfg_.fec.mode == FecCfg::Mode::TwoD
                ? fec::FecEncoder::PacketSink(
                      [row, sendOn](std::span<const std::uint8_t> p) {
                          sendOn(row, p);
                      })
                : fec::FecEncoder::PacketSink{});
    }

    // Reset PSI state for clean rediscovery.
    psi_assembler_.clearAll();
    input_program_table_.clear();
    last_input_snapshot_.reset();
    input_video_pid_ = 0xFFFF;
    input_audio_pid_ = 0xFFFF;
    input_video_stream_type_ = 0;
    input_audio_stream_type_ = 0;
    out_video_cc_ = out_audio_cc_ = 0;
    video_pes_.reset();
    audio_pes_.reset();

    stop_flag_.store(false, std::memory_order_release);
    running_.store(true,  std::memory_order_release);
    io_thread_ = std::thread([this] { runIoThread(); });

    if (logger_) {
        logger_->info("TranscodeGateway[{}/{}]: started, in={}:{} → out={}:{}",
                      id_, name_, cfg_.input.address, cfg_.input.port,
                      output_cfg_.address, output_cfg_.port);
    }
    return true;
}

void TranscodeGateway::stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) return;
    stop_flag_.store(true, std::memory_order_release);
    if (io_thread_.joinable()) io_thread_.join();
    if (fec_encoder_) {
        fec_encoder_->flush();
        fec_encoder_.reset();
    }
    sockets_.reset();
    if (logger_) logger_->info("TranscodeGateway[{}/{}]: stopped", id_, name_);
}

GatewayStats TranscodeGateway::getStats() const {
    std::lock_guard<std::mutex> lk(stats_mu_);
    return stats_;
}

nlohmann::json TranscodeGateway::statusJson() const {
    const auto s = getStats();
    nlohmann::json input_program = nlohmann::json::object();
    if (auto snap = input_program_table_.current(); snap && !snap->programs.empty()) {
        const auto& p = snap->programs.front();
        nlohmann::json streams = nlohmann::json::array();
        for (const auto& es : p.streams) {
            streams.push_back({
                {"stream_type",    es.stream_type},
                {"elementary_pid", es.elementary_pid},
                {"language",       es.language},
                {"codec_name",     es.codec_name},
            });
        }
        input_program = {
            {"service_id",   p.service_id},
            {"pmt_pid",      p.pmt_pid},
            {"pcr_pid",      p.pcr_pid},
            {"discovered",   p.discovered},
            {"service_name", p.service_name},
            {"provider_name", p.provider_name},
            {"streams",      streams},
        };
    }
    nlohmann::json pipeline_stats = nullptr;
    if (pipeline_) pipeline_stats = pipeline_->statsJson();
    nlohmann::json fec_json = toJson(cfg_.fec);
    if (cfg_.fec.enabled && fec_encoder_) {
        fec_json["media_rtp_emitted"]  = fec_encoder_->mediaRtpEmitted();
        fec_json["column_fec_emitted"] = fec_encoder_->columnFecEmitted();
        fec_json["row_fec_emitted"]    = fec_encoder_->rowFecEmitted();
    }
    return nlohmann::json{
        {"id",         id_},
        {"name",       name_},
        {"mode",       "transcode"},
        {"running",    running_.load(std::memory_order_acquire)},
        {"input",      toJson(cfg_.input)},
        {"output",     toJson(output_cfg_)},
        {"transcode",  toJson(cfg_.transcode)},
        {"fec",        fec_json},
        {"input_program",   input_program},
        {"input_video_pid", input_video_pid_},
        {"input_audio_pid", input_audio_pid_},
        {"pipeline_attached", pipeline_ != nullptr},
        {"pipeline",   pipeline_stats},
        {"pkt_in",     s.pkt_in},
        {"bytes_in",   s.bytes_in},
        {"pkt_out",    s.pkt_out},
        {"bytes_out",  s.bytes_out},
        {"drops",      s.drops},
    };
}

void TranscodeGateway::setCfg(GatewayCfg cfg) {
    cfg_ = std::move(cfg);
    if (cfg_.outputs.size() != 1)
        throw std::invalid_argument(
            "TranscodeGateway: exactly one output required");
    output_cfg_ = cfg_.outputs.front();
}

// ─── Pipeline injection / registration ──────────────────────────────────────

void TranscodeGateway::setPipeline(
        std::unique_ptr<transcode::ITranscodePipeline> pipeline) {
    if (running_.load(std::memory_order_acquire)) {
        throw std::logic_error(
            "TranscodeGateway::setPipeline: cannot swap while running");
    }
    pipeline_ = std::move(pipeline);
    if (pipeline_) {
        // Hook the pipeline's TS-packet output back into the gateway. Each
        // packet is 188 bytes; the pipeline's PesPacketizer already set the
        // PID and CC, so we just append.
        pipeline_->setOutputSink(
            [this](std::span<const std::uint8_t> p) {
                if (p.size() != ts::kTsPacketSize) return;
                appendPreparedPacket(
                    std::span<const std::uint8_t, ts::kTsPacketSize>(
                        p.data(), ts::kTsPacketSize));
            });
        // Wire the assembler sinks. PUSI handling lives in PesAssembler; on
        // each fully-assembled PES we forward to the pipeline.
        video_pes_.setSink([this](ts::PesPacket pes) {
            if (pipeline_) pipeline_->feedVideoPes(std::move(pes));
        });
        audio_pes_.setSink([this](ts::PesPacket pes) {
            if (pipeline_) pipeline_->feedAudioPes(std::move(pes));
        });
    }
}

void TranscodeGateway::registerPipelineFactory(PipelineFactory factory) {
    std::lock_guard<std::mutex> lk(g_pipeline_factory_mu);
    g_pipeline_factory = std::move(factory);
}

bool TranscodeGateway::hasRegisteredPipelineFactory() noexcept {
    std::lock_guard<std::mutex> lk(g_pipeline_factory_mu);
    return static_cast<bool>(g_pipeline_factory);
}

// ─── Test seam ───────────────────────────────────────────────────────────────

void TranscodeGateway::testFeedDatagram(std::span<const std::uint8_t> bytes) {
    processInputDatagram(bytes);
}

void TranscodeGateway::testEmitPsi() { emitPsi(); }

std::vector<std::vector<std::uint8_t>> TranscodeGateway::testDrainOutput() {
    if (output_buffer_ && output_buffer_->fill > 0) {
        std::vector<std::uint8_t> v(output_buffer_->bytes.data(),
                                    output_buffer_->bytes.data() + output_buffer_->fill);
        output_test_queue_.push_back(std::move(v));
        output_buffer_->reset();
    }
    auto out = std::move(output_test_queue_);
    output_test_queue_.clear();
    return out;
}

// ─── Hot path ────────────────────────────────────────────────────────────────

void TranscodeGateway::processInputDatagram(std::span<const std::uint8_t> bytes) {
    {
        std::lock_guard<std::mutex> lk(stats_mu_);
        stats_.pkt_in   += 1;
        stats_.bytes_in += bytes.size();
    }
    const std::size_t whole = (bytes.size() / kTsPacketSize) * kTsPacketSize;
    for (std::size_t off = 0; off + kTsPacketSize <= whole; off += kTsPacketSize) {
        if (bytes[off] != ts::kTsSyncByte) continue;
        std::span<const std::uint8_t, kTsPacketSize> pkt(
            bytes.data() + off, kTsPacketSize);
        processInputTsPacket(pkt);
    }
    flushOutput();
}

void TranscodeGateway::processInputTsPacket(
        std::span<const std::uint8_t, kTsPacketSize> bytes) {
    TsPacketView v(bytes);
    if (!v.isValidSync()) return;
    const std::uint16_t pid = v.pid();
    if (pid == kPidNull) return;

    // PSI tap: PAT/SDT and any known PMT PID feed the assembler. We never
    // forward input PSI verbatim — the output gets a self-generated PAT/PMT/
    // SDT built from cfg.transcode.
    auto snap_for_pmt = input_program_table_.current();
    bool is_pmt_pid = false;
    if (snap_for_pmt) {
        for (const auto& p : snap_for_pmt->programs) {
            if (p.pmt_pid == pid) { is_pmt_pid = true; break; }
        }
    }
    if (pid == kPidPat || pid == kPidSdt || pid == kPidEit || is_pmt_pid) {
        psi_assembler_.feed(v, [this](std::uint16_t spid,
                                      std::span<const std::uint8_t> sec) {
            onPsiSection(spid, sec);
        });
        return;
    }

    // ES routing. Two modes:
    //   pipeline_ == nullptr → pass-through (PID/CC remap, no decode). Used
    //     by the unit-test build that doesn't link FFmpeg.
    //   pipeline_ != nullptr → feed the per-PID PesAssembler. The assembler
    //     hands fully-formed PES to the pipeline, which decodes, re-encodes,
    //     re-PESes, re-TS-packetises and emits via appendPreparedPacket().
    if (pid == input_video_pid_ && input_video_pid_ != 0xFFFF) {
        if (pipeline_) video_pes_.feed(v);
        else           emitOutputPacket(bytes, cfg_.transcode.video_pid, out_video_cc_);
    } else if (pid == input_audio_pid_ && input_audio_pid_ != 0xFFFF) {
        if (pipeline_) audio_pes_.feed(v);
        else           emitOutputPacket(bytes, cfg_.transcode.audio_pid, out_audio_cc_);
    }
    // All other PIDs (e.g. teletext, subtitles, EIT raw) are dropped — the
    // transcode gateway emits exactly one re-encoded A/V program.
}

void TranscodeGateway::onPsiSection(std::uint16_t pid,
                                    std::span<const std::uint8_t> sec) {
    if (pid == kPidPat) {
        if (auto pat = ts::parsePat(sec)) {
            input_program_table_.ingestPat(*pat);
            refreshInputBindings();
        }
        return;
    }
    if (pid == kPidSdt) {
        if (auto sdt = ts::parseSdt(sec))
            input_program_table_.ingestSdt(*sdt);
        return;
    }
    if (pid == kPidEit) {
        // Drop input EIT — we don't currently regenerate program guide for
        // a transcoded service. Step 6+ may re-emit a placeholder EIT
        // (present/following) once freeze/silence policy is in place.
        return;
    }
    if (auto pmt = ts::parsePmt(sec)) {
        input_program_table_.ingestPmt(*pmt);
        refreshInputBindings();
    }
}

void TranscodeGateway::refreshInputBindings() {
    auto snap = input_program_table_.current();
    if (!snap || snap->programs.empty()) return;
    if (last_input_snapshot_ == snap) return;
    last_input_snapshot_ = snap;

    // Pick the first discovered program; SPTS input typically carries one.
    // If multiple programs are present (mis-configured upstream), we still
    // pick the first — the operator-visible service_id in cfg.transcode is
    // the *output* identity, not a filter.
    const ts::ProgramInfo* prog = nullptr;
    for (const auto& p : snap->programs) {
        if (p.discovered) { prog = &p; break; }
    }
    if (!prog) return;

    // Reset, then walk ES list picking first video + first audio.
    input_video_pid_ = 0xFFFF;
    input_audio_pid_ = 0xFFFF;
    input_video_stream_type_ = 0;
    input_audio_stream_type_ = 0;
    for (const auto& es : prog->streams) {
        const std::uint8_t st = es.stream_type;
        const bool is_video = (st == 0x01 || st == 0x02 || st == 0x10
                               || st == 0x1B || st == 0x24);
        const bool is_audio = (st == 0x03 || st == 0x04 || st == 0x0F
                               || st == 0x11 || st == 0x81 || st == 0x06);
        if (is_video && input_video_pid_ == 0xFFFF) {
            input_video_pid_ = es.elementary_pid;
            input_video_stream_type_ = st;
        } else if (is_audio && input_audio_pid_ == 0xFFFF) {
            input_audio_pid_ = es.elementary_pid;
            input_audio_stream_type_ = st;
        }
    }

    // Per-PID assemblers must restart on every PMT change — the input PIDs
    // themselves may have moved (cable rebuild, encoder reset upstream).
    video_pes_.reset();
    audio_pes_.reset();

    // Tell the pipeline what input codecs to expect. Repeated calls are
    // idempotent unless the stream_type actually changed.
    if (pipeline_) {
        pipeline_->onInputStreamTypes(input_video_stream_type_,
                                      input_audio_stream_type_);
    }
}

void TranscodeGateway::emitOutputPacket(
        std::span<const std::uint8_t, kTsPacketSize> in_pkt,
        std::uint16_t out_pid,
        std::uint8_t& cc) {
    std::array<std::uint8_t, kTsPacketSize> rewritten;
    std::memcpy(rewritten.data(), in_pkt.data(), kTsPacketSize);
    ts::TsPacketMut m(std::span<std::uint8_t, kTsPacketSize>(
        rewritten.data(), kTsPacketSize));
    if (m.view().pid() != out_pid) m.setPid(out_pid);
    m.setContinuityCounter(cc);
    cc = static_cast<std::uint8_t>((cc + 1) & 0x0F);

    appendPreparedPacket(std::span<const std::uint8_t, kTsPacketSize>(
        rewritten.data(), kTsPacketSize));
}

void TranscodeGateway::appendPreparedPacket(
        std::span<const std::uint8_t, kTsPacketSize> pkt) {
    // FEC live path: each TS goes through the encoder (RTP-batched, fanned
    // out to media + column / row FEC sinks). Test path keeps raw-TS UDP
    // buffer behaviour so existing test seams stay deterministic.
    if (fec_encoder_ && running_.load(std::memory_order_acquire)) {
        fec_encoder_->feedTsPacket(
            std::span<const std::uint8_t>(pkt.data(), kTsPacketSize));
        std::lock_guard<std::mutex> lk(stats_mu_);
        stats_.pkt_out += 1;
        return;
    }
    if (!output_buffer_) output_buffer_ = std::make_unique<OutputBuffer>();
    output_buffer_->append(pkt);
    if (output_buffer_->full()) flushOutput();
}

void TranscodeGateway::flushOutput() {
    if (!output_buffer_ || output_buffer_->fill == 0) return;
    if (running_.load(std::memory_order_acquire) && sockets_) {
        const ssize_t s = ::sendto(
            sockets_->out_fd.get(),
            output_buffer_->bytes.data(), output_buffer_->fill,
            MSG_DONTWAIT,
            reinterpret_cast<sockaddr*>(&sockets_->out_addr),
            sizeof(sockaddr_in));
        std::lock_guard<std::mutex> lk(stats_mu_);
        if (s == static_cast<ssize_t>(output_buffer_->fill)) {
            stats_.pkt_out   += output_buffer_->fill / kTsPacketSize;
            stats_.bytes_out += output_buffer_->fill;
        } else {
            stats_.drops += output_buffer_->fill / kTsPacketSize;
        }
    } else {
        std::vector<std::uint8_t> v(output_buffer_->bytes.data(),
                                    output_buffer_->bytes.data()
                                        + output_buffer_->fill);
        output_test_queue_.push_back(std::move(v));
    }
    output_buffer_->reset();
}

// ─── PSI emission ────────────────────────────────────────────────────────────

void TranscodeGateway::emitPsi() {
    const auto& tc = cfg_.transcode;

    // --- PAT (single program)
    ts::PatBuildInput patIn;
    patIn.transport_stream_id = tc.transport_stream_id;
    patIn.version_number      = pat_version_;
    patIn.programs.push_back({tc.service_id, tc.pmt_pid});
    auto pat_sec = ts::buildPatSection(patIn);
    auto pat_pkts = ts::packetizeSection(kPidPat, pat_sec, pat_cc_);
    for (const auto& pk : pat_pkts) {
        appendPreparedPacket(std::span<const std::uint8_t, kTsPacketSize>(
            pk.data(), kTsPacketSize));
    }

    // --- PMT
    ts::PmtBuildInput pmtIn;
    pmtIn.program_number = tc.service_id;
    pmtIn.version_number = pmt_version_;
    pmtIn.pcr_pid        = tc.pcr_pid != 0 ? tc.pcr_pid : tc.video_pid;

    // When a transcode pipeline is attached the output codecs are fixed:
    //   video → 0x1B (H.264)   |   audio → 0x0F (AAC ADTS)
    // In pass-through mode (no pipeline; unit-test build) we mirror the
    // input's stream_type so PSI matches the bytes we forward verbatim.
    {
        ts::PmtStream sv;
        if (pipeline_) {
            sv.stream_type = 0x1B;
        } else {
            sv.stream_type = input_video_stream_type_ != 0
                           ? input_video_stream_type_ : 0x1B;
        }
        sv.elementary_pid = tc.video_pid;
        pmtIn.streams.push_back(std::move(sv));

        ts::PmtStream sa;
        if (pipeline_) {
            sa.stream_type = 0x0F;
        } else {
            sa.stream_type = input_audio_stream_type_ != 0
                           ? input_audio_stream_type_ : 0x0F;
        }
        sa.elementary_pid = tc.audio_pid;
        pmtIn.streams.push_back(std::move(sa));
    }
    auto pmt_sec  = ts::buildPmtSection(pmtIn);
    auto pmt_pkts = ts::packetizeSection(tc.pmt_pid, pmt_sec, pmt_cc_);
    for (const auto& pk : pmt_pkts) {
        appendPreparedPacket(std::span<const std::uint8_t, kTsPacketSize>(
            pk.data(), kTsPacketSize));
    }

    // --- SDT
    if (tc.emit_sdt) {
        ts::SdtBuildInput sdtIn;
        sdtIn.transport_stream_id  = tc.transport_stream_id;
        sdtIn.original_network_id  = tc.original_network_id;
        sdtIn.version_number       = sdt_version_;
        ts::SdtService svc;
        svc.service_id                 = tc.service_id;
        svc.eit_present_following_flag = false;
        svc.eit_schedule_flag          = false;
        svc.running_status             = 4;          // running
        svc.descriptors.push_back(ts::makeServiceDescriptor(
            serviceTypeForStreamTypes(input_video_stream_type_,
                                      input_audio_stream_type_),
            tc.provider_name,
            tc.service_name));
        sdtIn.services.push_back(std::move(svc));
        auto sdt_sec  = ts::buildSdtSection(sdtIn);
        auto sdt_pkts = ts::packetizeSection(kPidSdt, sdt_sec, sdt_cc_);
        for (const auto& pk : sdt_pkts) {
            appendPreparedPacket(std::span<const std::uint8_t, kTsPacketSize>(
                pk.data(), kTsPacketSize));
        }
    }

    flushOutput();
}

void TranscodeGateway::emitPsiIfDue(std::chrono::steady_clock::time_point now) {
    using namespace std::chrono;
    if (last_pat_emit_ == steady_clock::time_point{}) {
        last_pat_emit_ = last_pmt_emit_ = last_sdt_emit_ = now - hours(1);
    }
    const auto& tc = cfg_.transcode;
    const auto pat_due = now - last_pat_emit_ >= milliseconds(tc.pat_period_ms);
    const auto pmt_due = now - last_pmt_emit_ >= milliseconds(tc.pmt_period_ms);
    const auto sdt_due = now - last_sdt_emit_ >= milliseconds(tc.sdt_period_ms);
    if (pat_due || pmt_due || sdt_due) {
        emitPsi();
        if (pat_due) last_pat_emit_ = now;
        if (pmt_due) last_pmt_emit_ = now;
        if (sdt_due) last_sdt_emit_ = now;
    }
}

void TranscodeGateway::runIoThread() {
    if (numa_node_ >= 0) numa::bindCurrentThreadToNode(numa_node_);

    auto& lg = logger_ ? *logger_ : *spdlog::default_logger();

    std::array<std::uint8_t, kRecvBufBytes> buf{};
    while (!stop_flag_.load(std::memory_order_acquire)) {
        const ssize_t n = ::recv(sockets_->in_fd.get(), buf.data(), buf.size(), 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                // heartbeat — fall through
            } else {
                lg.error("TranscodeGateway[{}]: recv: {}", id_, std::strerror(errno));
                break;
            }
        } else if (n > 0) {
            processInputDatagram(std::span<const std::uint8_t>(
                buf.data(), static_cast<std::size_t>(n)));
        }
        const auto now = std::chrono::steady_clock::now();
        // Tick the pipeline regardless of whether a datagram arrived — the
        // loss-FSM (freeze/silence/fallback) needs the heartbeat to fire on
        // recv timeouts to make any progress at all.
        if (pipeline_) pipeline_->tick(now);
        emitPsiIfDue(now);
    }
    if (pipeline_) pipeline_->flush();
    flushOutput();
}

} // namespace liveqx::gateway
