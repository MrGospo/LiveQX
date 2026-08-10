#include "clips/VideoClip.h"
#include "utils/Log.h"
#include "utils/CpuAffinity.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

// ─── RAII wrappers ────────────────────────────────────────────────────────────

namespace {

struct FmtCtxDeleter   { void operator()(AVFormatContext* p) const noexcept { if (p) avformat_close_input(&p); } };
struct CodecCtxDeleter { void operator()(AVCodecContext*  p) const noexcept { if (p) avcodec_free_context(&p); } };
struct AvFrameDeleter  { void operator()(AVFrame*         p) const noexcept { if (p) av_frame_free(&p); } };
struct SwsCtxDeleter   { void operator()(SwsContext*      p) const noexcept { if (p) sws_freeContext(p); } };
struct SwrCtxDeleter   { void operator()(SwrContext*      p) const noexcept { if (p) swr_free(&p); } };

using FmtCtxPtr   = std::unique_ptr<AVFormatContext, FmtCtxDeleter>;
using CodecCtxPtr = std::unique_ptr<AVCodecContext,  CodecCtxDeleter>;
using AvFramePtr  = std::unique_ptr<AVFrame,         AvFrameDeleter>;
using SwsCtxPtr   = std::unique_ptr<SwsContext,      SwsCtxDeleter>;
using SwrCtxPtr   = std::unique_ptr<SwrContext,      SwrCtxDeleter>;

} // namespace

struct VideoClip::Impl {
    FmtCtxPtr   fmt_ctx;
    CodecCtxPtr video_ctx;
    CodecCtxPtr audio_ctx;
    SwsCtxPtr   sws_ctx;
    SwrCtxPtr   swr_ctx;
    int         video_stream_idx = -1;
    int         audio_stream_idx = -1;
    AVRational  video_tb{};
    AVRational  audio_tb{};

    static constexpr int kOutRate = 48000;
    static constexpr int kOutCh   = 2;
};

// ─── VideoClip ────────────────────────────────────────────────────────────────

VideoClip::VideoClip(const std::string& path, int out_width, int out_height,
                     std::shared_ptr<FramePool> pool,
                     int output_fps,
                     std::unique_ptr<IPacketReader> reader)
    : path_(path), out_width_(out_width), out_height_(out_height),
      output_fps_(output_fps),
      pool_(std::move(pool)),
      packet_reader_(reader ? std::move(reader)
                            : std::make_unique<IPacketReader>()) {}

VideoClip::~VideoClip() {
    // fix31 — log dtor wall-clock so field logs make any non-graveyard drop
    // visible (the bury() worker thread is acceptable; the render thread is
    // not). Tagged with thread id; correlate against RenderLoop's logged
    // thread id to spot regressions.
    using Clock = std::chrono::steady_clock;
    const bool had_impl = (impl_ != nullptr);
    const auto t0       = Clock::now();
    release();
    if (had_impl && logger_) {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            Clock::now() - t0).count();
        if (ms >= 5) {
            // 5ms threshold filters out trivial cleanups; only the heavy
            // stopThreads() path crosses it.
            std::ostringstream oss;
            oss << std::this_thread::get_id();
            lg().info("VideoClip '{}': dtor took {}ms on tid={}",
                      path_, ms, oss.str());
        }
    }
}

spdlog::logger& VideoClip::lg() noexcept {
    return logger_ ? *logger_ : *spdlog::default_logger();
}

void VideoClip::prepare() {
    const auto t_prepare_start = std::chrono::steady_clock::now();
    lg().info("VideoClip '{}': prepare() start", path_);
    release();

    auto impl = std::make_unique<Impl>();

    {
        AVFormatContext* raw = nullptr;
        if (avformat_open_input(&raw, path_.c_str(), nullptr, nullptr) != 0)
            throw std::runtime_error("VideoClip: cannot open '" + path_ + "'");
        impl->fmt_ctx.reset(raw);
    }

    if (avformat_find_stream_info(impl->fmt_ctx.get(), nullptr) < 0)
        throw std::runtime_error("VideoClip: cannot read stream info: " + path_);

    for (unsigned i = 0; i < impl->fmt_ctx->nb_streams; ++i) {
        const auto t = impl->fmt_ctx->streams[i]->codecpar->codec_type;
        if (impl->video_stream_idx < 0 && t == AVMEDIA_TYPE_VIDEO)
            impl->video_stream_idx = static_cast<int>(i);
        if (impl->audio_stream_idx < 0 && t == AVMEDIA_TYPE_AUDIO)
            impl->audio_stream_idx = static_cast<int>(i);
    }

    if (impl->video_stream_idx < 0)
        throw std::runtime_error("VideoClip: no video stream in " + path_);

    // Codec opens (avcodec_open2) spawn FFmpeg's internal frame/slice
    // threading workers. Run them on a NUMA-pinned worker so those workers
    // inherit the channel's node affinity rather than the caller thread's.
    // numa_node_ < 0 makes runOnNode inline — current behavior preserved.
    numa::runOnNode(numa_node_, [&] {
        const AVStream* st  = impl->fmt_ctx->streams[impl->video_stream_idx];
        const auto*     par = st->codecpar;
        impl->video_tb      = st->time_base;

        const AVCodec* codec = avcodec_find_decoder(par->codec_id);
        if (!codec)
            throw std::runtime_error("VideoClip: unsupported video codec in " + path_);

        CodecCtxPtr ctx{avcodec_alloc_context3(codec)};
        if (!ctx)
            throw std::runtime_error("VideoClip: avcodec_alloc_context3 failed");
        if (avcodec_parameters_to_context(ctx.get(), par) < 0)
            throw std::runtime_error("VideoClip: avcodec_parameters_to_context failed");
        if (avcodec_open2(ctx.get(), codec, nullptr) < 0)
            throw std::runtime_error("VideoClip: avcodec_open2 failed for " + path_);

        const int src_w   = par->width  > 0 ? par->width  : 16;
        const int src_h   = par->height > 0 ? par->height : 16;
        const auto src_fmt = par->format >= 0
                                 ? static_cast<AVPixelFormat>(par->format)
                                 : AV_PIX_FMT_YUV420P;

        SwsCtxPtr sws{sws_getContext(
            src_w, src_h, src_fmt,
            out_width_, out_height_, AV_PIX_FMT_RGBA,
            SWS_BILINEAR, nullptr, nullptr, nullptr)};
        if (!sws)
            throw std::runtime_error("VideoClip: sws_getContext failed for " + path_);

        impl->video_ctx = std::move(ctx);
        impl->sws_ctx   = std::move(sws);
    });

    if (impl->audio_stream_idx >= 0) {
        numa::runOnNode(numa_node_, [&] {
            const AVStream*          st  = impl->fmt_ctx->streams[impl->audio_stream_idx];
            const AVCodecParameters* par = st->codecpar;
            impl->audio_tb = st->time_base;

            const AVCodec* codec = avcodec_find_decoder(par->codec_id);
            if (!codec) return;

            CodecCtxPtr ctx{avcodec_alloc_context3(codec)};
            if (!ctx
                    || avcodec_parameters_to_context(ctx.get(), par) < 0
                    || avcodec_open2(ctx.get(), codec, nullptr) < 0) {
                return;
            }

            AVChannelLayout in_layout{};
            if (ctx->ch_layout.nb_channels > 0)
                in_layout = ctx->ch_layout;
            else
                av_channel_layout_default(&in_layout,
                    par->ch_layout.nb_channels > 0 ? par->ch_layout.nb_channels : 1);

            AVChannelLayout out_layout = AV_CHANNEL_LAYOUT_STEREO;
            SwrContext* swr_raw = nullptr;
            const bool swr_ok =
                swr_alloc_set_opts2(&swr_raw,
                    &out_layout, AV_SAMPLE_FMT_FLT, Impl::kOutRate,
                    &in_layout,  ctx->sample_fmt,   ctx->sample_rate,
                    0, nullptr) >= 0
                && swr_raw && swr_init(swr_raw) >= 0;

            if (swr_ok) {
                impl->swr_ctx.reset(swr_raw);
                impl->audio_ctx = std::move(ctx);
                has_audio_ = true;
            } else {
                if (swr_raw) swr_free(&swr_raw);
            }
        });
    }

    if (impl->fmt_ctx->duration > 0)
        duration_sec_ = static_cast<double>(impl->fmt_ctx->duration) / AV_TIME_BASE;

    impl_ = std::move(impl);

    const auto t_threads_start = std::chrono::steady_clock::now();
    startThreads();
    waitForFirstFrameSeed();
    const auto t_done = std::chrono::steady_clock::now();

    using namespace std::chrono;
    const auto open_ms = duration_cast<milliseconds>(t_threads_start - t_prepare_start).count();
    const auto seed_ms = duration_cast<milliseconds>(t_done - t_threads_start).count();
    const size_t audio_buf_bytes = audio_sample_buf_.size();
    lg().info("VideoClip '{}': prepare() done — open={}ms, video_seed={}ms, "
             "audio_buf={} samples, audio_q_empty={}, video_q_empty={}",
             path_, open_ms, seed_ms,
             audio_buf_bytes / Impl::kOutCh,
             audio_queue_.empty(), video_queue_.empty());
    lg().info("VideoClip: opened {} ({:.2f}s, audio={})", path_, duration_sec_, has_audio_);

    if (head_buffer_sec_ > 0.0) captureHeadBuffer();
}

void VideoClip::captureHeadBuffer() {
    if (head_buffer_sec_ <= 0.0 || !impl_) return;

    const size_t target_video =
        static_cast<size_t>(std::ceil(head_buffer_sec_
                                       * static_cast<double>(output_fps_)));
    const size_t target_audio_floats =
        has_audio_
            ? static_cast<size_t>(head_buffer_sec_
                                  * static_cast<double>(Impl::kOutRate))
              * Impl::kOutCh
            : 0;

    head_video_.clear();
    head_video_.reserve(target_video);
    head_audio_samples_.clear();
    if (target_audio_floats) head_audio_samples_.reserve(target_audio_floats);

    const auto t_start  = std::chrono::steady_clock::now();
    const auto deadline = t_start + std::chrono::seconds(5);

    auto need_more = [&] {
        return head_video_.size() < target_video
            || head_audio_samples_.size() < target_audio_floats;
    };

    while (need_more()) {
        if (std::chrono::steady_clock::now() > deadline) {
            lg().warn("VideoClip '{}': head buffer capture timed out "
                     "(video={}/{}, audio={}/{})",
                     path_, head_video_.size(), target_video,
                     head_audio_samples_.size(), target_audio_floats);
            break;
        }

        bool progressed = false;

        if (head_video_.size() < target_video) {
            {
                std::unique_lock<std::mutex> lk(frame_cv_mtx_);
                frame_cv_.wait_for(lk, std::chrono::milliseconds(40),
                    [this] { return !video_queue_.empty()
                                 || !decoding_.load(std::memory_order_relaxed); });
            }
            if (auto f = video_queue_.pop()) {
                video_push_cv_.notify_one();
                head_video_.push_back(*f);
                progressed = true;
            }
        }

        if (head_audio_samples_.size() < target_audio_floats) {
            {
                std::unique_lock<std::mutex> lk(audio_cv_mtx_);
                audio_cv_.wait_for(lk, std::chrono::milliseconds(40),
                    [this] { return !audio_queue_.empty()
                                 || !decoding_.load(std::memory_order_relaxed); });
            }
            if (auto a = audio_queue_.pop()) {
                head_audio_samples_.insert(head_audio_samples_.end(),
                    a->samples.begin(), a->samples.end());
                progressed = true;
            }
        }

        if (!progressed && !decoding_.load(std::memory_order_relaxed)) break;
    }

    lg().info("VideoClip '{}': head buffer captured ({} frames, {} audio samples)",
             path_,
             head_video_.size(),
             has_audio_ ? head_audio_samples_.size() / Impl::kOutCh : 0);

    // The first head_buffer_sec_ of the clip are now cached in
    // head_video_/head_audio_samples_ and will be replayed during a self-loop
    // transition window via head_playback_. Rewind the demuxer to t=0 and
    // restart threads so the live queue refills from frame 0 — first
    // playback (and any non-self-loop transition) must serve the head from
    // the live queue. Skip is applied only on self-loop reset(), keyed off
    // head_was_played_, where the user has actually seen those frames via
    // head_video_ and the live queue would otherwise duplicate them.
    stopThreads();
    if (impl_->fmt_ctx && impl_->video_stream_idx >= 0) {
        const int rc = avformat_seek_file(impl_->fmt_ctx.get(),
            impl_->video_stream_idx, INT64_MIN, 0, INT64_MAX, 0);
        if (rc < 0)
            lg().warn("VideoClip '{}': head buffer seek-back failed (rc={})",
                     path_, rc);
    }
    if (impl_->video_ctx) avcodec_flush_buffers(impl_->video_ctx.get());
    if (impl_->audio_ctx) avcodec_flush_buffers(impl_->audio_ctx.get());
    audio_sample_buf_.clear();
    seeded_first_frame_.store(false, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(tail_frame_mtx_);
        tail_frame_ = Frame{};
    }
    startThreads();
    waitForFirstFrameSeed();
}

void VideoClip::setHeadPlayback(bool on) {
    const bool prev = head_playback_.exchange(on, std::memory_order_acq_rel);
    // Reset cursors only on rising edge — the Preloader calls this every
    // tick during a self-loop transition; resetting unconditionally would
    // pin playback to the first head frame. The rising edge also marks the
    // head buffer as "consumed", so the next reset() knows to skip the
    // matching frames in the live queue (otherwise they'd be replayed).
    if (on && !prev) {
        head_video_idx_.store(0, std::memory_order_release);
        head_audio_idx_.store(0, std::memory_order_release);
        head_was_played_.store(true, std::memory_order_release);
    }
}

void VideoClip::startThreads() {
    seeded_first_frame_.store(false, std::memory_order_relaxed);
    video_exhausted_.store(false, std::memory_order_relaxed);
    audio_exhausted_.store(false, std::memory_order_relaxed);
    decoding_.store(true, std::memory_order_relaxed);
    packet_reader_thread_ = std::jthread([this] { packetReaderLoop(); });
    video_decode_thread_  = std::jthread([this] { videoDecodeLoop(); });
    if (has_audio_)
        audio_decode_thread_ = std::jthread([this] { audioDecodeLoop(); });
}

void VideoClip::stopThreads() {
    decoding_.store(false, std::memory_order_relaxed);
    frame_cv_.notify_all();
    audio_cv_.notify_all();
    video_push_cv_.notify_all();
    pkt_drain_cv_.notify_all();
    seed_cv_.notify_all();
    video_pkt_queue_.notify_all();
    audio_pkt_queue_.notify_all();

    if (packet_reader_thread_.joinable()) packet_reader_thread_.join();
    if (video_decode_thread_.joinable())  video_decode_thread_.join();
    if (audio_decode_thread_.joinable())  audio_decode_thread_.join();

    while (auto opt = video_pkt_queue_.pop()) {
        AVPacket* pkt = opt->pkt;
        if (pkt) { av_packet_unref(pkt); av_packet_free(&pkt); }
    }
    while (auto opt = audio_pkt_queue_.pop()) {
        AVPacket* pkt = opt->pkt;
        if (pkt) { av_packet_unref(pkt); av_packet_free(&pkt); }
    }
    while (video_queue_.pop()) {}
    while (audio_queue_.pop()) {}

    if (has_pending_video_ && pending_video_.pkt) {
        av_packet_unref(pending_video_.pkt);
        av_packet_free(&pending_video_.pkt);
    }
    if (has_pending_audio_ && pending_audio_.pkt) {
        av_packet_unref(pending_audio_.pkt);
        av_packet_free(&pending_audio_.pkt);
    }
    has_pending_video_ = has_pending_audio_ = false;
    pending_video_     = {};
    pending_audio_     = {};

    audio_sample_buf_.clear();
}

void VideoClip::waitForFirstFrameSeed() {
    std::unique_lock<std::mutex> lk(seed_mtx_);
    seed_cv_.wait_for(lk, std::chrono::seconds(2),
        [this] { return seeded_first_frame_.load(std::memory_order_acquire)
                     || !decoding_.load(std::memory_order_relaxed); });
}

void VideoClip::release() {
    const bool had_impl = (impl_ != nullptr);
    if (had_impl)
        lg().info("VideoClip '{}': release() start", path_);
    stopThreads();
    impl_.reset();
    // duration_sec_ and has_audio_ remain — they are stable container
    // metadata, not runtime state. Timeline's slot arithmetic depends on
    // getDuration() being constant across the clip's lifetime; zeroing it
    // here would collapse the timeline geometry mid-cycle and cause the
    // Preloader to release wrong clips.
    {
        std::lock_guard<std::mutex> lk(tail_frame_mtx_);
        tail_frame_ = Frame{};
    }
    seeded_first_frame_.store(false, std::memory_order_relaxed);
    head_playback_.store(false, std::memory_order_relaxed);
    head_was_played_.store(false, std::memory_order_relaxed);
    head_video_.clear();
    head_video_.shrink_to_fit();
    head_audio_samples_.clear();
    head_audio_samples_.shrink_to_fit();
    if (had_impl)
        lg().info("VideoClip '{}': release() done", path_);
}

void VideoClip::reset() {
    if (!impl_) return;
    const auto t_reset_start = std::chrono::steady_clock::now();
    lg().info("VideoClip '{}': reset() start", path_);

    stopThreads();

    // Seek demuxer back to start; flush decoder state.
    if (impl_->fmt_ctx && impl_->video_stream_idx >= 0) {
        const int rc = avformat_seek_file(impl_->fmt_ctx.get(),
            impl_->video_stream_idx, INT64_MIN, 0, INT64_MAX, 0);
        if (rc < 0)
            lg().warn("VideoClip '{}': reset seek failed (rc={})", path_, rc);
    }
    // Self-loop reset: the previous transition window served head_video_
    // (head_was_played_ rose), so the live queue must skip the same frames
    // on resume — otherwise the user sees the head played twice in a row.
    // Multi-clip wrap (or first-time prepare-after-reset) leaves the flag
    // false so the live queue serves the entire clip from frame 0.
    if (head_buffer_sec_ > 0.0 && !head_video_.empty()
            && head_was_played_.exchange(false, std::memory_order_acq_rel)) {
        skip_video_frames_.store(head_video_.size(),
                                 std::memory_order_relaxed);
        skip_audio_floats_.store(head_audio_samples_.size(),
                                 std::memory_order_relaxed);
    }
    if (impl_->video_ctx) avcodec_flush_buffers(impl_->video_ctx.get());
    if (impl_->audio_ctx) avcodec_flush_buffers(impl_->audio_ctx.get());

    startThreads();
    waitForFirstFrameSeed();
    const auto reset_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t_reset_start).count();
    lg().info("VideoClip '{}': reset() done in {}ms (audio_buf={} samples)",
             path_, reset_ms, audio_sample_buf_.size() / Impl::kOutCh);
}

// ─── packetReaderLoop ─────────────────────────────────────────────────────────
//
// Broadcast model: read packets until EOF, push FLUSH sentinels so decoders
// drain their last frames, then idle until reset() or release(). No internal
// looping — Timeline handles playlist wrap.

void VideoClip::packetReaderLoop() {
    if (!impl_) return;
    numa::bindCurrentThreadToNode(numa_node_);
    Log::setThreadName("ch" + (channel_id_.empty() ? std::string("?") : channel_id_) + "-pktrd");
    const auto t_thread_start = std::chrono::steady_clock::now();
    lg().debug("VideoClip '{}': reader thread started (numa_node={})",
              path_, numa_node_);
    int video_pkts = 0;
    int audio_pkts = 0;

    auto flushPending = [&]() {
        while (decoding_.load(std::memory_order_relaxed) &&
               (has_pending_video_ || has_pending_audio_)) {
            if (has_pending_video_ && video_pkt_queue_.push(pending_video_)) {
                has_pending_video_ = false;
                video_pkt_queue_.notify();
            }
            if (has_pending_audio_ && audio_pkt_queue_.push(pending_audio_)) {
                has_pending_audio_ = false;
                audio_pkt_queue_.notify();
            }
            if (has_pending_video_ || has_pending_audio_) {
                std::unique_lock<std::mutex> lk(pkt_drain_mtx_);
                pkt_drain_cv_.wait_for(lk, std::chrono::milliseconds(5));
            }
        }
    };

    auto pushOrPend = [&](auto& queue, AVPacket* pkt,
                          PacketOrSentinel& pending, bool& has_pending) {
        PacketOrSentinel item{pkt, SentinelType::FLUSH};
        if (queue.push(item)) {
            queue.notify();
            return;
        }
        pending     = item;
        has_pending = true;
    };

    auto pushSentinelBlocking = [&](auto& queue) {
        PacketOrSentinel sentinel{nullptr, SentinelType::FLUSH};
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(100);
        while (decoding_.load(std::memory_order_relaxed) && !queue.push(sentinel)) {
            if (std::chrono::steady_clock::now() >= deadline) return;
            std::unique_lock<std::mutex> lk(pkt_drain_mtx_);
            pkt_drain_cv_.wait_for(lk, std::chrono::milliseconds(1));
        }
        queue.notify();
    };

    bool eof_seen = false;

    while (decoding_.load(std::memory_order_relaxed)) {
        flushPending();
        if (!decoding_.load(std::memory_order_relaxed)) break;

        if (eof_seen) {
            // Idle: wait for shutdown or reset (which tears us down via stopThreads).
            std::unique_lock<std::mutex> lk(pkt_drain_mtx_);
            pkt_drain_cv_.wait_for(lk, std::chrono::milliseconds(50));
            continue;
        }

        AVPacket* pkt = av_packet_alloc();
        if (!pkt) break;

        const int ret = packet_reader_->read(impl_->fmt_ctx.get(), pkt);
        if (ret == AVERROR_EOF) {
            av_packet_free(&pkt);
            const auto eof_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t_thread_start).count();
            lg().info("VideoClip '{}': reader EOF after {}ms (video_pkts={}, audio_pkts={})",
                     path_, eof_ms, video_pkts, audio_pkts);
            // Tell decoders to drain whatever they still hold.
            pushSentinelBlocking(video_pkt_queue_);
            if (has_audio_) pushSentinelBlocking(audio_pkt_queue_);
            eof_seen = true;
            continue;
        }
        if (ret < 0) {
            av_packet_free(&pkt);
            if (decoding_.load(std::memory_order_relaxed))
                lg().error("VideoClip '{}': read error {}, stopping", path_, ret);
            decoding_.store(false, std::memory_order_relaxed);
            break;
        }

        if (pkt->stream_index == impl_->video_stream_idx) {
            if (video_pkts == 0) {
                const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - t_thread_start).count();
                lg().debug("VideoClip '{}': first video pkt at {}ms", path_, ms);
            }
            ++video_pkts;
            pushOrPend(video_pkt_queue_, pkt, pending_video_, has_pending_video_);
        } else if (has_audio_ && pkt->stream_index == impl_->audio_stream_idx) {
            if (audio_pkts == 0) {
                const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - t_thread_start).count();
                lg().debug("VideoClip '{}': first audio pkt at {}ms", path_, ms);
            }
            ++audio_pkts;
            pushOrPend(audio_pkt_queue_, pkt, pending_audio_, has_pending_audio_);
        } else {
            av_packet_unref(pkt);
            av_packet_free(&pkt);
        }
    }
}

// ─── videoDecodeLoop ─────────────────────────────────────────────────────────

void VideoClip::videoDecodeLoop() {
    if (!impl_) return;
    numa::bindCurrentThreadToNode(numa_node_);
    Log::setThreadName("ch" + (channel_id_.empty() ? std::string("?") : channel_id_) + "-vdec");
    const auto t_thread_start = std::chrono::steady_clock::now();
    lg().debug("VideoClip '{}': video decode thread started (numa_node={})",
              path_, numa_node_);
    int decoded_frames = 0;

    auto av_frm = AvFramePtr{av_frame_alloc()};
    if (!av_frm) return;

    auto buildFrame = [&](const AVFrame* frm) -> Frame {
        const int src_h = frm->height > 0 ? frm->height : impl_->video_ctx->height;
        int64_t pts_us = 0;
        if (frm->pts != AV_NOPTS_VALUE && impl_->video_tb.den > 0)
            pts_us = static_cast<int64_t>(
                static_cast<double>(frm->pts) * impl_->video_tb.num
                * AV_TIME_BASE / impl_->video_tb.den);
        Frame f;
        if (pool_) f = pool_->acquire();
        if (!f.valid()) {
            const size_t sz = static_cast<size_t>(out_width_) * out_height_ * 4;
            f.data   = std::make_shared<uint8_t[]>(sz);
            f.width  = out_width_;
            f.height = out_height_;
        }
        f.pts = pts_us;
        uint8_t* dst_data[4]   = { f.pixels(), nullptr, nullptr, nullptr };
        int      dst_stride[4] = { out_width_ * 4, 0, 0, 0 };
        sws_scale(impl_->sws_ctx.get(),
                  frm->data, frm->linesize, 0, src_h, dst_data, dst_stride);
        return f;
    };

    auto seedTailFrame = [&](const Frame& f) {
        // freeze_tail_ keeps the OLD last-frame snapshot intact while a
        // fresh decoder seed comes in (single-clip transition loop). Still
        // signal seeded_first_frame_ so waitForFirstFrameSeed() unblocks.
        if (!freeze_tail_.load(std::memory_order_acquire)) {
            std::lock_guard<std::mutex> lk(tail_frame_mtx_);
            tail_frame_ = f;
        }
        if (!seeded_first_frame_.exchange(true, std::memory_order_acq_rel)) {
            std::lock_guard<std::mutex> lk(seed_mtx_);
            seed_cv_.notify_all();
        }
    };

    auto pushVideo = [&](Frame f) {
        if (skip_video_frames_.load(std::memory_order_relaxed) > 0) {
            skip_video_frames_.fetch_sub(1, std::memory_order_relaxed);
            return;
        }
        if (decoded_frames == 0) {
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t_thread_start).count();
            lg().debug("VideoClip '{}': first video frame decoded at {}ms", path_, ms);
        }
        ++decoded_frames;
        seedTailFrame(f);
        while (decoding_.load(std::memory_order_relaxed)) {
            if (video_queue_.push(f)) {
                frame_cv_.notify_one();
                return;
            }
            std::unique_lock<std::mutex> lk(video_push_mtx_);
            video_push_cv_.wait_for(lk, std::chrono::milliseconds(5));
        }
    };

    auto drainDecoder = [&]() {
        while (avcodec_receive_frame(impl_->video_ctx.get(), av_frm.get()) == 0
               && decoding_.load(std::memory_order_relaxed)) {
            pushVideo(buildFrame(av_frm.get()));
        }
    };

    const auto pop_timeout = std::chrono::milliseconds(
        std::max(5, 1000 / std::max(1, output_fps_)));

    bool expect_keyframe = false;

    while (decoding_.load(std::memory_order_relaxed)) {
        auto item = video_pkt_queue_.pop_wait(pop_timeout);
        if (!item) continue;

        pkt_drain_cv_.notify_one();

        if (item->is_sentinel()) {
            avcodec_send_packet(impl_->video_ctx.get(), nullptr);
            drainDecoder();
            avcodec_flush_buffers(impl_->video_ctx.get());
            expect_keyframe = true;
            video_exhausted_.store(true, std::memory_order_release);
            frame_cv_.notify_all();
            continue;
        }

        AVPacket* pkt = item->pkt;
        if (avcodec_send_packet(impl_->video_ctx.get(), pkt) == 0) {
            while (avcodec_receive_frame(impl_->video_ctx.get(), av_frm.get()) == 0
                   && decoding_.load(std::memory_order_relaxed)) {
                if (expect_keyframe) {
                    if (av_frm->pict_type != AV_PICTURE_TYPE_I) {
                        lg().warn("VideoClip '{}': non-IDR frame after flush "
                                 "(pict_type={}), expect artifacts",
                                 path_, static_cast<int>(av_frm->pict_type));
                        if (metrics_)
                            metrics_->decode_errors.fetch_add(1, std::memory_order_relaxed);
                    }
                    expect_keyframe = false;
                }
                pushVideo(buildFrame(av_frm.get()));
            }
        } else if (metrics_) {
            metrics_->decode_errors.fetch_add(1, std::memory_order_relaxed);
        }
        av_packet_unref(pkt);
        av_packet_free(&pkt);
    }
}

// ─── audioDecodeLoop ─────────────────────────────────────────────────────────
//
// Direct pass-through: decode → SWR → push to audio_queue_. On FLUSH sentinel
// (EOF or reset) drain decoder + SWR, then flush buffers. No internal
// crossfade — broadcast model handles A/V transitions at the RenderLoop level.

void VideoClip::audioDecodeLoop() {
    if (!impl_ || !impl_->audio_ctx) return;
    numa::bindCurrentThreadToNode(numa_node_);
    Log::setThreadName("ch" + (channel_id_.empty() ? std::string("?") : channel_id_) + "-adec");
    const auto t_thread_start = std::chrono::steady_clock::now();
    lg().debug("VideoClip '{}': audio decode thread started (numa_node={})",
              path_, numa_node_);
    int total_samples_pushed = 0;
    bool first_push_logged = false;

    auto av_frm = AvFramePtr{av_frame_alloc()};
    if (!av_frm) return;

    auto convertFrame = [&](const AVFrame* frm) -> std::vector<float> {
        const int max_out =
            swr_get_out_samples(impl_->swr_ctx.get(), frm->nb_samples) + 256;
        std::vector<float> out(static_cast<size_t>(max_out) * Impl::kOutCh);
        uint8_t* p = reinterpret_cast<uint8_t*>(out.data());
        const int n = swr_convert(impl_->swr_ctx.get(),
            &p, max_out,
            const_cast<const uint8_t**>(frm->data), frm->nb_samples);
        if (n <= 0) { out.clear(); return out; }
        out.resize(static_cast<size_t>(n) * Impl::kOutCh);
        return out;
    };

    auto drainSwr = [&]() -> std::vector<float> {
        if (!impl_->swr_ctx) return {};
        const int delay = swr_get_delay(impl_->swr_ctx.get(), Impl::kOutRate);
        if (delay <= 0) return {};
        std::vector<float> out(static_cast<size_t>(delay) * Impl::kOutCh);
        uint8_t* p = reinterpret_cast<uint8_t*>(out.data());
        const int n = swr_convert(impl_->swr_ctx.get(), &p, delay, nullptr, 0);
        if (n <= 0) { out.clear(); return out; }
        out.resize(static_cast<size_t>(n) * Impl::kOutCh);
        return out;
    };

    auto pushAudio = [&](std::vector<float>&& samples) {
        if (samples.empty()) return;
        size_t skip = skip_audio_floats_.load(std::memory_order_relaxed);
        if (skip > 0) {
            if (skip >= samples.size()) {
                skip_audio_floats_.fetch_sub(samples.size(),
                                             std::memory_order_relaxed);
                return;
            }
            samples.erase(samples.begin(),
                          samples.begin() + static_cast<std::ptrdiff_t>(skip));
            skip_audio_floats_.store(0, std::memory_order_relaxed);
        }
        AudioFrame af;
        af.samples     = std::move(samples);
        af.num_samples = static_cast<int>(af.samples.size() / Impl::kOutCh);
        af.sample_rate = Impl::kOutRate;
        af.channels    = Impl::kOutCh;
        af.valid       = true;
        if (!first_push_logged) {
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t_thread_start).count();
            lg().debug("VideoClip '{}': first audio samples ({}) pushed at {}ms",
                      path_, af.num_samples, ms);
            first_push_logged = true;
        }
        total_samples_pushed += af.num_samples;
        while (decoding_.load(std::memory_order_relaxed)) {
            if (audio_queue_.push(af)) {
                audio_queue_.notify();
                audio_cv_.notify_one();
                return;
            }
            std::unique_lock<std::mutex> lk(audio_cv_mtx_);
            audio_cv_.wait_for(lk, std::chrono::milliseconds(5));
        }
    };

    const auto pop_timeout = std::chrono::milliseconds(40);

    while (decoding_.load(std::memory_order_relaxed)) {
        auto item = audio_pkt_queue_.pop_wait(pop_timeout);
        if (!item) continue;

        pkt_drain_cv_.notify_one();

        if (item->is_sentinel()) {
            avcodec_send_packet(impl_->audio_ctx.get(), nullptr);
            while (avcodec_receive_frame(impl_->audio_ctx.get(), av_frm.get()) == 0
                   && decoding_.load(std::memory_order_relaxed)) {
                pushAudio(convertFrame(av_frm.get()));
            }
            {
                auto leftover = drainSwr();
                if (!leftover.empty()) pushAudio(std::move(leftover));
            }
            avcodec_flush_buffers(impl_->audio_ctx.get());
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t_thread_start).count();
            lg().info("VideoClip '{}': audio FLUSH drained at {}ms (total_pushed={} samples)",
                     path_, ms, total_samples_pushed);
            audio_exhausted_.store(true, std::memory_order_release);
            audio_cv_.notify_all();
            continue;
        }

        AVPacket* pkt = item->pkt;
        if (avcodec_send_packet(impl_->audio_ctx.get(), pkt) == 0) {
            while (avcodec_receive_frame(impl_->audio_ctx.get(), av_frm.get()) == 0
                   && decoding_.load(std::memory_order_relaxed)) {
                auto samples = convertFrame(av_frm.get());
                if (!samples.empty()) pushAudio(std::move(samples));
            }
        }
        av_packet_unref(pkt);
        av_packet_free(&pkt);
    }
}

// ─── IClip overrides ─────────────────────────────────────────────────────────

Frame VideoClip::getFrame() {
    if (head_playback_.load(std::memory_order_acquire)) {
        if (head_video_.empty()) return Frame{};
        size_t i = head_video_idx_.fetch_add(1, std::memory_order_relaxed);
        if (i >= head_video_.size()) i = head_video_.size() - 1;
        return head_video_[i];
    }
    std::unique_lock<std::mutex> lk(frame_cv_mtx_);
    frame_cv_.wait_for(lk, std::chrono::milliseconds(40),
        [this] { return !video_queue_.empty()
                     || !decoding_.load(std::memory_order_relaxed)
                     || video_exhausted_.load(std::memory_order_relaxed); });
    auto f = video_queue_.pop();
    video_push_cv_.notify_one();
    if (f) {
        if (!freeze_tail_.load(std::memory_order_acquire)) {
            std::lock_guard<std::mutex> tg(tail_frame_mtx_);
            tail_frame_ = *f;
        }
        return *f;
    }
    // Queue empty (exhausted post-EOF, or decoder stalled). Hold on the last
    // good frame so RenderLoop never sees invalid — broadcast tail-freeze
    // semantics until Preloader triggers reset() for the next cycle.
    if (seeded_first_frame_.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> tg(tail_frame_mtx_);
        return tail_frame_;
    }
    return Frame{};
}

AudioFrame VideoClip::getAudio(int num_samples) {
    if (head_playback_.load(std::memory_order_acquire)) {
        AudioFrame result;
        result.sample_rate = Impl::kOutRate;
        result.channels    = Impl::kOutCh;
        if (num_samples <= 0 || !has_audio_ || head_audio_samples_.empty())
            return result;
        const size_t want = static_cast<size_t>(num_samples) * Impl::kOutCh;
        const size_t cur  = head_audio_idx_.load(std::memory_order_relaxed);
        const size_t avail = (cur < head_audio_samples_.size())
                              ? head_audio_samples_.size() - cur : 0;
        const size_t take = std::min(want, avail);
        result.samples.assign(want, 0.0f);
        if (take > 0) {
            std::copy(head_audio_samples_.begin() + cur,
                      head_audio_samples_.begin() + cur + take,
                      result.samples.begin());
            head_audio_idx_.store(cur + take, std::memory_order_release);
        }
        result.num_samples = num_samples;
        result.valid       = true;
        return result;
    }
    const int needed_floats = num_samples * 2;

    auto drainQueue = [&] {
        while (auto a = audio_queue_.pop()) {
            audio_cv_.notify_one();
            if (!a->valid || a->samples.empty()) continue;
            audio_sample_buf_.insert(audio_sample_buf_.end(),
                                     a->samples.begin(), a->samples.end());
        }
    };

    drainQueue();

    constexpr int kMaxWaits = 10;
    int waits_used = 0;
    for (int w = 0;
         w < kMaxWaits
             && static_cast<int>(audio_sample_buf_.size()) < needed_floats
             && has_audio_
             && decoding_.load(std::memory_order_relaxed)
             && !audio_exhausted_.load(std::memory_order_relaxed);
         ++w) {
        std::unique_lock<std::mutex> lk(audio_cv_mtx_);
        audio_cv_.wait_for(lk, std::chrono::milliseconds(10),
            [this] {
                return !audio_queue_.empty()
                    || !decoding_.load(std::memory_order_relaxed)
                    || audio_exhausted_.load(std::memory_order_relaxed);
            });
        lk.unlock();
        drainQueue();
        ++waits_used;
    }

    AudioFrame result;
    result.sample_rate = Impl::kOutRate;
    result.channels    = Impl::kOutCh;

    const bool exhausted = audio_exhausted_.load(std::memory_order_relaxed)
                        || !decoding_.load(std::memory_order_relaxed);
    const int available = static_cast<int>(audio_sample_buf_.size()) / 2;

    if (available == 0 && exhausted) {
        // Post-EOF tail freeze: broadcast clip is "frozen" until reset().
        // Return silence (full-length, valid) — not an underrun.
        result.samples.assign(static_cast<size_t>(num_samples) * 2, 0.0f);
        result.num_samples = num_samples;
        result.valid       = true;
        return result;
    }
    if (available == 0) return result;

    const int to_serve = std::min(available, num_samples);
    const int floats   = to_serve * 2;
    if (exhausted && to_serve < num_samples) {
        // Post-EOF leftover (e.g. 991 of 1920): pad with silence to full
        // length so this isn't counted as an underrun.
        result.samples.assign(static_cast<size_t>(num_samples) * 2, 0.0f);
        for (int i = 0; i < floats; ++i) {
            result.samples[i] = audio_sample_buf_.front();
            audio_sample_buf_.pop_front();
        }
        result.num_samples = num_samples;
        result.valid       = true;
        return result;
    }
    result.samples.resize(static_cast<size_t>(floats));
    for (int i = 0; i < floats; ++i) {
        result.samples[i] = audio_sample_buf_.front();
        audio_sample_buf_.pop_front();
    }
    result.num_samples = to_serve;
    result.valid       = true;
    if (to_serve < num_samples && !exhausted) {
        if (metrics_)
            metrics_->audio_underruns.fetch_add(1, std::memory_order_relaxed);
        lg().warn("VideoClip '{}': getAudio short {}/{} (waits={}, audio_q_empty={}, "
                 "decoding={})",
                 path_, to_serve, num_samples, waits_used,
                 audio_queue_.empty(),
                 decoding_.load(std::memory_order_relaxed));
    }
    return result;
}

double VideoClip::getDuration()  const { return duration_sec_; }
bool   VideoClip::hasAudio()     const { return has_audio_; }
bool   VideoClip::isPrepared()   const { return impl_ != nullptr; }

Frame VideoClip::getTailFrame() {
    std::lock_guard<std::mutex> lk(tail_frame_mtx_);
    return tail_frame_;
}

AudioFrame VideoClip::getTailAudio(int num_samples) {
    AudioFrame silence;
    silence.sample_rate = 48000;
    silence.channels    = 2;
    silence.num_samples = num_samples;
    silence.samples.assign(static_cast<size_t>(num_samples) * 2, 0.0f);
    silence.valid = true;
    return silence;
}
