#include "output/HlsOutput.h"

#include "output/HlsOrphanSweep.h"
#include "utils/CpuAffinity.h"
#include "utils/Log.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <thread>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
}

#include <cctype>

namespace liveqx::hls {

namespace fs = std::filesystem;

// ─── RAII for the FFmpeg pipeline ────────────────────────────────────────────
namespace {

// Mirrors the Pipeline pattern from RtmpOutput.cpp: input MPEG-TS demuxer
// fed by a custom AVIOContext sourcing bytes from our SPSC queue, output
// HLS muxer that writes into output_dir. Built and torn down once per
// run() invocation; if any step fails we destroy and mark unhealthy.
struct Pipeline {
    AVFormatContext* in_fmt         = nullptr;
    AVFormatContext* out_fmt        = nullptr;
    AVIOContext*     in_avio        = nullptr;
    bool             header_written = false;

    ~Pipeline() {
        if (out_fmt) {
            // HLS muxer's trailer flushes the final `#EXT-X-ENDLIST` tag —
            // only valid if we successfully wrote the header. HLS oformat
            // carries AVFMT_NOFILE, so no top-level pb to close ourselves;
            // segment / playlist IOContexts are owned by the muxer.
            if (header_written) av_write_trailer(out_fmt);
            avformat_free_context(out_fmt);
            out_fmt = nullptr;
        }
        if (in_fmt) {
            avformat_close_input(&in_fmt);
        }
        if (in_avio) {
            // The buffer we passed to avio_alloc_context may have been
            // av_realloc'd internally, so the original pointer is stale.
            // Free the *current* buffer via avio_ctx->buffer, then the ctx.
            av_freep(&in_avio->buffer);
            avio_context_free(&in_avio);
        }
    }
};

constexpr int kAvioBufBytes = 32 * 1024;     // FFmpeg-recommended starting size

} // namespace

// ─── Impl ────────────────────────────────────────────────────────────────────

struct HlsOutput::Impl {
    SpscQueue<Packet, 64> queue;

    std::vector<uint8_t> current_packet;
    size_t               current_offset = 0;

    std::jthread thread;
};

// ─── Construction / destruction ──────────────────────────────────────────────

HlsOutput::HlsOutput(HlsCfg cfg)
    : cfg_(std::move(cfg)),
      impl_(std::make_unique<Impl>()) {}

HlsOutput::~HlsOutput() { stop(); }

spdlog::logger& HlsOutput::lg() noexcept {
    return logger_ ? *logger_ : *spdlog::default_logger();
}

// ─── Lifecycle ───────────────────────────────────────────────────────────────

bool HlsOutput::start() {
    if (running_.exchange(true)) return false;

    // Validate output_dir up-front so the obvious operator errors fail at
    // start() rather than minutes later when the muxer can't write a
    // segment. The proper validator (incl. writability + abs-path checks)
    // ships in c3; here we only refuse non-existent / non-directory paths.
    std::error_code ec;
    if (cfg_.output_dir.empty()) {
        lg().error("HlsOutput: output_dir not set");
        running_.store(false);
        healthy_.store(false);
        return false;
    }
    if (!fs::is_directory(cfg_.output_dir, ec)) {
        lg().error("HlsOutput: output_dir '{}' is not a directory ({})",
                   cfg_.output_dir, ec.message());
        running_.store(false);
        healthy_.store(false);
        return false;
    }

    // Sweep crash-leftovers and stale segments before the muxer starts
    // numbering from 0 again — without this a fresh start() would publish
    // a playlist next to old `seg_NNNNN.ts` files and confuse any client
    // resuming from a cached MEDIA-SEQUENCE.
    sweepOrphans(cfg_, &lg());

    stop_io_.store(false, std::memory_order_release);
    healthy_.store(true,  std::memory_order_release);

    impl_->thread = std::jthread([this](std::stop_token st) { runThread(st); });
    started_.store(true, std::memory_order_release);
    lg().info("HlsOutput: started output_dir={} segdur={}s playlist_size={}",
              cfg_.output_dir, cfg_.segment_duration_sec, cfg_.playlist_size);
    return true;
}

void HlsOutput::stop() {
    if (!running_.exchange(false)) return;
    stop_io_.store(true, std::memory_order_release);

    if (impl_->thread.joinable()) {
        impl_->thread.request_stop();
        impl_->thread.join();
    }
    started_.store(false, std::memory_order_release);
    lg().info("HlsOutput: stopped");
}

void HlsOutput::send(const Packet& pkt) {
    if (!running_.load(std::memory_order_relaxed)) {
        packets_dropped_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!impl_->queue.push(pkt)) {
        // Queue full — slow disk, ENOSPC, or the writer thread parked in
        // av_write_frame for >a few segments. Drop and surface via stats.
        packets_dropped_.fetch_add(1, std::memory_order_relaxed);
    }
}

bool HlsOutput::isHealthy() const {
    // Three layers, in reverse order of cheapness:
    //   1. healthy_ flag — flipped false on write_frame error (ENOSPC/EROFS).
    //   2. started_     — stop()/start() invariant; pre-start the driver
    //                     is technically idle but not "broken".
    //   3. staleness    — once we have any packet through, refuse to
    //                     report healthy if the writer has been quiet for
    //                     > 2*segment_duration_sec. That's the same
    //                     2-segment grace nginx uses to decide a stream
    //                     went dead.
    if (!healthy_.load(std::memory_order_acquire)) return false;
    if (!started_.load(std::memory_order_acquire)) return false;

    const auto last_ns = last_packet_write_ns_.load(std::memory_order_acquire);
    if (last_ns == 0) return true;   // not yet started writing — give it slack

    const auto now_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    const uint64_t budget_ns =
        static_cast<uint64_t>(cfg_.segment_duration_sec) * 2ULL * 1'000'000'000ULL;
    return (now_ns - last_ns) < budget_ns;
}

OutputStats HlsOutput::getStats() const {
    OutputStats s;
    s.packets_sent = packets_sent_.load(std::memory_order_relaxed);
    s.bytes_sent   = bytes_sent_.load(std::memory_order_relaxed);
    return s;
}

nlohmann::json HlsOutput::statusJson() const {
    // Segment metrics are derived from the filesystem on demand rather
    // than tracked from inside the muxer pipeline. FFmpeg doesn't expose
    // a public per-segment callback (the private struct dance is brittle
    // across versions), and inotify would add a thread we don't need —
    // a status read is on a slow path, an opendir + scan is cheap.
    namespace cfs = std::filesystem;
    int      segs_present = 0;
    std::string latest_name;
    cfs::file_time_type latest_mtime{};
    if (!cfg_.output_dir.empty()) {
        const auto parts = splitPattern(cfg_.segment_filename_pattern);
        std::error_code ec;
        cfs::directory_iterator it(cfg_.output_dir, ec);
        if (!ec) {
            for (const auto& entry : it) {
                std::error_code lec;
                if (!entry.is_regular_file(lec)) continue;
                const auto name = entry.path().filename().string();
                if (!matchesSegmentName(name, parts)) continue;
                ++segs_present;
                const auto m = cfs::last_write_time(entry.path(), lec);
                if (lec) continue;
                if (latest_name.empty() || m > latest_mtime) {
                    latest_name  = name;
                    latest_mtime = m;
                }
            }
        }
    }

    int64_t last_segment_age_ms = -1;
    if (!latest_name.empty()) {
        const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
            cfs::file_time_type::clock::now() - latest_mtime);
        last_segment_age_ms = age.count();
    }

    int64_t last_packet_age_ms = -1;
    const auto last_pw = last_packet_write_ns_.load(std::memory_order_acquire);
    if (last_pw != 0) {
        const auto now_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        last_packet_age_ms = static_cast<int64_t>((now_ns - last_pw) / 1'000'000ULL);
    }

    return nlohmann::json{
        {"transport",             "hls"},
        {"output_dir",            cfg_.output_dir},
        {"playlist_path",
            cfg_.output_dir.empty() ? std::string{}
                                    : cfg_.output_dir + "/" + cfg_.playlist_filename},
        {"segment_duration_sec",  cfg_.segment_duration_sec},
        {"playlist_size",         cfg_.playlist_size},
        {"packets_sent",          packets_sent_.load(std::memory_order_relaxed)},
        {"bytes_sent",            bytes_sent_.load(std::memory_order_relaxed)},
        {"packets_dropped",       packets_dropped_.load(std::memory_order_relaxed)},
        {"segments_present",      segs_present},
        {"last_segment",          latest_name},
        {"last_segment_age_ms",   last_segment_age_ms},
        {"last_packet_age_ms",    last_packet_age_ms},
        {"healthy",               isHealthy()},
        {"started",               started_.load(std::memory_order_acquire)},
    };
}

// ─── AVIO read callback ──────────────────────────────────────────────────────

int HlsOutput::readPacketCb(void* opaque, uint8_t* buf, int buf_size) {
    auto* self = static_cast<HlsOutput*>(opaque);
    return self->readBytes(buf, buf_size);
}

int HlsOutput::readBytes(uint8_t* buf, int buf_size) {
    using namespace std::chrono_literals;
    while (true) {
        if (stop_io_.load(std::memory_order_acquire)) return AVERROR_EOF;

        if (impl_->current_packet.empty()) {
            auto p = impl_->queue.pop();
            if (!p) {
                // Block briefly; the mpegts demuxer treats sync read=0 as EOF
                // on some versions.
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

void HlsOutput::runThread(std::stop_token st) {
    numa::bindCurrentThreadToNode(numa_node_);
    Log::setThreadName(
        "ch" + (channel_id_.empty() ? std::string("?") : channel_id_) + "-hls-tx");

    Pipeline pipe;

    // ── AVIO over our SPSC queue ─────────────────────────────────────────
    // Buffer ownership transfers to the AVIOContext; libavformat may
    // av_realloc it internally during reads, so we never hold the pointer
    // after avio_alloc_context succeeds. The destructor walks
    // avio_ctx->buffer for the up-to-date pointer.
    auto* in_buf = static_cast<uint8_t*>(av_malloc(kAvioBufBytes));
    if (!in_buf) {
        lg().error("HlsOutput: av_malloc(avio_buf) failed");
        healthy_.store(false, std::memory_order_release);
        return;
    }
    pipe.in_avio = avio_alloc_context(
        in_buf, kAvioBufBytes,
        /*write_flag=*/0, /*opaque=*/this,
        &HlsOutput::readPacketCb, /*write=*/nullptr, /*seek=*/nullptr);
    if (!pipe.in_avio) {
        av_free(in_buf);
        lg().error("HlsOutput: avio_alloc_context failed");
        healthy_.store(false, std::memory_order_release);
        return;
    }

    // ── MPEG-TS demuxer ──────────────────────────────────────────────────
    pipe.in_fmt = avformat_alloc_context();
    if (!pipe.in_fmt) {
        lg().error("HlsOutput: avformat_alloc_context (in) failed");
        healthy_.store(false, std::memory_order_release);
        return;
    }
    pipe.in_fmt->pb     = pipe.in_avio;
    pipe.in_fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
    pipe.in_fmt->probesize            = 64 * 1024;
    pipe.in_fmt->max_analyze_duration = 2 * AV_TIME_BASE;

    const AVInputFormat* in_format = av_find_input_format("mpegts");
    if (!in_format) {
        lg().error("HlsOutput: av_find_input_format(mpegts) returned null");
        healthy_.store(false, std::memory_order_release);
        return;
    }
    int rc = avformat_open_input(&pipe.in_fmt, nullptr, in_format, nullptr);
    if (rc < 0) {
        lg().warn("HlsOutput: avformat_open_input(mpegts) failed (rc={})", rc);
        healthy_.store(false, std::memory_order_release);
        return;
    }
    if (avformat_find_stream_info(pipe.in_fmt, nullptr) < 0) {
        lg().warn("HlsOutput: avformat_find_stream_info failed");
        healthy_.store(false, std::memory_order_release);
        return;
    }

    // ── HLS muxer ────────────────────────────────────────────────────────
    const std::string playlist_path =
        cfg_.output_dir + "/" + cfg_.playlist_filename;
    rc = avformat_alloc_output_context2(&pipe.out_fmt, nullptr,
                                        "hls", playlist_path.c_str());
    if (rc < 0 || !pipe.out_fmt) {
        lg().error("HlsOutput: alloc_output_context2(hls) failed (rc={})", rc);
        healthy_.store(false, std::memory_order_release);
        return;
    }

    // Mirror input streams 1:1; HLS muxer wraps an internal mpegts muxer
    // for segments, so codec_tag must be cleared and codecpar copied as-is.
    std::vector<int> in_to_out(pipe.in_fmt->nb_streams, -1);
    bool stream_setup_ok = true;
    for (unsigned i = 0; i < pipe.in_fmt->nb_streams; ++i) {
        const AVStream* in_st = pipe.in_fmt->streams[i];
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
        out_st->codecpar->codec_tag = 0;
        in_to_out[i] = out_st->index;
    }
    if (!stream_setup_ok || pipe.out_fmt->nb_streams == 0) {
        lg().error("HlsOutput: no compatible (h264+aac) streams in input");
        healthy_.store(false, std::memory_order_release);
        return;
    }

    // ── HLS muxer options ───────────────────────────────────────────────
    // hls_flags=temp_file gives us atomic rename on segment + playlist
    // updates so concurrent nginx readers never see a partial file
    // (POSIX rename is atomic on a single FS). delete_segments lets the
    // muxer reap segments that fall out of the sliding window.
    AVDictionary* mux_opts = nullptr;
    av_dict_set    (&mux_opts, "hls_time",
                    std::to_string(cfg_.segment_duration_sec).c_str(), 0);
    av_dict_set    (&mux_opts, "hls_list_size",
                    std::to_string(cfg_.playlist_size).c_str(), 0);
    const std::string seg_pattern =
        cfg_.output_dir + "/" + cfg_.segment_filename_pattern;
    av_dict_set    (&mux_opts, "hls_segment_filename", seg_pattern.c_str(), 0);
    av_dict_set    (&mux_opts, "hls_flags",
                    "delete_segments+independent_segments+program_date_time+temp_file", 0);
    if (cfg_.segment_duration_sec > 0) {
        const int delete_threshold =
            std::max(1, cfg_.delete_segments_after_sec / cfg_.segment_duration_sec);
        av_dict_set_int(&mux_opts, "hls_delete_threshold", delete_threshold, 0);
    }

    if (avformat_write_header(pipe.out_fmt, &mux_opts) < 0) {
        lg().error("HlsOutput: write_header failed for {}", playlist_path);
        av_dict_free(&mux_opts);
        healthy_.store(false, std::memory_order_release);
        return;
    }
    av_dict_free(&mux_opts);
    pipe.header_written = true;

    lg().info("HlsOutput: muxer initialized, writing to {}", playlist_path);

    // ── Relay until error or stop ────────────────────────────────────────
    AVPacket* pkt = av_packet_alloc();
    while (!st.stop_requested() && running_.load(std::memory_order_relaxed)) {
        int read_rc = av_read_frame(pipe.in_fmt, pkt);
        if (read_rc < 0) {
            lg().warn("HlsOutput: av_read_frame rc={} — exiting writer", read_rc);
            break;
        }

        const int orig_idx = pkt->stream_index;
        const int mapped   = (orig_idx >= 0 && orig_idx < static_cast<int>(in_to_out.size()))
                                 ? in_to_out[orig_idx] : -1;
        if (mapped < 0) {
            av_packet_unref(pkt);
            continue;
        }
        pkt->stream_index = mapped;
        av_packet_rescale_ts(pkt,
            pipe.in_fmt->streams[orig_idx]->time_base,
            pipe.out_fmt->streams[mapped]->time_base);
        pkt->pos = -1;

        const int sent_size = pkt->size;
        int write_rc = av_interleaved_write_frame(pipe.out_fmt, pkt);
        if (write_rc < 0) {
            // ENOSPC, EROFS, or muxer-internal error. Mark unhealthy and
            // exit the loop; operator must restart the output. The current
            // segment may be partial — temp_file flag means it never got
            // renamed into place, so nginx will still serve the previous
            // playlist consistently.
            lg().error("HlsOutput: write_frame rc={} — marking unhealthy", write_rc);
            healthy_.store(false, std::memory_order_release);
            break;
        }
        packets_sent_.fetch_add(1, std::memory_order_relaxed);
        bytes_sent_.fetch_add(static_cast<uint64_t>(std::max(0, sent_size)),
                              std::memory_order_relaxed);
        const auto now_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        last_packet_write_ns_.store(now_ns, std::memory_order_release);
    }
    av_packet_free(&pkt);

    // pipe destructor writes trailer (#EXT-X-ENDLIST) + frees everything.
}

} // namespace liveqx::hls
