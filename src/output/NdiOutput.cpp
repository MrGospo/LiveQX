#include "output/NdiOutput.h"

#include <algorithm>
#include <climits>
#include <cstdint>
#include <utility>

#include <spdlog/spdlog.h>

#include "encoding/Encoder.h"
#include "output/NdiLib.h"

namespace liveqx::ndi {

NdiOutput::NdiOutput(OutputCfg cfg) : cfg_(std::move(cfg)) {}

NdiOutput::~NdiOutput() { NdiOutput::stop(); }

bool NdiOutput::start() {
    if (running_.load(std::memory_order_acquire)) return true;

    auto& log = log_;
    if (!log) log = spdlog::default_logger();

    lib_ = NdiLib::getOrLoad(cfg_.lib_path_override, log);
    if (!lib_ || !lib_->ok()) {
        log->warn("ndi[ch={}]: libndi unavailable: {}",
                  channel_id_,
                  lib_ ? lib_->lastError() : "no library handle");
        lib_.reset();
        return false;
    }

    abi::send_create_t s{};
    s.p_ndi_name  = cfg_.ndi_name.c_str();
    s.p_groups    = cfg_.groups.empty() ? nullptr : cfg_.groups.c_str();
    s.clock_video = cfg_.clock_video;
    s.clock_audio = cfg_.clock_audio;

    sender_ = lib_->send_create(&s);
    if (!sender_) {
        log->error("ndi[ch={}]: NDIlib_send_create('{}') returned null",
                   channel_id_, cfg_.ndi_name);
        return false;
    }
    log->info("ndi[ch={}]: sender '{}' created (groups='{}')",
              channel_id_, cfg_.ndi_name, cfg_.groups);
    running_.store(true, std::memory_order_release);
    return true;
}

void NdiOutput::stop() {
    if (!running_.exchange(false)) return;
    std::lock_guard lk(mu_);
    if (sender_ && lib_ && lib_->send_destroy) {
        lib_->send_destroy(sender_);
    }
    sender_ = nullptr;
    // lib_ остаётся жить (process-singleton); сбрасываем shared_ptr, чтобы
    // последний consumer мог дать NdiLib::~ выполниться при exit.
    lib_.reset();
    if (log_) {
        log_->info("ndi[ch={}]: sender stopped (frames_sent={}, frames_dropped={}, "
                   "encoded_dropped={})",
                   channel_id_,
                   frames_sent_.load(std::memory_order_relaxed),
                   frames_dropped_.load(std::memory_order_relaxed),
                   packets_dropped_encoded_.load(std::memory_order_relaxed));
    }
}

void NdiOutput::send(const Packet& pkt) {
    if (!running_.load(std::memory_order_acquire)) return;

    // OutputManager pumps encoded MPEG-TS chunks to every registered
    // driver. NDI consumes uncompressed RGBA/UYVY/BGRA via the
    // pre-encode tap (attachEncoder), so the encoded path is just
    // accounting noise here. Track byte counts for /outputs/{id}/status
    // visibility and drop. We never warn — operator config legitimately
    // mixes NDI with SRT/RTMP, and the warning would spam every channel
    // boot.
    packets_received_.fetch_add(1, std::memory_order_relaxed);
    bytes_received_.fetch_add(pkt.data.size(), std::memory_order_relaxed);
    packets_dropped_encoded_.fetch_add(1, std::memory_order_relaxed);
    (void) warned_encoded_packet_;
}

void NdiOutput::attachEncoder(Encoder* enc, int fps) {
    if (!enc) return;
    if (fps > 0 && fps <= 240) fps_n_ = fps;
    std::weak_ptr<NdiOutput> weak = shared_from_this();
    enc->addRawFrameCallback([weak](const Frame& f) {
        if (auto self = weak.lock()) self->sendRawFrame(f);
    });
}

void NdiOutput::sendRawFrame(const Frame& f) {
    if (!running_.load(std::memory_order_acquire)) return;
    if (!f.valid() || f.width <= 0 || f.height <= 0) {
        frames_dropped_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // Enforce the same frame size for the lifetime of the sender. NDI
    // tolerates per-frame size changes but switching mid-stream breaks
    // most receivers (vMix, Tricaster). The render loop guarantees this
    // unless the operator hot-patches encoder.width/height at runtime —
    // which already triggers a channel rebuild.
    const std::size_t expected =
        static_cast<std::size_t>(f.width) * static_cast<std::size_t>(f.height) * 4u;
    if (f.sizeBytes() != expected) {
        frames_dropped_.fetch_add(1, std::memory_order_relaxed);
        if (!warned_frame_size_mismatch_.exchange(true, std::memory_order_acq_rel)) {
            if (auto& log = log_)
                log->warn("ndi[ch={}]: dropping frame, sizeBytes={} != width*height*4={}",
                          channel_id_, f.sizeBytes(), expected);
        }
        return;
    }

    // RGBA → BGRA: swap byte 0 and byte 2 of every pixel. NDI 5 accepts
    // BGRA directly (FourCC_BGRA) — saves a libswscale dependency on
    // this hot path. UYVY would be more bandwidth-efficient on the wire
    // but needs full chroma-subsample conversion; revisit once we have
    // throughput data from real deployments.
    bgra_scratch_.resize(expected);
    const std::uint8_t* src = f.pixels();
    std::uint8_t*       dst = bgra_scratch_.data();
    const std::size_t   pix = static_cast<std::size_t>(f.width) * f.height;
    for (std::size_t i = 0; i < pix; ++i) {
        dst[0] = src[2];
        dst[1] = src[1];
        dst[2] = src[0];
        dst[3] = src[3];
        dst += 4;
        src += 4;
    }

    abi::video_frame_v2_t v{};
    v.xres                  = f.width;
    v.yres                  = f.height;
    v.FourCC                = abi::FourCC_BGRA;
    v.frame_rate_N          = fps_n_ * 1000;
    v.frame_rate_D          = 1000;
    v.picture_aspect_ratio  = static_cast<float>(f.width) / static_cast<float>(f.height);
    v.frame_format_type     = abi::frame_format_progressive;
    // INT64_MAX = "synthesize timecode" sentinel per NDI SDK 5 docs;
    // libndi clocks it from clock_video itself when set.
    v.timecode              = LLONG_MAX;
    v.timestamp             = LLONG_MAX;
    v.p_data                = bgra_scratch_.data();
    v.line_stride_in_bytes  = f.width * 4;
    v.p_metadata            = nullptr;

    // Guard the actual send_send_v2 call with mu_ so a concurrent stop()
    // can't destroy the sender_ between our null-check and the call.
    std::lock_guard lk(mu_);
    if (!sender_ || !lib_ || !lib_->send_send_v2) {
        frames_dropped_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    lib_->send_send_v2(sender_, &v);
    frames_sent_.fetch_add(1, std::memory_order_relaxed);
}

bool NdiOutput::isHealthy() const {
    return running_.load(std::memory_order_acquire) && sender_ != nullptr;
}

OutputStats NdiOutput::getStats() const {
    OutputStats s;
    s.packets_sent = packets_received_.load(std::memory_order_relaxed);
    s.bytes_sent   = bytes_received_.load(std::memory_order_relaxed);
    s.bitrate_bps  = 0.0;
    s.rtt_ms       = 0.0;
    return s;
}

nlohmann::json NdiOutput::statusJson() const {
    auto base = IOutput::statusJson();
    base["mode"]                  = "ndi";
    base["ndi_name"]              = cfg_.ndi_name;
    base["ndi_groups"]            = cfg_.groups;
    base["ndi_loaded"]            = static_cast<bool>(lib_ && lib_->ok());
    base["ndi_version"]           = (lib_ && lib_->ok() && lib_->version_string)
                                      ? lib_->version_string() : "";
    base["packets_dropped_encoded"] = packets_dropped_encoded_.load(std::memory_order_relaxed);
    base["frames_sent"]             = frames_sent_.load(std::memory_order_relaxed);
    base["frames_dropped"]          = frames_dropped_.load(std::memory_order_relaxed);
    base["fps_n"]                   = fps_n_;
    return base;
}

}  // namespace liveqx::ndi
