#include "output/RtmpOutput.h"
#include "utils/CpuAffinity.h"
#include "utils/Log.h"
#include "utils/UrlSanitize.h"

#include <chrono>
#include <cstring>
#include <thread>

#include <spdlog/spdlog.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
}

namespace liveqx::rtmp {

// ─── RAII for the relay's FFmpeg state ───────────────────────────────────────

namespace {

// We can't reuse the deleter pattern from MulticastInput verbatim because
// the output context is allocated by avformat_alloc_output_context2 and must
// be torn down in two steps (close pb, then free context). The Pipeline
// struct below owns the pair as a unit so an early-return cleans up cleanly.
struct Pipeline {
    AVFormatContext* in_fmt          = nullptr;
    AVFormatContext* out_fmt         = nullptr;
    AVIOContext*     in_avio         = nullptr;
    uint8_t*         in_buf          = nullptr;     // owned by us, NOT by avio_alloc
    bool             header_written  = false;       // write trailer iff true

    ~Pipeline() {
        if (out_fmt) {
            if (out_fmt->pb) {
                // Trailer writes a final FLV tag — only valid if we passed
                // write_header. Otherwise the muxer state is half-initialized
                // and av_write_trailer would dereference null tables.
                if (header_written) av_write_trailer(out_fmt);
                avio_closep(&out_fmt->pb);
            }
            avformat_free_context(out_fmt);
            out_fmt = nullptr;
        }
        if (in_fmt) {
            avformat_close_input(&in_fmt);
        }
        if (in_avio) {
            avio_context_free(&in_avio);
        }
        if (in_buf) {
            av_free(in_buf);
            in_buf = nullptr;
        }
    }
};

constexpr int kAvioBufBytes = 32 * 1024;     // FFmpeg-recommended starting size

} // namespace

// Stream keys live in the URL path on every RTMP CDN, so every log line
// and statusJson field routes through the shared sanitizer in
// utils/UrlSanitize.h instead of formatting cfg_.url directly.
using liveqx::util::rtmpUrlForLogs;

// ─── Impl ────────────────────────────────────────────────────────────────────

struct RtmpOutput::Impl {
    SpscQueue<Packet, 64> queue;

    // Held by the writer thread; protects the read-callback's "current packet"
    // state from concurrent re-entry. The AVIO contract is single-threaded
    // per context, so in practice no contention — but the field needs to
    // outlive a single readBytes() call across many AVIO invocations.
    std::vector<uint8_t> current_packet;
    size_t               current_offset = 0;

    std::jthread thread;
};

// ─── Construction / destruction ──────────────────────────────────────────────

RtmpOutput::RtmpOutput(OutputCfg cfg)
    : cfg_(std::move(cfg)),
      impl_(std::make_unique<Impl>()),
      current_backoff_ms_(cfg_.reconnect_initial_ms) {}

RtmpOutput::~RtmpOutput() { stop(); }

spdlog::logger& RtmpOutput::lg() noexcept {
    return logger_ ? *logger_ : *spdlog::default_logger();
}

// ─── Lifecycle ───────────────────────────────────────────────────────────────

bool RtmpOutput::start() {
    if (running_.exchange(true)) return false;
    stop_io_.store(false, std::memory_order_release);

    impl_->thread = std::jthread([this](std::stop_token st) { runThread(st); });
    lg().info("RtmpOutput: started target={}", rtmpUrlForLogs(cfg_.url));
    return true;
}

void RtmpOutput::stop() {
    if (!running_.exchange(false)) return;
    stop_io_.store(true, std::memory_order_release);

    if (impl_->thread.joinable()) {
        impl_->thread.request_stop();
        impl_->thread.join();
    }
    connected_.store(false, std::memory_order_release);
    lg().info("RtmpOutput: stopped");
}

void RtmpOutput::send(const Packet& pkt) {
    if (!running_.load(std::memory_order_relaxed)) return;
    if (!impl_->queue.push(pkt)) {
        // Queue full — common when the upstream is slower than the encoder
        // (high RTT, throttled link, target rate-limiting). Drop the
        // packet and surface the count via statusJson. Step 4 will layer
        // a "stale-disconnect" drop policy on top of this baseline.
        packets_dropped_.fetch_add(1, std::memory_order_relaxed);
    }
}

bool RtmpOutput::isHealthy() const {
    return connected_.load(std::memory_order_acquire);
}

OutputStats RtmpOutput::getStats() const {
    OutputStats s;
    s.packets_sent = packets_sent_.load(std::memory_order_relaxed);
    s.bytes_sent   = bytes_sent_.load(std::memory_order_relaxed);
    return s;
}

nlohmann::json RtmpOutput::statusJson() const {
    auto j = IOutput::statusJson();
    j["transport"]        = "rtmp";
    j["host"]             = rtmpUrlForLogs(cfg_.url);   // sanitized — no key in path
    j["bind_address"]     = cfg_.bind_address;
    j["packets_dropped"]  = packets_dropped_.load(std::memory_order_relaxed);
    j["reconnect_count"]  = reconnect_count_.load(std::memory_order_relaxed);
    j["connected"]        = connected_.load(std::memory_order_acquire);
    return j;
}

// ─── Backoff ─────────────────────────────────────────────────────────────────

std::chrono::milliseconds RtmpOutput::nextBackoff() {
    const int now_ms = current_backoff_ms_;
    long long doubled = static_cast<long long>(now_ms) * 2;
    if (doubled > cfg_.reconnect_max_ms) doubled = cfg_.reconnect_max_ms;
    current_backoff_ms_ = static_cast<int>(doubled);
    return std::chrono::milliseconds(now_ms);
}

// ─── AVIO read callback ──────────────────────────────────────────────────────

int RtmpOutput::readPacketCb(void* opaque, uint8_t* buf, int buf_size) {
    auto* self = static_cast<RtmpOutput*>(opaque);
    return self->readBytes(buf, buf_size);
}

int RtmpOutput::readBytes(uint8_t* buf, int buf_size) {
    using namespace std::chrono_literals;
    while (true) {
        if (stop_io_.load(std::memory_order_acquire)) return AVERROR_EOF;

        if (impl_->current_packet.empty()) {
            auto p = impl_->queue.pop();
            if (!p) {
                // No packet ready — block briefly and retry. The MPEG-TS
                // demuxer expects this read to feel synchronous; returning
                // 0 here would be interpreted as EOF on some FFmpeg
                // versions and abort find_stream_info.
                std::this_thread::sleep_for(2ms);
                continue;
            }
            impl_->current_packet = std::move(p->data);
            impl_->current_offset = 0;
        }

        const size_t avail = impl_->current_packet.size() - impl_->current_offset;
        const int copy = static_cast<int>(std::min<size_t>(
            static_cast<size_t>(buf_size), avail));
        std::memcpy(buf, impl_->current_packet.data() + impl_->current_offset, copy);
        impl_->current_offset += static_cast<size_t>(copy);
        if (impl_->current_offset == impl_->current_packet.size()) {
            impl_->current_packet.clear();
            impl_->current_offset = 0;
        }
        return copy;
    }
}

// ─── Writer thread ───────────────────────────────────────────────────────────

void RtmpOutput::runThread(std::stop_token st) {
    numa::bindCurrentThreadToNode(numa_node_);
    Log::setThreadName("ch" + (channel_id_.empty() ? std::string("?") : channel_id_) + "-rtmp-tx");

    while (!st.stop_requested() && running_.load(std::memory_order_relaxed)) {
        // Stamp the start of "we are not connected" once per gap. Cleared
        // by the relay loop after a successful avformat_write_header.
        // Also covers initial-connect failures: the first iteration stamps
        // before any I/O attempt, so the queue can begin draining if the
        // target is unreachable for longer than drop_after_disconnect_sec.
        if (disconnect_start_ns_.load(std::memory_order_acquire) == 0) {
            const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            disconnect_start_ns_.store(now_ns, std::memory_order_release);
        }

        // ── Conditional stale-packet drop ─────────────────────────────────
        // Policy: drop_after_disconnect_sec == 0 → never drop on reconnect,
        // let SPSC overflow naturally bound the backlog (~64 packets, tens
        // of ms at typical bitrates). > 0 → once the gap exceeds the
        // threshold, discard the backlog so the fresh connection starts on
        // the next encoder keyframe rather than replaying old GOPs (which
        // most players render as macroblock garbage). Short blips ≤ the
        // threshold replay as-is.
        const int    drop_sec = cfg_.drop_after_disconnect_sec;
        const int64_t dc_start = disconnect_start_ns_.load(std::memory_order_acquire);
        if (drop_sec > 0 && dc_start != 0) {
            const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            const int64_t elapsed_ns = now_ns - dc_start;
            if (elapsed_ns >= static_cast<int64_t>(drop_sec) * 1'000'000'000LL) {
                size_t dropped = 0;
                impl_->current_packet.clear();
                impl_->current_offset = 0;
                while (impl_->queue.pop()) { ++dropped; }
                if (dropped > 0) {
                    packets_dropped_.fetch_add(dropped, std::memory_order_relaxed);
                    lg().info("RtmpOutput: dropped {} stale packets after {}s disconnect",
                              dropped, elapsed_ns / 1'000'000'000LL);
                }
            }
        }

        Pipeline pipe;

        // ── Allocate AVIO over our SPSC queue ─────────────────────────────
        pipe.in_buf = static_cast<uint8_t*>(av_malloc(kAvioBufBytes));
        if (!pipe.in_buf) {
            lg().error("RtmpOutput: av_malloc(avio_buf) failed");
            std::this_thread::sleep_for(nextBackoff());
            continue;
        }
        pipe.in_avio = avio_alloc_context(
            pipe.in_buf, kAvioBufBytes,
            /*write_flag=*/0, /*opaque=*/this,
            &RtmpOutput::readPacketCb, /*write=*/nullptr, /*seek=*/nullptr);
        if (!pipe.in_avio) {
            lg().error("RtmpOutput: avio_alloc_context failed");
            // pipe destructor will av_free(in_buf).
            std::this_thread::sleep_for(nextBackoff());
            continue;
        }

        // ── Open MPEG-TS input demuxer ────────────────────────────────────
        pipe.in_fmt = avformat_alloc_context();
        if (!pipe.in_fmt) {
            lg().error("RtmpOutput: avformat_alloc_context (in) failed");
            std::this_thread::sleep_for(nextBackoff());
            continue;
        }
        pipe.in_fmt->pb     = pipe.in_avio;
        pipe.in_fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
        // Keep find_stream_info latency low — we already know the streams
        // are H.264 + AAC, so 2s of analysis is overkill but cheap.
        pipe.in_fmt->probesize            = 64 * 1024;
        pipe.in_fmt->max_analyze_duration = 2 * AV_TIME_BASE;

        const AVInputFormat* in_format = av_find_input_format("mpegts");
        if (!in_format) {
            lg().error("RtmpOutput: av_find_input_format(mpegts) returned null");
            std::this_thread::sleep_for(nextBackoff());
            continue;
        }
        int rc = avformat_open_input(&pipe.in_fmt, nullptr, in_format, nullptr);
        if (rc < 0) {
            lg().warn("RtmpOutput: avformat_open_input(mpegts) failed (rc={})", rc);
            std::this_thread::sleep_for(nextBackoff());
            continue;
        }
        if (avformat_find_stream_info(pipe.in_fmt, nullptr) < 0) {
            lg().warn("RtmpOutput: avformat_find_stream_info failed");
            std::this_thread::sleep_for(nextBackoff());
            continue;
        }

        // ── Open FLV output ───────────────────────────────────────────────
        rc = avformat_alloc_output_context2(&pipe.out_fmt, nullptr,
                                            cfg_.container.c_str(),
                                            cfg_.url.c_str());
        if (rc < 0 || !pipe.out_fmt) {
            lg().warn("RtmpOutput: alloc_output_context2(flv) failed (rc={})", rc);
            std::this_thread::sleep_for(nextBackoff());
            continue;
        }

        // Mirror input streams 1:1 in the output. We don't transcode; we
        // copy codecpar so the FLV muxer can wrap the same compressed
        // bitstreams the encoder produced.
        std::vector<int> in_to_out(pipe.in_fmt->nb_streams, -1);
        bool stream_setup_ok = true;
        for (unsigned i = 0; i < pipe.in_fmt->nb_streams; ++i) {
            const AVStream* in_st = pipe.in_fmt->streams[i];
            // FLV supports H.264 video + AAC audio; skip anything else
            // (e.g. subtitle PIDs in passthrough scenarios) so the muxer
            // doesn't reject the write_header.
            const auto codec_id = in_st->codecpar->codec_id;
            const bool is_h264 = (in_st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO
                                  && codec_id == AV_CODEC_ID_H264);
            const bool is_aac  = (in_st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO
                                  && codec_id == AV_CODEC_ID_AAC);
            if (!is_h264 && !is_aac) continue;

            AVStream* out_st = avformat_new_stream(pipe.out_fmt, nullptr);
            if (!out_st) { stream_setup_ok = false; break; }
            if (avcodec_parameters_copy(out_st->codecpar, in_st->codecpar) < 0) {
                stream_setup_ok = false;
                break;
            }
            out_st->codecpar->codec_tag = 0;       // let muxer pick
            in_to_out[i] = out_st->index;
        }
        if (!stream_setup_ok || pipe.out_fmt->nb_streams == 0) {
            lg().warn("RtmpOutput: no compatible (h264+aac) streams in input");
            std::this_thread::sleep_for(nextBackoff());
            continue;
        }

        // Open the actual RTMP socket. This is the call that hits the
        // network — failure means the target is unreachable.
        AVDictionary* io_opts = nullptr;
        av_dict_set(&io_opts, "rtmp_live", "live", 0);
        // rw_timeout limits the connect+handshake; no setting means infinite.
        av_dict_set(&io_opts, "rw_timeout", "5000000", 0);   // 5 s
        // Source-NIC pin: FFmpeg's tcp protocol honours `localaddr` to bind
        // the source IPv4 before connect(). Propagates through the rtmp and
        // tls layers transparently, so the same key works for rtmp:// and
        // rtmps://. Empty cfg_.bind_address = no key set = OS default route.
        if (!cfg_.bind_address.empty())
            av_dict_set(&io_opts, "localaddr", cfg_.bind_address.c_str(), 0);
        // TLS for rtmps:// — pass through to FFmpeg's tls protocol layer.
        // Skipping these for plain rtmp:// keeps the dictionary clean and
        // avoids dragging the openssl backend into the connect path.
        if (cfg_.url.rfind("rtmps://", 0) == 0) {
            av_dict_set(&io_opts, "tls_verify", cfg_.tls_verify ? "1" : "0", 0);
            if (!cfg_.tls_ca_file.empty())
                av_dict_set(&io_opts, "ca_file", cfg_.tls_ca_file.c_str(), 0);
        }

        rc = avio_open2(&pipe.out_fmt->pb, cfg_.url.c_str(),
                        AVIO_FLAG_WRITE, nullptr, &io_opts);
        av_dict_free(&io_opts);
        if (rc < 0) {
            lg().warn("RtmpOutput: avio_open2({}) failed (rc={})",
                      rtmpUrlForLogs(cfg_.url), rc);
            // Reset the output's pb so the Pipeline destructor doesn't try to
            // av_write_trailer over a non-existent file.
            pipe.out_fmt->pb = nullptr;
            std::this_thread::sleep_for(nextBackoff());
            continue;
        }

        if (avformat_write_header(pipe.out_fmt, nullptr) < 0) {
            lg().warn("RtmpOutput: write_header failed against {}",
                      rtmpUrlForLogs(cfg_.url));
            std::this_thread::sleep_for(nextBackoff());
            continue;
        }
        pipe.header_written = true;

        // ── Connected; relay until error or stop ──────────────────────────
        connected_.store(true, std::memory_order_release);
        current_backoff_ms_ = cfg_.reconnect_initial_ms;   // reset ladder
        disconnect_start_ns_.store(0, std::memory_order_release);
        if (reconnect_count_.load() > 0)
            lg().info("RtmpOutput: re-connected to {}", rtmpUrlForLogs(cfg_.url));
        else
            lg().info("RtmpOutput: connected to {}", rtmpUrlForLogs(cfg_.url));

        AVPacket* pkt = av_packet_alloc();
        bool relay_ok = true;
        while (!st.stop_requested()
                   && running_.load(std::memory_order_relaxed)
                   && relay_ok) {
            int read_rc = av_read_frame(pipe.in_fmt, pkt);
            if (read_rc < 0) {
                lg().warn("RtmpOutput: av_read_frame rc={} — disconnecting", read_rc);
                relay_ok = false;
                break;
            }

            const int orig_idx = pkt->stream_index;
            const int mapped   = (orig_idx >= 0 && orig_idx < static_cast<int>(in_to_out.size()))
                                     ? in_to_out[orig_idx] : -1;
            if (mapped < 0) {
                av_packet_unref(pkt);
                continue;   // not a stream we kept
            }
            pkt->stream_index = mapped;
            av_packet_rescale_ts(pkt,
                pipe.in_fmt->streams[orig_idx]->time_base,
                pipe.out_fmt->streams[mapped]->time_base);
            pkt->pos = -1;

            const int sent_size = pkt->size;
            int write_rc = av_interleaved_write_frame(pipe.out_fmt, pkt);
            // av_interleaved_write_frame frees pkt internally; no unref needed.
            if (write_rc < 0) {
                lg().warn("RtmpOutput: write_frame rc={} — disconnecting", write_rc);
                relay_ok = false;
                break;
            }
            packets_sent_.fetch_add(1, std::memory_order_relaxed);
            bytes_sent_.fetch_add(static_cast<uint64_t>(std::max(0, sent_size)),
                                  std::memory_order_relaxed);
        }
        av_packet_free(&pkt);
        connected_.store(false, std::memory_order_release);

        // pipe destructor writes trailer + frees everything.
        if (st.stop_requested() || !running_.load(std::memory_order_relaxed))
            break;

        reconnect_count_.fetch_add(1, std::memory_order_relaxed);
        const auto wait = nextBackoff();
        lg().info("RtmpOutput: reconnect to {} in {}ms",
                  rtmpUrlForLogs(cfg_.url), wait.count());
        const auto deadline = std::chrono::steady_clock::now() + wait;
        while (!st.stop_requested()
                   && std::chrono::steady_clock::now() < deadline
                   && running_.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

} // namespace liveqx::rtmp
