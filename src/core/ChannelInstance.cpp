#include "core/ChannelInstance.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>
#include <system_error>
#include <unistd.h>
#include <utility>

#include "clips/ClipFactory.h"
#include "clips/FallbackClip.h"
#include "clips/LiveClip.h"
#include "clips/LiveInputClip.h"
#include "clips/LiveInputFactory.h"
#include "clips/OnLossProvider.h"
#include "clips/PlaylistEntry.h"
#include "clips/PlaylistItem.h"
#include "content/ContentSync.h"
#include "core/FramePool.h"
#include "events/EventBus.h"
#include "preview/PreviewManager.h"
#include "core/RenderLoop.h"
#include "core/Timeline.h"
#include "core/ScheduleController.h"
#include "core/ScheduleEntry.h"
#include "core/Scheduler.h"
#include "logging/FilePlaybackSink.h"
#include "logging/IPlaybackSink.h"
#include "logging/NullPlaybackSink.h"
#include "logging/SqlitePlaybackSink.h"
#include "metrics/ChannelHealth.h"
#include "metrics/ChannelMetrics.h"
#include "output/HlsOutput.h"
#include "output/HlsOutputCfg.h"
#include "output/MulticastOutput.h"
#include "output/MulticastOutputCfg.h"
#include "output/NdiOutput.h"
#include "output/NdiOutputCfg.h"
#include "output/RtmpOutput.h"
#include "output/RtmpOutputCfg.h"
#include "output/OutputManager.h"
#include "output/SrtOutput.h"
#include "persistence/ChannelStateSaver.h"
#include "persistence/StatePersistence.h"
#include "render/CpuCompositor.h"
#include "transitions/ITransition.h"
#include "utils/CpuAffinity.h"
#include "utils/Log.h"
#include "utils/PathUtils.h"

using nlohmann::json;

namespace {

void parse_resolution(const std::string& res, int& w, int& h) {
    const auto x = res.find('x');
    if (x == std::string::npos) { w = 1280; h = 720; return; }
    w = std::stoi(res.substr(0, x));
    h = std::stoi(res.substr(x + 1));
}

TransitionType parse_transition_type(const std::string& s) {
    if (s == "crossfade")  return TransitionType::CrossFade;
    if (s == "wipe_left")  return TransitionType::WipeLeft;
    if (s == "wipe_right") return TransitionType::WipeRight;
    if (s == "wipe_up")    return TransitionType::WipeUp;
    if (s == "wipe_down")  return TransitionType::WipeDown;
    if (s == "push_left")  return TransitionType::PushLeft;
    if (s == "push_right") return TransitionType::PushRight;
    if (s == "push_up")    return TransitionType::PushUp;
    if (s == "push_down")  return TransitionType::PushDown;
    if (s == "dissolve")   return TransitionType::Dissolve;
    if (s == "fade_black" || s == "fade_to_black") return TransitionType::FadeToBlack;
    return TransitionType::HardCut;
}

TransitionMode parse_transition_mode(const std::string& s) {
    if (s == "hardcut" || s == "hard_cut") return TransitionMode::HardCut;
    if (s == "live_mix" || s == "live")    return TransitionMode::LiveMix;
    return TransitionMode::FreezeFade;
}

// fix20 — playlist-side easing parser. Lenient: unknown spellings fall back
// to Linear so the channel still boots. ScheduleEntry's parser is strict.
Easing parse_easing(const std::string& s) {
    if (s == "ease_in")     return Easing::EaseIn;
    if (s == "ease_out")    return Easing::EaseOut;
    if (s == "ease_in_out") return Easing::EaseInOut;
    return Easing::Linear;
}

const char* easingName(Easing e) {
    switch (e) {
        case Easing::Linear:    return "linear";
        case Easing::EaseIn:    return "ease_in";
        case Easing::EaseOut:   return "ease_out";
        case Easing::EaseInOut: return "ease_in_out";
    }
    return "linear";
}

TransitionConfig parse_default_transition(const json& cfg) {
    TransitionConfig tc;
    if (cfg.contains("default_transition")) {
        const auto& dt = cfg["default_transition"];
        tc.type         = parse_transition_type(dt.value("type", "crossfade"));
        tc.mode         = parse_transition_mode(dt.value("mode", "freeze_fade"));
        tc.duration_sec = dt.value("duration", 2.0);
        tc.easing       = parse_easing(dt.value("easing", "linear"));
    }
    return tc;
}

const char* transitionTypeName(TransitionType t) {
    switch (t) {
        case TransitionType::CrossFade:   return "crossfade";
        case TransitionType::WipeLeft:    return "wipe_left";
        case TransitionType::WipeRight:   return "wipe_right";
        case TransitionType::WipeUp:      return "wipe_up";
        case TransitionType::WipeDown:    return "wipe_down";
        case TransitionType::PushLeft:    return "push_left";
        case TransitionType::PushRight:   return "push_right";
        case TransitionType::PushUp:      return "push_up";
        case TransitionType::PushDown:    return "push_down";
        case TransitionType::Dissolve:    return "dissolve";
        case TransitionType::FadeToBlack: return "fade_black";
        case TransitionType::HardCut:     return "hardcut";
    }
    return "hardcut";
}

}  // namespace

// ─── build ────────────────────────────────────────────────────────────────────

std::unique_ptr<ChannelInstance> ChannelInstance::build(
    const json& cfg, std::filesystem::path channel_dir) {
    auto ch = std::unique_ptr<ChannelInstance>(new ChannelInstance());
    ch->cfg_         = cfg;
    ch->channel_dir_ = std::move(channel_dir);
    if (!ch->channel_dir_.empty()) {
        // mkdir -p channel_dir/{logs,cache} so subsequent logger / cache
        // creation never has to race the parent dir into existence.
        std::error_code ec;
        std::filesystem::create_directories(ch->channel_dir_ / "logs", ec);
        std::filesystem::create_directories(ch->channel_dir_ / "cache", ec);
    }
    ch->buildLongLived(cfg);

    // fix17 — open per-channel state.db and start the debounce writer.
    // Legacy channels with empty channel_dir get no persistence (and no
    // saver); they still play, just without kill-9 recovery.
    if (!ch->channel_dir_.empty()) {
        ch->state_persist_ =
            std::make_unique<liveqx::persistence::ChannelStatePersistence>(
                ch->channel_dir_ / "state.db");

        // Read whatever the previous run left behind. ok()==false (corrupt
        // db that got renamed, or not yet created) returns an empty
        // snapshot. We cache the cursor anchor and the paused flag for
        // play()/main.cpp to consume; everything else (schedule_active) is
        // diagnostic and gets re-derived from wall-clock at runtime.
        if (ch->state_persist_->ok()) {
            const auto snap = ch->state_persist_->load();
            if (snap.playlist_index.has_value() &&
                snap.playlist_index.value() >= 0) {
                ch->pending_restore_idx_      = snap.playlist_index;
                ch->pending_restore_slot_pos_ = snap.slot_pos_sec;
            }
            if (snap.paused.has_value()) {
                ch->persisted_paused_at_load_ = snap.paused.value();
                ch->paused_intent_            = snap.paused.value();
            }
            if (ch->logger_ && (ch->pending_restore_idx_ ||
                                ch->persisted_paused_at_load_)) {
                ch->logger_->info(
                    "state.db restore — idx={} slot={} paused={}",
                    snap.playlist_index.value_or(-1),
                    snap.slot_pos_sec.value_or(0.0),
                    snap.paused.value_or(false));
            }
        }

        ch->state_saver_ =
            std::make_unique<liveqx::persistence::ChannelStateSaver>(
                *ch->state_persist_,
                [raw = ch.get()] { return raw->captureChannelState(); });
    }
    return ch;
}

void ChannelInstance::buildLongLived(const json& cfg) {
    id_   = cfg.value("id", 0);
    name_ = cfg.value("name", "channel");

    // Create per-channel logger up front so every subsequent log line in the
    // build path is routed to the channel's log file. With channel_dir set
    // (fix7) it goes to channel_dir/logs/channel.log; otherwise legacy
    // logs/ch{id}-{name}.log.
    logger_ = channel_dir_.empty()
        ? Log::createChannelLogger(id_, name_)
        : Log::createChannelLoggerInDir(id_, name_,
                                         (channel_dir_ / "logs").string());

    parse_resolution(cfg.value("resolution", "1280x720"), width_, height_);
    fps_ = cfg.value("fps", 25);
    numa_node_ = cfg.value("numa_node", -1);

    // First-touch on the right NUMA node so pool arenas, Timeline and
     // compositor structures land on the channel's local node. runOnNode
     // is a no-op on -1 / WSL.
     numa::runOnNode(numa_node_, [&] {
         decode_pool_ = std::make_shared<FramePool>(8, width_, height_);
         render_pool_ = std::make_shared<FramePool>(3, width_, height_);
         metrics_     = std::make_shared<ChannelMetrics>();
         health_      = std::make_shared<ChannelHealth>(std::to_string(id_), fps_);
         timeline_    = std::make_unique<Timeline>();
         compositor_  = std::make_unique<CpuCompositor>(*render_pool_);
     });
     // Belt-and-suspenders: explicit mbind so the pool arena is bound
     // regardless of first-touch placement (transparent hugepage migrations
     // etc.). No-op if numa_node_ < 0.
     decode_pool_->bindMemoryToNumaNode(numa_node_);
     render_pool_->bindMemoryToNumaNode(numa_node_);

    std::vector<std::unique_ptr<IClip>> clips;
    std::vector<TransitionConfig>       transitions;

    std::vector<std::string> paths;
    // fix46: канал, привязанный к ContentSync (cache/passthrough), не
    // должен грузить playlist из cfg — Timeline заполнит ContentSync через
    // restoreFromDisk() в buildRuntime(). Исторические записи в config.json
    // (legacy либо смена режима с manual на cache) сложатся со свежими из
    // кэша → в UI плейлист удваивается. Вычищаем cfg_["playlist"] и сразу
    // persistConfig(), чтобы баг не возвращался при последующих рестартах.
    if (managedByContentSync() && cfg.contains("playlist")
            && cfg["playlist"].is_array() && !cfg["playlist"].empty()) {
        logger_->info("ContentSync-managed channel: discarding {} stale "
                      "playlist entries from config",
                      cfg["playlist"].size());
        cfg_.erase("playlist");
        try { persistConfig(); }
        catch (const std::exception& e) {
            logger_->warn("persistConfig (playlist purge) failed: {}", e.what());
        }
    } else if (cfg.contains("playlist") && cfg["playlist"].is_array()) {
        // fix9: cache the JSON items so we can rebuild the regular playlist
        // when a schedule window ends. Keep failed-to-prepare items too —
        // they'll fail again on rebuild and be skipped, same outcome.
        regular_playlist_items_ = cfg["playlist"];
        for (const auto& item_json : cfg["playlist"]) {
            std::string      path;
            TransitionConfig tc;
            try {
                auto clip = buildClipFromItem(item_json, path, tc);
                clips.push_back(std::move(clip));
                transitions.push_back(tc);
                paths.push_back(path);
                logger_->info("loaded clip '{}'", path);
            } catch (const std::exception& e) {
                logger_->error("failed to load '{}': {}",
                               item_json.value("path", ""), e.what());
            }
        }
    }

    if (clips.empty()) {
        logger_->warn("playlist is empty — fallback only");
    } else {
        timeline_->setPlaylist(wrapClips(std::move(clips), graveyard_),
                               std::move(transitions),
                               std::move(paths));
    }

    {
        std::string fb_path;
        if (cfg.contains("fallback"))
            fb_path = cfg["fallback"].value("image_path", "");
        try {
            auto* raw = new FallbackClip(fb_path, width_, height_);
            raw->prepare();
            // Wrap through graveyard so fallback drops on stop/teardown also
            // route off-thread. fallback_clip_ co-owns with Timeline.
            auto fb_clip = wrapClip(raw, graveyard_);
            fallback_clip_ = fb_clip;
            timeline_->setFallback(std::move(fb_clip));
        } catch (const std::exception& e) {
            logger_->warn("could not load fallback '{}': {}", fb_path, e.what());
            fallback_clip_.reset();
        }
    }

    // fix13 c9: install Timeline prepare callback. Fires once per upcoming
    // clip on the warm-up boundary. For LiveClip this is the cue to pin
    // the scheduled_start wall-clock and kick off the underlying input's
    // prepare() (RTMP handshake / multicast join) so the cut is on time.
    // Non-Live clips return warmUpSec()==0 and never reach this callback.
    timeline_->setPrepareCallback(
        [this](std::shared_ptr<IClip> next, double seconds_until_start) {
            auto* live = dynamic_cast<liveqx::LiveClip*>(next.get());
            if (!live) return;
            using namespace std::chrono;
            const std::uint64_t now_ns = duration_cast<nanoseconds>(
                system_clock::now().time_since_epoch()).count();
            const auto until_ns = static_cast<std::uint64_t>(
                std::max(0.0, seconds_until_start) * 1e9);
            live->setScheduledStart(now_ns + until_ns);
            // prepare() actually runs from LiveClip::onTick the next time
            // the wall clock crosses (start - warm_up_ns). Calling prepare
            // synchronously here would block the render thread on a slow
            // handshake; the state machine retries on every tick so a
            // late open still recovers.
        });

    // Encoder config — saved for later runtime rebuilds.
    enc_cfg_.width         = width_;
    enc_cfg_.height        = height_;
    enc_cfg_.fps           = fps_;
    enc_cfg_.video_bitrate = cfg.value("bitrate", 4'000'000);
    enc_cfg_.preset        = cfg.value("preset", std::string("medium"));
    {
        const int mbf = cfg.value("max_b_frames", 0);
        if (mbf < 0 || mbf > 16) {
            logger_->warn("max_b_frames={} out of range [0..16], using 0", mbf);
            enc_cfg_.max_b_frames = 0;
        } else {
            enc_cfg_.max_b_frames = mbf;
        }
    }
    if (cfg.contains("audio")) {
        enc_cfg_.audio_bitrate = cfg["audio"].value("bitrate", 128'000);
        enc_cfg_.sample_rate   = cfg["audio"].value("sample_rate", 48000);
    }

    // fix29 c13: GPU encoder selection. Both keys are optional —
    // unspecified means "cpu"/0 which preserves the pre-fix29 behavior.
    // Validation is deferred to EncoderFactory::pickVideoEncoder, which
    // logs and falls back if the requested backend isn't built in.
    enc_cfg_.encoder_mode = cfg.value("encoder_mode", std::string("cpu"));
    enc_cfg_.gpu_index    = cfg.value("gpu_index",    0);
    if (enc_cfg_.gpu_index < 0) {
        logger_->warn("gpu_index={} negative, clamping to 0", enc_cfg_.gpu_index);
        enc_cfg_.gpu_index = 0;
    }

    // Video codec selection. "h264" is the default; "mpeg2video" is for
    // legacy hospitality set-tops that don't decode H.264 cleanly. Any
    // other value falls back to h264 with a warning — we don't want a
    // typo to permanently break a channel.
    {
        std::string vc = cfg.value("video_codec", std::string("h264"));
        std::transform(vc.begin(), vc.end(), vc.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (vc == "mpeg2") vc = "mpeg2video";
        if (vc != "h264" && vc != "mpeg2video") {
            logger_->warn("video_codec=\"{}\" unknown, using h264", vc);
            vc = "h264";
        }
        enc_cfg_.video_codec = vc;
    }

    // Broadcast/IPTV knobs. Optional sub-object cfg.mpegts.{...}. Absent
    // means "use Encoder::Config defaults", which reproduces the pre-fix
    // shape (service_id=1, TSID=1, VBR muxer) — but SDT is now always
    // emitted regardless (unconditional in Encoder.cpp).
    if (cfg.contains("mpegts") && cfg["mpegts"].is_object()) {
        const auto& m = cfg["mpegts"];
        enc_cfg_.service_name        = m.value("service_name",        enc_cfg_.service_name);
        enc_cfg_.service_provider    = m.value("service_provider",    enc_cfg_.service_provider);
        enc_cfg_.service_id          = m.value("service_id",          enc_cfg_.service_id);
        enc_cfg_.transport_stream_id = m.value("transport_stream_id", enc_cfg_.transport_stream_id);
        enc_cfg_.original_network_id = m.value("original_network_id", enc_cfg_.original_network_id);
        enc_cfg_.mux_rate            = m.value("mux_rate",            enc_cfg_.mux_rate);
        enc_cfg_.sdt_period_ms       = m.value("sdt_period_ms",       enc_cfg_.sdt_period_ms);
        enc_cfg_.pat_period_ms       = m.value("pat_period_ms",       enc_cfg_.pat_period_ms);
        // Clamp service ids to the DVB spec range [1..0xFFFF] — 0 is
        // reserved and negative values simply don't make sense here.
        auto clamp16 = [&](int v, const char* n, int fallback) {
            if (v < 1 || v > 0xFFFF) {
                logger_->warn("mpegts.{}={} out of range [1..65535], using {}", n, v, fallback);
                return fallback;
            }
            return v;
        };
        enc_cfg_.service_id          = clamp16(enc_cfg_.service_id,          "service_id",          1);
        enc_cfg_.transport_stream_id = clamp16(enc_cfg_.transport_stream_id, "transport_stream_id", 1);
        enc_cfg_.original_network_id = clamp16(enc_cfg_.original_network_id, "original_network_id", 1);
        if (enc_cfg_.mux_rate < 0) {
            logger_->warn("mpegts.mux_rate={} negative, using 0 (VBR)", enc_cfg_.mux_rate);
            enc_cfg_.mux_rate = 0;
        }
        if (enc_cfg_.sdt_period_ms < 0) enc_cfg_.sdt_period_ms = 0;
        if (enc_cfg_.pat_period_ms < 0) enc_cfg_.pat_period_ms = 0;
    }

    // fix12 c3: migrate legacy "output" → outputs[]. After this block
    // cfg_["outputs"] is the authoritative source (and "output" is gone).
    // Migration is in-memory only — build() must be a disk read-only op
    // (ChannelInstancePersist tests assert this). The next updateConfig
    // or ChannelManager::create persist will save the migrated form, so
    // the legacy field disappears from disk lazily.
    if (!cfg_.contains("outputs") && cfg_.contains("output")) {
        json out = cfg_["output"];
        if (!out.is_object()) {
            logger_->error("legacy 'output' is not an object — ignored");
            cfg_.erase("output");
        } else {
            if (!out.contains("id") || !out["id"].is_string() ||
                out["id"].get<std::string>().empty()) {
                out["id"] = "default";
            }
            cfg_["outputs"] = json::array({std::move(out)});
            cfg_.erase("output");
            logger_->info("output → outputs[] migration: 1 driver, id='default'");
        }
    }

    // Per-entry validation (parse-only) so a bad config surfaces here
    // rather than at first packet. Bad entries are dropped from cfg_ —
    // buildRuntime falls back to a default SRT if outputs[] becomes empty.
    if (cfg_.contains("outputs")) {
        if (!cfg_["outputs"].is_array()) {
            logger_->error("'outputs' must be an array — ignoring");
            cfg_.erase("outputs");
        } else {
            json kept = json::array();
            std::set<std::string> seen_ids;
            for (auto& oc : cfg_["outputs"]) {
                if (!oc.is_object()) {
                    logger_->error("outputs[]: entry is not an object — skipped");
                    continue;
                }
                const std::string id   = oc.value("id", std::string{});
                const std::string type = oc.value("type", std::string("srt"));
                if (id.empty()) {
                    logger_->error("outputs[]: entry missing 'id' — skipped");
                    continue;
                }
                if (!seen_ids.insert(id).second) {
                    logger_->error("outputs[]: duplicate id '{}' — skipped", id);
                    continue;
                }
                try {
                    if (type == "srt") {
                        const int port = oc.value("port", 9000);
                        if (port < 1 || port > 65535)
                            throw std::runtime_error("port out of range");
                    } else if (type == "multicast") {
                        (void) liveqx::multicast::parseOutputCfg(oc);
                    } else if (type == "rtmp") {
                        (void) liveqx::rtmp::parseOutputCfg(oc);
                    } else if (type == "hls") {
                        (void) liveqx::hls::parseOutputCfg(oc);
                    } else if (type == "ndi") {
                        (void) liveqx::ndi::parseOutputCfg(oc);
                    } else {
                        throw std::runtime_error("unknown type '" + type + "'");
                    }
                } catch (const std::exception& e) {
                    logger_->error("outputs[id={}]: bad config — {} — skipped",
                                   id, e.what());
                    continue;
                }
                kept.push_back(std::move(oc));
            }
            cfg_["outputs"] = std::move(kept);
        }
    }


    preload_sec_ = cfg.value("preload_sec", 4.0);

    // Two source modes:
    //   1. cache mode      — { share_path, cache_path? }   SMB / NFS
    //   2. passthrough mode — { source_path }              local folder
    // share_path without cache_path auto-resolves to cache/<channel_leaf>.
    // source_path is fed directly to ContentSync; no cache copy.
    // fix9: always create scheduler_ + schedule_ctrl_, even when cfg has no
    // schedule. This way hot-reload via PATCH /channels/{id} can populate
    // entries without having to lazily spin up the controller and the
    // hard-switch poll thread mid-flight. Empty entries → decide() returns
    // scheduled=false → channel behaves exactly like a no-schedule channel.
    {
        // fix33 C — channel_timezone semantics:
        //   absent / null / ""     → inherits_server_tz_=true (effective = server TZ)
        //   "Europe/Moscow"        → inherits_server_tz_=false (explicit override)
        // Серверная TZ берётся из server_tz_getter_ (ставится ChannelManager).
        // На этапе build() getter обычно уже выставлен ChannelManager::create.
        // Если getter пустой (legacy/тесты) — fallback на UTC.
        if (cfg.contains("channel_timezone") &&
            !cfg["channel_timezone"].is_null() &&
            cfg["channel_timezone"].is_string() &&
            !cfg["channel_timezone"].get<std::string>().empty()) {
            inherits_server_tz_ = false;
        } else {
            inherits_server_tz_ = true;
        }
        const std::string tz = effectiveTimezone();
        std::vector<liveqx::scheduling::ScheduleEntry> initial_entries;
        if (cfg.contains("schedule")) {
            try {
                initial_entries =
                    liveqx::scheduling::parseSchedule(cfg["schedule"]);
            } catch (const std::exception& e) {
                logger_->error("schedule: parse failed — starting with empty "
                               "schedule: {}", e.what());
                initial_entries.clear();
            }
        }
        scheduler_     = std::make_unique<liveqx::scheduling::Scheduler>(
                             std::move(initial_entries), tz);
        schedule_ctrl_ = std::make_unique<liveqx::scheduling::ScheduleController>(
                             *scheduler_);
        if (!scheduler_->entries().empty())
            logger_->info("schedule: {} entry(ies) loaded (tz={})",
                          scheduler_->entries().size(), tz);
    }

    if (cfg_.contains("content_source")) {
        auto& cs = cfg_["content_source"];
        const std::string share  = cs.value("share_path",  std::string{});
        const std::string cache  = cs.value("cache_path",  std::string{});
        const std::string source = cs.value("source_path", std::string{});

        if (!source.empty() && (!share.empty() || !cache.empty())) {
            logger_->error("content_source must be either {{source_path}} "
                           "or {{share_path,cache_path?}}, not both");
        } else if (!share.empty() && cache.empty()) {
            // fix7: per-channel directory owns the cache. Legacy mode keeps
            // the previous global cache/<leaf> path so old test fixtures
            // and configs without channel_dir continue to work.
            std::string resolved;
            if (!channel_dir_.empty()) {
                resolved = (channel_dir_ / "cache").string();
            } else {
                std::string leaf = PathUtils::sanitizeForPath(name_);
                if (leaf.empty()) leaf = "ch_" + std::to_string(id_);
                resolved = "cache/" + leaf;
            }
            cs["cache_path"] = resolved;
            logger_->info("cache_path auto-resolved to '{}'", resolved);
        }
    }
}

// ─── play / stop ──────────────────────────────────────────────────────────────

bool ChannelInstance::play() {
    std::lock_guard<std::mutex> lk(state_mu_);
    if (running_.load(std::memory_order_acquire)) return false;
    paused_intent_ = false;   // fix17: explicit play clears prior pause intent

    // Reset transient signals so a previous stop/play cycle does not bleed in.
    // Watchdog skips channels with last_tick_ns==0 (set by RenderLoop on tick).
    metrics_->last_tick_ns.store(0, std::memory_order_relaxed);
    metrics_->resetTransient();
    health_->reset();

    setupPlaybackSink();
    if (!buildRuntime()) return false;

    if (!out_mgr_->startAll()) {
        logger_->error("one or more outputs failed to start");
        out_mgr_->stopAll();
        encoder_.reset(); srt_out_ = nullptr;
        out_mgr_.reset(); loop_.reset();
        return false;
    }
    if (!encoder_->open()) {
        logger_->error("encoder failed to open");
        out_mgr_->stopAll();
        encoder_.reset(); srt_out_ = nullptr;
        out_mgr_.reset(); loop_.reset();
        return false;
    }
    // fix17 — apply pending cursor restore before the render loop starts.
    // Timeline is already populated from buildLongLived; restoreCursor
    // re-anchors on the persisted (idx, slot_pos) so the first rendered
    // frame resumes near where the previous run left off. Consumed once —
    // a later stop/play cycle resets to the playlist head as before.
    if (timeline_ && pending_restore_idx_.has_value()) {
        const int    idx = pending_restore_idx_.value();
        const double sp  = pending_restore_slot_pos_.value_or(0.0);
        if (timeline_->restoreCursor(idx, sp)) {
            logger_->info("state.db cursor restored — idx={} slot={:.3f}",
                          idx, sp);
        } else {
            logger_->warn("state.db cursor restore skipped — idx={} out of range "
                          "(playlist size={})", idx, timeline_->getPlaylistSize());
        }
        pending_restore_idx_.reset();
        pending_restore_slot_pos_.reset();
    }
    loop_->start();
    if (content_sync_) content_sync_->start();
    running_.store(true, std::memory_order_release);
    startHardSwitchPoll();
    startPeriodicStatePoll();
    logger_->info("running");
    requestStateSave();   // fix17 — paused=false now visible to recovery
    if (event_bus_) {
        event_bus_->publish(
            liveqx::events::EventType::ChannelStateChange, id_,
            {{"channel_id", id_},
             {"name",       cfg_.value("name", std::string{})},
             {"state",      "running"}});
    }
    return true;
}

void ChannelInstance::stop() {
    std::lock_guard<std::mutex> lk(state_mu_);
    if (!running_.load(std::memory_order_acquire)) return;
    stopHardSwitchPoll();
    stopPeriodicStatePoll();
    if (content_sync_) content_sync_->stop();
    // fix34 D2.8 — detach preview tap BEFORE loop_->stop() so the encoder
    // is no longer fed from this thread. preview_mgr_->stopChannel() then
    // tears down the encoder + every session under the manager mutex.
    // Order matters: detaching the tap first means any in-flight tap call
    // finishes before stopChannel destroys the encoder.
    if (loop_) loop_->setFrameTap(nullptr);
    if (preview_mgr_) preview_mgr_->stopChannel(id_);
    if (loop_) loop_->stop();
    if (encoder_) {
        encoder_->onPacket(nullptr); // discard flush packets — output already draining
        encoder_->close();
    }
    if (out_mgr_) out_mgr_->stopAll();
    content_sync_.reset();
    loop_.reset();
    encoder_.reset();
    out_mgr_.reset();
    srt_out_ = nullptr;
    // fix31 Layer 2 — drain the graveyard. ContentSync/loop teardown above
    // dropped their last shared_ptr<IClip> handles into bury(); waiting for
    // the worker to chew through them here keeps a subsequent play() from
    // racing against in-flight ~VideoClip stopThreads() for file handles or
    // FFmpeg contexts. Render loop is already stopped so the 30-50ms per
    // clip cost is on a non-realtime path.
    graveyard_.drainSync();
    // Stop heartbeat so Watchdog won't flip the channel to Failed while idle.
    metrics_->last_tick_ns.store(0, std::memory_order_relaxed);
    running_.store(false, std::memory_order_release);
    logger_->info("stopped");
    // fix17 — capture & flush whatever paused_intent_ currently holds. SIGTERM
    // path leaves it false (running channel auto-resumes on next boot);
    // pause() set it to true beforehand (operator stop survives reboot).
    requestStateSave();
    if (state_saver_) state_saver_->flush();
    if (event_bus_) {
        event_bus_->publish(
            liveqx::events::EventType::ChannelStateChange, id_,
            {{"channel_id", id_},
             {"name",       cfg_.value("name", std::string{})},
             {"state",      paused_intent_ ? "paused" : "stopped"}});
    }
}

void ChannelInstance::pause() {
    {
        std::lock_guard<std::mutex> lk(state_mu_);
        paused_intent_ = true;
    }
    if (running_.load(std::memory_order_acquire)) {
        // Hot path: stop() will requestStateSave + flush at the end.
        stop();
        return;
    }
    // Channel was already stopped — stop() would early-return without
    // touching state.db. Persist the intent flip directly so a fresh
    // build()→pause() (without play()) still survives a restart.
    requestStateSave();
    if (state_saver_) state_saver_->flush();
}

bool ChannelInstance::buildRuntime() {
    srt_out_ = nullptr;
    numa::runOnNode(numa_node_, [&] {
        encoder_ = std::make_unique<Encoder>(enc_cfg_);
    });
    encoder_->setLogger(logger_);

    out_mgr_ = std::make_unique<OutputManager>();

    // outputs[] was validated in buildLongLived. Empty list (no outputs[]
    // at all, or every entry rejected) → fall back to a default SRT so the
    // channel is usable. This matches fix10/fix11 behaviour where bad
    // output cfg silently fell back to SRT@9000.
    if (!cfg_.contains("outputs") || !cfg_["outputs"].is_array()
            || cfg_["outputs"].empty()) {
        logger_->warn("no usable outputs in config — defaulting to SRT@9000");
        cfg_["outputs"] = json::array({
            json{ {"id", "default"}, {"type", "srt"},
                  {"port", 9000}, {"latency_ms", 200} }
        });
    }

    for (const auto& oc : cfg_["outputs"]) {
        const std::string oid  = oc.value("id",   std::string("default"));
        const std::string type = oc.value("type", std::string("srt"));
        std::shared_ptr<IOutput> drv;
        try {
            numa::runOnNode(numa_node_, [&] {
                if (type == "srt") {
                    const int port    = oc.value("port",       9000);
                    const int latency = oc.value("latency_ms", 200);
                    auto srt = std::make_shared<SrtOutput>(
                        port, latency,
                        oc.value("bind_address", std::string{}));
                    if (!srt_out_) srt_out_ = srt.get();
                    drv = std::move(srt);
                } else if (type == "multicast") {
                    auto mc = liveqx::multicast::parseOutputCfg(oc);
                    drv = std::make_shared<MulticastOutput>(std::move(mc));
                } else if (type == "rtmp") {
                    auto rc = liveqx::rtmp::parseOutputCfg(oc);
                    drv = std::make_shared<liveqx::rtmp::RtmpOutput>(
                              std::move(rc));
                } else if (type == "hls") {
                    auto hc = liveqx::hls::parseOutputCfg(oc);
                    drv = std::make_shared<liveqx::hls::HlsOutput>(std::move(hc));
                } else if (type == "ndi") {
                    auto nc = liveqx::ndi::parseOutputCfg(oc);
                    drv = std::make_shared<liveqx::ndi::NdiOutput>(std::move(nc));
                }
            });
        } catch (const std::exception& e) {
            logger_->error("outputs[id={}]: build failed: {} — skipped",
                           oid, e.what());
            continue;
        }
        if (!drv) {
            logger_->error("outputs[id={}]: unknown type '{}' — skipped",
                           oid, type);
            continue;
        }
        drv->setNumaNode(numa_node_);
        drv->setLogger(logger_);
        drv->setChannelId(std::to_string(id_));
        const std::uint64_t qlim = oc.value("queue_bytes_limit",
                                            OutputManager::kDefaultQueueBytesLimit);
        if (!out_mgr_->addDriver(oid, drv, qlim)) {
            logger_->error("outputs[id={}]: addDriver failed", oid);
            continue;
        }
        if (type == "ndi") {
            // Attach AFTER addDriver — start() ran inside startAll() later,
            // but the encoder hook only fires once frames are pushed, by
            // which time start() has either succeeded or marked us !running_.
            auto nd = std::static_pointer_cast<liveqx::ndi::NdiOutput>(drv);
            nd->attachEncoder(encoder_.get(), fps_);
        }
    }

    encoder_->onPacket([mgr = out_mgr_.get()](const Packet& pkt) { mgr->send(pkt); });
    if (srt_out_)
        srt_out_->onClientConnected([enc = encoder_.get()] { enc->resetOnReconnect(); });

    loop_ = std::make_unique<RenderLoop>(fps_, *timeline_, *compositor_, *encoder_, metrics_);
    loop_->setChannelId(std::to_string(id_));
    loop_->setNumaNode(numa_node_);
    loop_->setLogger(logger_);
    loop_->enablePreloader(preload_sec_);
    loop_->onClipBoundary([this](const ClipBoundaryEvent& ev) { onClipBoundary(ev); });

    // fix34 D2.8 — preview tap. Each composed RGBA frame goes into the
    // process-wide PreviewManager, which fans NAL units out to all the
    // channel's WebRtcSession peers. Tap is set only when a manager is
    // attached; nullptr means "no preview" (default for tests/legacy).
    if (preview_mgr_) {
        loop_->setFrameTap(
            [pm = preview_mgr_, id = id_](const Frame& f) {
                pm->onChannelFrame(id, f);
            });
    }

    if (cfg_.contains("content_source")) {
        const auto& cs_cfg = cfg_["content_source"];
        ContentSync::Config cs;
        const std::string source = cs_cfg.value("source_path", std::string{});
        if (!source.empty()) {
            // Passthrough mode — local folder, no cache.
            cs.share_dir = source;
            cs.cache_dir.clear();
        } else {
            cs.share_dir = cs_cfg.value("share_path", std::string{});
            cs.cache_dir = cs_cfg.value("cache_path", std::string{});
        }
        const auto mb = cs_cfg.value("max_file_size_mb", 0u);
        cs.max_file_size_bytes = static_cast<std::uint64_t>(mb) * 1024ull * 1024ull;
        cs.scan_interval = std::chrono::milliseconds(cs_cfg.value("scan_interval_ms", 2000));
        cs.video_width  = width_;
        cs.video_height = height_;
        cs.default_photo_duration_sec = cfg_.value("default_photo_duration", 10.0);
        cs.default_transition         = parse_default_transition(cfg_);
        cs.numa_node                  = numa_node_;

        const bool cache_mode_ok = !cs.share_dir.empty() && !cs.cache_dir.empty();
        const bool passthrough_ok = !cs.share_dir.empty() && cs.cache_dir.empty()
                                    && !source.empty();
        if (!cache_mode_ok && !passthrough_ok) {
            logger_->error("content_source needs either source_path "
                           "(passthrough) or share_path+cache_path (cache)");
        } else {
            auto* loop_ptr = loop_.get();
            content_sync_ = std::make_unique<ContentSync>(
                std::move(cs), *timeline_, graveyard_,
                [loop_ptr] { return loop_ptr->activeClipIndex(); },
                decode_pool_, metrics_, std::to_string(id_));
            content_sync_->setLogger(logger_);
            const auto restored = content_sync_->restoreFromDisk();
            logger_->info("ContentSync configured (mode={}, restored {} clips)",
                          content_sync_->usesCache() ? "cache" : "passthrough", restored);
        }
    }
    return true;
}

ChannelInstance::~ChannelInstance() {
    stop();
    // fix17 — order matters: kill saver thread first (its capture lambda
    // references timeline_/scheduler_, which the rest of the dtor will tear
    // down). state_persist_ then closes the SQLite handle.
    state_saver_.reset();
    state_persist_.reset();
}

// ─── transport / config ───────────────────────────────────────────────────────

void ChannelInstance::skipToNext() {
    if (timeline_) timeline_->skipToNext();
    requestStateSave();
}

bool ChannelInstance::updateConfig(const json& patch) {
    std::lock_guard<std::mutex> lk(state_mu_);
    // Reject fields that are immutable after build (require recreation)
    // or that need a full rebuild we don't support hot.
    for (const char* fld : {"resolution", "fps", "id", "name"}) {
        if (patch.contains(fld)) return false;
    }
    // fix10/11: refuse hot-swap of output transport. Changing
    // port/type/address/url requires socket recreation which collides with
    // an actively-encoding pipeline; operators must stop+update+play to
    // switch (RTMP url carries the target host plus the stream key).
    if (patch.contains("output")) {
        const auto& op = patch["output"];
        if (op.contains("port") || op.contains("type")
                || op.contains("address") || op.contains("url"))
            return false;
    }
    // fix12 c3: outputs[] mutations go through dedicated REST endpoints
    // (POST/DELETE/PATCH /channels/{id}/outputs/{id}) added in c4-c6.
    // Refuse PATCH /channels/{id} bodies that try to rewrite outputs[]
    // wholesale — that would bypass per-driver lifecycle (start/stop).
    if (patch.contains("outputs")) return false;
    // content_source can be patched only while stopped — runtime ContentSync
    // is bound to the cache_dir at buildRuntime().
    if (patch.contains("content_source") &&
        running_.load(std::memory_order_acquire)) return false;
    // playback_log sink/retention is bound at first play() (own_sink_ writer
    // thread + open files). Hot-swapping while running would have to tear
    // down a live writer mid-event — refuse and require a stop/play.
    if (patch.contains("playback_log") &&
        running_.load(std::memory_order_acquire)) return false;

    // fix9: schedule hot-reload. Validate parse BEFORE mutating any
    // in-memory state so a bad patch leaves the channel exactly as it was.
    // patch["schedule"] = null is the RFC 7396 "delete" convention and is
    // treated as "clear all entries".
    std::vector<liveqx::scheduling::ScheduleEntry> new_schedule;
    bool has_schedule_patch = false;
    if (patch.contains("schedule")) {
        const auto& sj = patch["schedule"];
        has_schedule_patch = true;
        if (!sj.is_null()) {
            try {
                new_schedule = liveqx::scheduling::parseSchedule(sj);
            } catch (const std::exception& e) {
                if (logger_)
                    logger_->error("schedule: hot-reload rejected — {}", e.what());
                return false;
            }
        }
    }

    if (patch.contains("bitrate"))       enc_cfg_.video_bitrate = patch.value("bitrate",       enc_cfg_.video_bitrate);
    if (patch.contains("preset"))        enc_cfg_.preset        = patch.value("preset",        enc_cfg_.preset);
    if (patch.contains("encoder_mode"))  enc_cfg_.encoder_mode  = patch.value("encoder_mode",  enc_cfg_.encoder_mode);
    if (patch.contains("gpu_index"))     enc_cfg_.gpu_index     = patch.value("gpu_index",     enc_cfg_.gpu_index);
    if (patch.contains("video_codec")) {
        std::string vc = patch.value("video_codec", enc_cfg_.video_codec);
        std::transform(vc.begin(), vc.end(), vc.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (vc == "mpeg2") vc = "mpeg2video";
        if (vc == "h264" || vc == "mpeg2video") {
            enc_cfg_.video_codec = vc;
        } else {
            logger_->warn("patch video_codec=\"{}\" unknown, keeping {}",
                          vc, enc_cfg_.video_codec);
        }
    }
    if (patch.contains("max_b_frames")) {
        const int mbf = patch.value("max_b_frames", enc_cfg_.max_b_frames);
        if (mbf >= 0 && mbf <= 16)
            enc_cfg_.max_b_frames = mbf;
        else
            logger_->warn("patch max_b_frames={} out of range, ignored", mbf);
    }
    if (patch.contains("default_photo_duration"))
        cfg_["default_photo_duration"] = patch["default_photo_duration"];
    if (patch.contains("default_transition"))
        cfg_["default_transition"] = patch["default_transition"];
    if (patch.contains("audio")) {
        const auto& a = patch["audio"];
        if (a.contains("bitrate"))     enc_cfg_.audio_bitrate = a.value("bitrate", enc_cfg_.audio_bitrate);
        if (a.contains("sample_rate")) enc_cfg_.sample_rate   = a.value("sample_rate", enc_cfg_.sample_rate);
    }
    if (patch.contains("mpegts") && patch["mpegts"].is_object()) {
        const auto& m = patch["mpegts"];
        auto clamp16 = [&](int v, const char* n, int fallback) {
            if (v < 1 || v > 0xFFFF) {
                logger_->warn("patch mpegts.{}={} out of range [1..65535], keeping {}", n, v, fallback);
                return fallback;
            }
            return v;
        };
        if (m.contains("service_name"))
            enc_cfg_.service_name = m.value("service_name", enc_cfg_.service_name);
        if (m.contains("service_provider"))
            enc_cfg_.service_provider = m.value("service_provider", enc_cfg_.service_provider);
        if (m.contains("service_id"))
            enc_cfg_.service_id = clamp16(m.value("service_id", enc_cfg_.service_id),
                                          "service_id", enc_cfg_.service_id);
        if (m.contains("transport_stream_id"))
            enc_cfg_.transport_stream_id = clamp16(m.value("transport_stream_id", enc_cfg_.transport_stream_id),
                                                   "transport_stream_id", enc_cfg_.transport_stream_id);
        if (m.contains("original_network_id"))
            enc_cfg_.original_network_id = clamp16(m.value("original_network_id", enc_cfg_.original_network_id),
                                                   "original_network_id", enc_cfg_.original_network_id);
        if (m.contains("mux_rate")) {
            int64_t mr = m.value("mux_rate", enc_cfg_.mux_rate);
            enc_cfg_.mux_rate = mr < 0 ? 0 : mr;
        }
        if (m.contains("sdt_period_ms")) {
            int v = m.value("sdt_period_ms", enc_cfg_.sdt_period_ms);
            enc_cfg_.sdt_period_ms = v < 0 ? 0 : v;
        }
        if (m.contains("pat_period_ms")) {
            int v = m.value("pat_period_ms", enc_cfg_.pat_period_ms);
            enc_cfg_.pat_period_ms = v < 0 ? 0 : v;
        }
    }
    // fix33 C — channel_timezone hot-swap:
    //   patch.channel_timezone == null / "" → switch to inherit-server-tz
    //   patch.channel_timezone == "Asia/..." → explicit override
    // Меняет inherits_server_tz_ + хот-свопит Scheduler через setChannelTz.
    // Валидация IANA-имени происходит выше по стеку (ControlApi/REST).
    bool has_tz_patch = false;
    if (patch.contains("channel_timezone")) {
        has_tz_patch = true;
        const auto& tz = patch["channel_timezone"];
        if (tz.is_null() ||
            (tz.is_string() && tz.get<std::string>().empty())) {
            inherits_server_tz_ = true;
        } else if (tz.is_string()) {
            inherits_server_tz_ = false;
        } else {
            return false;  // non-string / non-null type
        }
    }

    // cfg_ stays in sync so future play() rebuilds reflect the patch.
    cfg_.merge_patch(patch);

    if (has_tz_patch && scheduler_) {
        const std::string eff = effectiveTimezone();
        scheduler_->setChannelTz(eff);
        if (logger_)
            logger_->info("channel_timezone hot-swapped: effective={} (inherits={})",
                          eff, inherits_server_tz_);
    }

    // fix9: apply schedule patch atomically. scheduler_ is always non-null
    // since buildLongLived (eager creation), so setEntries works whether
    // the channel had a schedule before or not. The next clip boundary (or
    // the hard-switch poll for hard windows) will pick up the new
    // decisions naturally — REGULAR↔SCHEDULE transitions follow the usual
    // path with no special-cased "exit" needed here.
    if (has_schedule_patch) {
        scheduler_->setEntries(std::move(new_schedule));
        if (logger_)
            logger_->info("schedule: hot-reloaded ({} entries)",
                          scheduler_->entries().size());
    }

    // fix7: persist accepted patch under the same lock that mutated cfg_.
    // Crash mid-write leaves either old or new file (rename(2) is atomic
    // on a single FS); writer threads racing PATCH/GET observe the new
    // file only after the in-memory update succeeded.
    try {
        persistConfig();
    } catch (const std::exception& e) {
        if (logger_) logger_->error("persistConfig failed: {}", e.what());
        // We deliberately keep the in-memory mutation. Caller already saw
        // the patch take effect; rolling back to a stale on-disk state
        // would silently re-introduce the old config on next restart and
        // is worse than a logged disk error operators can investigate.
    }
    // fix17: schedule mutations or any other state-affecting patch should
    // refresh state.db so a hot-reload + crash doesn't lose schedule_active.
    requestStateSave();
    return true;
}

void ChannelInstance::persistConfig() const {
    if (channel_dir_.empty()) return;       // legacy mode — no persistence
    PathUtils::atomicWriteFile(channel_dir_ / "config.json", cfg_.dump(2));
}

// ─── status snapshot ──────────────────────────────────────────────────────────

nlohmann::json ChannelInstance::status() const {
    json out;
    out["id"]            = id_;
    out["name"]          = name_;
    out["state"]         = running_.load(std::memory_order_acquire)
                              ? healthStateName(health_->state())
                              : "stopped";
    out["resolution"]    = std::to_string(width_) + "x" + std::to_string(height_);
    out["fps_target"]    = fps_;
    out["numa_node"]     = numa_node_;
    out["bitrate"]       = enc_cfg_.video_bitrate;
    out["preset"]        = enc_cfg_.preset;
    out["max_b_frames"]  = enc_cfg_.max_b_frames;
    out["encoder_mode"]  = enc_cfg_.encoder_mode;
    out["gpu_index"]     = enc_cfg_.gpu_index;
    out["video_codec"]   = enc_cfg_.video_codec;
    out["mpegts"]        = {
        {"service_name",        enc_cfg_.service_name},
        {"service_provider",    enc_cfg_.service_provider},
        {"service_id",          enc_cfg_.service_id},
        {"transport_stream_id", enc_cfg_.transport_stream_id},
        {"original_network_id", enc_cfg_.original_network_id},
        {"mux_rate",            enc_cfg_.mux_rate},
        {"sdt_period_ms",       enc_cfg_.sdt_period_ms},
        {"pat_period_ms",       enc_cfg_.pat_period_ms},
    };
    out["preload_sec"]      = cfg_.value("preload_sec", preload_sec_);
    // fix33 C — channel_timezone surface:
    //   channel_timezone   : explicit override string or null (inherit)
    //   inherits_server_tz : derived flag for UI checkbox
    //   effective_timezone : resolved value scheduler currently uses
    if (inherits_server_tz_) {
        out["channel_timezone"] = nullptr;
    } else {
        out["channel_timezone"] = cfg_.value("channel_timezone", std::string("UTC"));
    }
    out["inherits_server_tz"] = inherits_server_tz_;
    out["effective_timezone"] = effectiveTimezone();
    out["default_photo_duration"] = cfg_.value("default_photo_duration", 10.0);
    {
        // Mirror the parsed default_transition so the UI can show current
        // values (the form was previously stateless w.r.t. backend).
        const auto tc = parse_default_transition(cfg_);
        out["default_transition"] = {
            {"type",     transitionTypeName(tc.type)},
            {"duration", tc.duration_sec},
            {"easing",   easingName(tc.easing)},
        };
    }
    {
        std::size_t pool_bytes = 0;
        if (decode_pool_) pool_bytes += decode_pool_->allocatedBytes();
        if (render_pool_) pool_bytes += render_pool_->allocatedBytes();
        out["frame_pool_bytes"] = pool_bytes;
    }
    {
        // Mirror the configured fallback image path so the UI can display
        // and edit it. FallbackClip itself is built once at buildLongLived
        // time — patching this field requires a channel restart.
        std::string fb;
        if (cfg_.contains("fallback") && cfg_["fallback"].is_object())
            fb = cfg_["fallback"].value("image_path", std::string{});
        out["fallback"] = { {"image_path", fb} };
    }
    // fix12 c3: outputs[] is authoritative. Emits an array of per-driver
    // statusJson() with id+queue_drops fields when running; reflects last-
    // known cfg when stopped. The deprecated singular "output" field is
    // mirrored from outputs[0] only when there is exactly one entry —
    // multi-output channels intentionally drop the field so REST consumers
    // are forced to migrate to outputs[].
    if (out_mgr_) {
        out["outputs"] = out_mgr_->statusJson();
    } else if (cfg_.contains("outputs") && cfg_["outputs"].is_array()) {
        out["outputs"] = cfg_["outputs"];
    } else {
        out["outputs"] = nlohmann::json::array();
    }
    if (out["outputs"].is_array() && out["outputs"].size() == 1) {
        out["output"] = out["outputs"][0];
    }

    if (cfg_.contains("content_source")) {
        const auto& cs = cfg_["content_source"];
        const std::string source = cs.value("source_path", std::string{});
        if (!source.empty()) {
            out["content_source"] = {
                {"mode",        "passthrough"},
                {"source_path", source},
            };
        } else {
            out["content_source"] = {
                {"mode",       "cache"},
                {"share_path", cs.value("share_path", std::string{})},
                {"cache_path", cs.value("cache_path", std::string{})},
            };
        }
    } else {
        out["content_source"] = nullptr;
    }

    if (cfg_.contains("playback_log") && cfg_["playback_log"].is_object()) {
        const auto& pl = cfg_["playback_log"];
        json plOut;
        plOut["sink"] = pl.value("sink", std::string{"none"});
        if (pl.contains("retention_days") && pl["retention_days"].is_number_integer())
            plOut["retention_days"] = pl["retention_days"].get<int>();
        out["playback_log"] = std::move(plOut);
    } else {
        out["playback_log"] = nullptr;
    }

    int    cur_idx       = -1;
    double remaining_sec = 0.0;
    std::string cur_path;
    if (timeline_) {
        cur_idx       = timeline_->getActiveIndex();
        remaining_sec = timeline_->getRemainingTime();
    }
    out["current_clip_index"] = cur_idx;
    out["current_clip_remaining_sec"] = remaining_sec;

    if (metrics_) {
        const auto s = metrics_->snapshot();
        out["fps_actual"]     = s.actual_fps;
        out["frames_dropped"] = s.frames_dropped;
        out["frames_rendered"]= s.frames_rendered;
    }
    // Legacy field kept for older REST clients. Always false for non-SRT outputs.
    out["srt_connected"] = (srt_out_ && srt_out_->isHealthy());
    return out;
}

// ─── Outputs API (fix12 c4) ───────────────────────────────────────────────────

ChannelInstance::OutputResult ChannelInstance::addOutput(const nlohmann::json& body) {
    std::lock_guard<std::mutex> lk(state_mu_);
    return addOutputLocked(body);
}

ChannelInstance::OutputResult
ChannelInstance::addOutputLocked(const nlohmann::json& body) {
    if (!body.is_object()) return OutputResult::BadJson;

    const std::string oid  = body.value("id",   std::string{});
    const std::string type = body.value("type", std::string{});
    if (oid.empty() || type.empty()) return OutputResult::BadJson;

    // Uniqueness check against current cfg_ outputs.
    if (cfg_.contains("outputs") && cfg_["outputs"].is_array()) {
        for (const auto& e : cfg_["outputs"]) {
            if (e.is_object() && e.value("id", std::string{}) == oid)
                return OutputResult::DuplicateId;
        }
    }

    // Per-type parse validation surfaces shape problems before we touch
    // the live OutputManager.
    try {
        if (type == "srt") {
            const int port = body.value("port", 9000);
            if (port < 1 || port > 65535)
                throw std::runtime_error("port out of range");
        } else if (type == "multicast") {
            (void) liveqx::multicast::parseOutputCfg(body);
        } else if (type == "rtmp") {
            (void) liveqx::rtmp::parseOutputCfg(body);
        } else if (type == "hls") {
            (void) liveqx::hls::parseOutputCfg(body);
        } else if (type == "ndi") {
            (void) liveqx::ndi::parseOutputCfg(body);
        } else {
            return OutputResult::BadJson;
        }
    } catch (const std::exception& e) {
        if (logger_) logger_->error("addOutput[id={}]: bad cfg: {}", oid, e.what());
        return OutputResult::BadJson;
    }

    // If the channel is running, build + start + register the driver now;
    // a stopped channel just stages the entry into cfg_ for next play().
    if (running_.load(std::memory_order_acquire) && out_mgr_) {
        std::shared_ptr<IOutput> drv;
        try {
            numa::runOnNode(numa_node_, [&] {
                if (type == "srt") {
                    const int port    = body.value("port",       9000);
                    const int latency = body.value("latency_ms", 200);
                    drv = std::make_shared<SrtOutput>(
                        port, latency,
                        body.value("bind_address", std::string{}));
                } else if (type == "multicast") {
                    auto mc = liveqx::multicast::parseOutputCfg(body);
                    drv = std::make_shared<MulticastOutput>(std::move(mc));
                } else if (type == "rtmp") {
                    auto rc = liveqx::rtmp::parseOutputCfg(body);
                    drv = std::make_shared<liveqx::rtmp::RtmpOutput>(
                              std::move(rc));
                } else if (type == "hls") {
                    auto hc = liveqx::hls::parseOutputCfg(body);
                    drv = std::make_shared<liveqx::hls::HlsOutput>(std::move(hc));
                } else if (type == "ndi") {
                    auto nc = liveqx::ndi::parseOutputCfg(body);
                    drv = std::make_shared<liveqx::ndi::NdiOutput>(std::move(nc));
                }
            });
        } catch (const std::exception& e) {
            logger_->error("addOutput[id={}]: build threw: {}", oid, e.what());
            return OutputResult::BuildFailed;
        }
        if (!drv) return OutputResult::BuildFailed;

        drv->setNumaNode(numa_node_);
        drv->setLogger(logger_);
        drv->setChannelId(std::to_string(id_));
        if (!drv->start()) {
            logger_->error("addOutput[id={}]: driver start failed", oid);
            drv->stop();
            return OutputResult::StartFailed;
        }
        // Wire SRT-specific keyframe-reset hook for the new driver. Multiple
        // SRT outputs each call resetOnReconnect — encoder treats it as
        // idempotent so a second SRT receiver tuning in still gets a fresh I.
        if (type == "srt") {
            auto* srt_ptr = static_cast<SrtOutput*>(drv.get());
            if (!srt_out_) srt_out_ = srt_ptr;
            if (encoder_) {
                srt_ptr->onClientConnected(
                    [enc = encoder_.get()] { enc->resetOnReconnect(); });
            }
        } else if (type == "ndi" && encoder_) {
            auto nd = std::static_pointer_cast<liveqx::ndi::NdiOutput>(drv);
            nd->attachEncoder(encoder_.get(), fps_);
        }
        const std::uint64_t qlim = body.value("queue_bytes_limit",
                                              OutputManager::kDefaultQueueBytesLimit);
        if (!out_mgr_->addDriver(oid, drv, qlim)) {
            // Should not happen — uniqueness was checked above. Defensive.
            drv->stop();
            return OutputResult::DuplicateId;
        }
    }

    // Stage the entry in cfg_ so persist + future rebuilds see it. If the
    // channel is stopped this is the only state mutation; on a running
    // channel it keeps cfg_ in sync with the live OutputManager.
    if (!cfg_.contains("outputs") || !cfg_["outputs"].is_array())
        cfg_["outputs"] = nlohmann::json::array();
    cfg_["outputs"].push_back(body);

    try { persistConfig(); }
    catch (const std::exception& e) {
        if (logger_) logger_->error("addOutput: persistConfig failed: {}", e.what());
    }
    if (logger_) logger_->info("output[id={}] added (type={})", oid, type);
    requestStateSave();
    if (event_bus_) {
        event_bus_->publish(
            liveqx::events::EventType::OutputStateChange, id_,
            {{"channel_id", id_},
             {"output_id",  oid},
             {"type",       type},
             {"state",      "added"}});
    }
    return OutputResult::Ok;
}

ChannelInstance::OutputResult
ChannelInstance::removeOutput(const std::string& output_id) {
    std::lock_guard<std::mutex> lk(state_mu_);
    return removeOutputLocked(output_id);
}

ChannelInstance::OutputResult
ChannelInstance::removeOutputLocked(const std::string& output_id) {
    if (output_id.empty()) return OutputResult::NotFound;

    if (!cfg_.contains("outputs") || !cfg_["outputs"].is_array())
        return OutputResult::NotFound;

    auto& arr = cfg_["outputs"];
    int idx = -1;
    std::string removed_type;
    for (std::size_t i = 0; i < arr.size(); ++i) {
        const auto& e = arr[i];
        if (e.is_object() && e.value("id", std::string{}) == output_id) {
            idx = static_cast<int>(i);
            removed_type = e.value("type", std::string{});
            break;
        }
    }
    if (idx < 0) return OutputResult::NotFound;

    // On a running channel, evict the driver from OutputManager (joins the
    // pump thread) and stop it to release the socket. The shared_ptr we
    // hold via getDriver keeps the driver alive until we exit this scope —
    // ensures driver->stop() happens after pump join.
    if (running_.load(std::memory_order_acquire) && out_mgr_) {
        auto drv = out_mgr_->getDriver(output_id);
        out_mgr_->removeDriver(output_id);
        if (drv) drv->stop();

        // If the cached srt_out_ pointed to the removed driver, recompute
        // it from the surviving SRT entries (if any). Encoder reset hooks
        // wired on the deceased driver simply die with it.
        if (srt_out_ && removed_type == "srt") {
            srt_out_ = nullptr;
            for (std::size_t i = 0; i < arr.size(); ++i) {
                if (static_cast<int>(i) == idx) continue;
                const auto& e = arr[i];
                if (!e.is_object()) continue;
                if (e.value("type", std::string{}) != "srt") continue;
                auto next = out_mgr_->getDriver(e.value("id", std::string{}));
                if (next) {
                    srt_out_ = static_cast<SrtOutput*>(next.get());
                    break;
                }
            }
        }
    }

    arr.erase(arr.begin() + idx);

    try { persistConfig(); }
    catch (const std::exception& e) {
        if (logger_) logger_->error("removeOutput: persistConfig failed: {}", e.what());
    }
    if (logger_) logger_->info("output[id={}] removed (type={})", output_id, removed_type);
    requestStateSave();
    if (event_bus_) {
        event_bus_->publish(
            liveqx::events::EventType::OutputStateChange, id_,
            {{"channel_id", id_},
             {"output_id",  output_id},
             {"type",       removed_type},
             {"state",      "removed"}});
    }
    return OutputResult::Ok;
}

ChannelInstance::OutputResult
ChannelInstance::patchOutput(const std::string& output_id,
                              const nlohmann::json& body) {
    if (output_id.empty())   return OutputResult::NotFound;
    if (!body.is_object())   return OutputResult::BadJson;
    // The URL id is authoritative — the body may either omit "id" or carry
    // a value that matches. A mismatched body id is a client error.
    if (body.contains("id") && body["id"] != output_id)
        return OutputResult::BadJson;

    std::lock_guard<std::mutex> lk(state_mu_);

    auto rm = removeOutputLocked(output_id);
    if (rm != OutputResult::Ok) return rm;          // NotFound — nothing to patch

    auto with_id = body;
    with_id["id"] = output_id;
    auto add = addOutputLocked(with_id);
    if (add != OutputResult::Ok && logger_) {
        // The old driver is gone; the channel is left without that output.
        // Caller must POST a fresh entry to recover.
        logger_->warn("patchOutput[id={}]: rebuild failed (code {}); "
                      "channel left without that output", output_id, int(add));
    }
    return add;
}

OutputHealthSummary ChannelInstance::outputsHealth() const {
    if (!out_mgr_) return {};
    const auto c = out_mgr_->healthCounts();
    return {c.healthy, c.total};
}

liveqx::profiler::ChannelProfiler* ChannelInstance::profiler() noexcept {
    return loop_ ? &loop_->profiler() : nullptr;
}

const liveqx::profiler::ChannelProfiler*
ChannelInstance::profiler() const noexcept {
    return loop_ ? &loop_->profiler() : nullptr;
}

nlohmann::json
ChannelInstance::outputStatusJson(const std::string& output_id) const {
    if (!out_mgr_) return nullptr;
    const auto arr = out_mgr_->statusJson();
    if (!arr.is_array()) return nullptr;
    for (const auto& e : arr) {
        if (e.is_object() && e.value("id", std::string{}) == output_id)
            return e;
    }
    return nullptr;
}

nlohmann::json ChannelInstance::outputsJson() const {
    if (out_mgr_) return out_mgr_->statusJson();
    std::lock_guard<std::mutex> lk(state_mu_);
    if (!cfg_.contains("outputs") || !cfg_["outputs"].is_array())
        return nlohmann::json::array();

    // Stopped-channel shape needs to match running shape so the UI sees the
    // same `transport` discriminator regardless of state — otherwise the
    // OutputFormModal can't tell SRT from Multicast and falls back to "srt".
    // We mirror `type` → `transport` for the four IP-transport drivers and
    // `type=ndi` → `mode=ndi` to follow IOutput::statusJson() convention.
    auto outs = cfg_["outputs"];
    for (auto& o : outs) {
        if (!o.is_object()) continue;
        const auto t = o.value("type", std::string{});
        if (t == "srt" || t == "rtmp" || t == "multicast" || t == "hls") {
            if (!o.contains("transport")) o["transport"] = t;
        } else if (t == "ndi") {
            if (!o.contains("mode")) o["mode"] = "ndi";
        }
    }
    return outs;
}

nlohmann::json ChannelInstance::liveStatusJson() const {
    // Walk the current playlist snapshot and collect every LiveClip's
    // statusJson(). Snapshot is a stable shared_ptr — safe to read from
    // any thread without holding state_mu_, but we still grab the lock
    // to read timeline_ (the unique_ptr itself can be replaced by
    // build/teardown).
    std::shared_ptr<const PlaylistSnapshot> snap;
    {
        std::lock_guard<std::mutex> lk(state_mu_);
        if (!timeline_) return nlohmann::json::array();
        snap = timeline_->snapshot();
    }
    auto out = nlohmann::json::array();
    if (!snap) return out;
    for (std::size_t i = 0; i < snap->clips.size(); ++i) {
        auto* live = dynamic_cast<liveqx::LiveClip*>(snap->clips[i].get());
        if (!live) continue;
        auto j = live->statusJson();
        j["playlist_index"] = i;
        out.push_back(std::move(j));
    }
    return out;
}

nlohmann::json ChannelInstance::watcherStatus() const {
    std::lock_guard lk(state_mu_);
    if (!content_sync_) return nullptr;
    auto j = content_sync_->statusJson();
    j["channel_id"] = id_;
    return j;
}

bool ChannelInstance::requestRescan() {
    std::lock_guard lk(state_mu_);
    if (!content_sync_) return false;
    content_sync_->requestRescan();
    return true;
}

// ─── Playlist API helpers ─────────────────────────────────────────────────────

bool ChannelInstance::managedByContentSync() const {
    if (!cfg_.contains("content_source")) return false;
    const auto& cs = cfg_["content_source"];
    return !cs.value("share_path",  std::string{}).empty() ||
           !cs.value("source_path", std::string{}).empty();
}

std::unique_ptr<IClip> ChannelInstance::buildClipFromItem(
    const json& item_json, std::string& out_path, TransitionConfig& out_tc) const {
    const double default_dur = cfg_.value("default_photo_duration", 10.0);
    const auto   default_tc  = parse_default_transition(cfg_);

    // fix13 c9: type=live entries take a different build path. The
    // PlaylistEntry parser already validated the JSON shape; we delegate
    // input construction to LiveInputFactory and wrap it in a LiveClip.
    // Timeline cursor sees a regular IClip with a finite duration; the
    // state machine fires from RenderLoop's per-tick onTick.
    using namespace liveqx::playlist;
    EntryKind kind = EntryKind::File;
    try {
        kind = parseEntryKind(item_json);
    } catch (...) {
        // Fall through to file-entry path so the legacy error message is
        // produced ("playlist item missing 'path'").
    }

    if (kind == EntryKind::Live) {
        auto entry = parseEntry(item_json, default_dur);

        TransitionConfig tc = default_tc;
        if (item_json.contains("transition")) {
            const auto& tj = item_json["transition"];
            tc.type         = parse_transition_type(tj.value("type",  "crossfade"));
            tc.mode         = parse_transition_mode(tj.value("mode",  "freeze_fade"));
            tc.duration_sec = tj.value("duration", 2.0);
            tc.easing       = parse_easing(tj.value("easing", "linear"));
        }

        auto input = liveqx::live_input::build(entry.live.input,
                                                        width_, height_);
        auto live  = std::make_unique<liveqx::LiveClip>(
            entry.live.live, std::move(input));
        live->setLogger(logger_);
        live->setChannelId(std::to_string(id_));

        // Build the OnLossProvider — the clip serves these frames whenever
        // it is not in Live state (Idle / WarmingUp / Lost / Finished).
        // If the operator asked for "fallback_clip" but the channel didn't
        // load one (no fallback image), silently downgrade to "black" so
        // the channel still has a valid provider.
        std::string mode = entry.live.fallback_on_loss;
        if (mode == "fallback_clip" && !fallback_clip_) {
            logger_->warn("[live id={}] fallback_on_loss=fallback_clip but "
                          "channel has no fallback image — using black",
                          entry.live.id);
            mode = "black";
        }
        liveqx::OnLossSources sources;
        sources.fallback_clip = fallback_clip_;
        // For "freeze" the upstream's last decoded frame is exposed via
        // LiveInputClip::getTailFrame() — LiveClip forwards to it. Capture
        // the LiveClip pointer (raw — it co-lives with the provider; the
        // provider is owned by the same LiveClip) so the lambda can pull.
        auto* live_raw = live.get();
        sources.tail_video    = [live_raw]() { return live_raw->getTailFrame(); };
        sources.black_width   = width_;
        sources.black_height  = height_;
        try {
            live->setOnLossProvider(
                liveqx::makeOnLossProvider(mode, std::move(sources)));
        } catch (const std::exception& e) {
            logger_->warn("[live id={}] makeOnLossProvider failed ({}) — "
                          "clip will return invalid frames outside Live",
                          entry.live.id, e.what());
        }

        // Don't call prepare() here — the input opens (RTMP handshake /
        // multicast join) on its own warm-up window via Timeline's
        // prepare-callback. Calling prepare() now would tie up the
        // playlist-build path on a slow handshake.
        out_path = "live:" + entry.live.id;
        out_tc   = tc;
        return live;
    }

    PlaylistItem item;
    item.path             = item_json.value("path", std::string{});
    item.display_duration = item_json.value("duration", default_dur);
    item.transition       = default_tc;
    item.numa_node        = numa_node_;
    if (item_json.contains("transition")) {
        const auto& tj = item_json["transition"];
        item.transition.type         = parse_transition_type(tj.value("type",  "crossfade"));
        item.transition.mode         = parse_transition_mode(tj.value("mode",  "freeze_fade"));
        item.transition.duration_sec = tj.value("duration", 2.0);
        item.transition.easing       = parse_easing(tj.value("easing", "linear"));
    }
    if (item.path.empty())
        throw std::runtime_error("playlist item missing 'path'");

    auto clip = ClipFactory::create(item, width_, height_, decode_pool_, metrics_);
    clip->setLogger(logger_);
    clip->setChannelId(std::to_string(id_));
    if (item.transition.mode != TransitionMode::HardCut
            && item.transition.duration_sec > 0.0) {
        clip->setHeadBufferSeconds(item.transition.duration_sec);
    }
    clip->prepare();
    out_path = item.path;
    out_tc   = item.transition;
    return clip;
}

nlohmann::json ChannelInstance::playlistJson() const {
    json arr = json::array();
    if (!timeline_) return arr;
    auto snap = timeline_->snapshot();
    for (std::size_t i = 0; i < snap->clips.size(); ++i) {
        json item;
        item["index"]    = static_cast<int>(i);
        item["path"]     = i < snap->cache_paths.size() ? snap->cache_paths[i] : "";
        item["duration"] = snap->clips[i] ? snap->clips[i]->getDuration() : 0.0;
        if (i < snap->transitions.size()) {
            const auto& tr = snap->transitions[i];
            const char* tname = "hardcut";
            switch (tr.type) {
                case TransitionType::CrossFade:   tname = "crossfade";  break;
                case TransitionType::WipeLeft:    tname = "wipe_left";  break;
                case TransitionType::WipeRight:   tname = "wipe_right"; break;
                case TransitionType::WipeUp:      tname = "wipe_up";    break;
                case TransitionType::WipeDown:    tname = "wipe_down";  break;
                case TransitionType::PushLeft:    tname = "push_left";  break;
                case TransitionType::PushRight:   tname = "push_right"; break;
                case TransitionType::PushUp:      tname = "push_up";    break;
                case TransitionType::PushDown:    tname = "push_down";  break;
                case TransitionType::Dissolve:    tname = "dissolve";   break;
                case TransitionType::FadeToBlack: tname = "fade_black"; break;
                case TransitionType::HardCut:     tname = "hardcut";    break;
            }
            const char* mname = "hardcut";
            switch (tr.mode) {
                case TransitionMode::HardCut:    mname = "hardcut";     break;
                case TransitionMode::FreezeFade: mname = "freeze_fade"; break;
                case TransitionMode::LiveMix:    mname = "live_mix";    break;
            }
            item["transition"] = {
                {"type",     tname},
                {"mode",     mname},
                {"duration", tr.duration_sec},
                {"easing",   easingName(tr.easing)},
            };
        }
        item["pending_remove"] =
            i < snap->pending_remove.size() && snap->pending_remove[i];
        arr.push_back(std::move(item));
    }
    return arr;
}

ChannelInstance::PlaylistResult
ChannelInstance::replacePlaylist(const json& items) {
    if (managedByContentSync()) return PlaylistResult::ManagedByContentSync;
    if (!items.is_array())      return PlaylistResult::BadJson;

    std::vector<std::unique_ptr<IClip>> clips;
    std::vector<TransitionConfig>       transitions;
    std::vector<std::string>            paths;
    clips.reserve(items.size());
    transitions.reserve(items.size());
    paths.reserve(items.size());

    for (const auto& item_json : items) {
        if (!item_json.is_object()) return PlaylistResult::BadJson;
        std::string path; TransitionConfig tc;
        try {
            auto clip = buildClipFromItem(item_json, path, tc);
            clips.push_back(std::move(clip));
            transitions.push_back(tc);
            paths.push_back(path);
        } catch (const std::exception& e) {
            logger_->error("replacePlaylist failed for '{}': {}",
                           item_json.value("path", std::string{}), e.what());
            return PlaylistResult::ItemBuildFailed;
        }
    }

    timeline_->setPlaylist(wrapClips(std::move(clips), graveyard_),
                           std::move(transitions), std::move(paths));
    logger_->info("playlist replaced ({} items)", items.size());
    requestStateSave();
    return PlaylistResult::Ok;
}

ChannelInstance::PlaylistResult
ChannelInstance::appendPlaylist(const json& items, int* out_first_idx) {
    if (managedByContentSync()) return PlaylistResult::ManagedByContentSync;
    if (!items.is_array())      return PlaylistResult::BadJson;

    int first = -1;
    for (const auto& item_json : items) {
        if (!item_json.is_object()) return PlaylistResult::BadJson;
        std::string path; TransitionConfig tc;
        try {
            auto clip = buildClipFromItem(item_json, path, tc);
            const int idx = timeline_->getPlaylistSize();
            if (first < 0) first = idx;
            timeline_->appendClip(wrapClip(std::move(clip), graveyard_),
                                  tc, std::move(path));
        } catch (const std::exception& e) {
            logger_->error("appendPlaylist failed for '{}': {}",
                           item_json.value("path", std::string{}), e.what());
            // Partial-append: leave already-added items, surface the error.
            if (out_first_idx) *out_first_idx = first;
            return PlaylistResult::ItemBuildFailed;
        }
    }
    if (out_first_idx) *out_first_idx = first;
    logger_->info("appended {} items (first idx={})", items.size(), first);
    requestStateSave();
    return PlaylistResult::Ok;
}

ChannelInstance::PlaylistResult
ChannelInstance::removeAt(int idx, bool* out_was_active) {
    if (managedByContentSync()) return PlaylistResult::ManagedByContentSync;
    if (!timeline_)             return PlaylistResult::IndexOutOfRange;
    if (idx < 0)                return PlaylistResult::IndexOutOfRange;

    const auto r = timeline_->removeAt(static_cast<std::size_t>(idx));
    if (out_was_active) *out_was_active = (r == Timeline::RemoveResult::MarkedActive);
    switch (r) {
        case Timeline::RemoveResult::NotFound:
            return PlaylistResult::IndexOutOfRange;
        case Timeline::RemoveResult::Removed:
            logger_->info("playlist[{}] removed", idx);
            requestStateSave();
            return PlaylistResult::Ok;
        case Timeline::RemoveResult::MarkedActive:
            logger_->info("playlist[{}] marked pending (active)", idx);
            requestStateSave();
            return PlaylistResult::Ok;
    }
    return PlaylistResult::Ok;
}

ChannelInstance::PlaylistResult ChannelInstance::clearPlaylist() {
    if (managedByContentSync()) return PlaylistResult::ManagedByContentSync;
    if (!timeline_)             return PlaylistResult::Ok;
    timeline_->setPlaylist(std::vector<std::shared_ptr<IClip>>{}, {}, {});
    logger_->info("playlist cleared (fallback only)");
    requestStateSave();
    return PlaylistResult::Ok;
}

ChannelInstance::PlaylistResult
ChannelInstance::notifyDeleted(const std::string& path) {
    if (path.empty())  return PlaylistResult::BadJson;
    if (!timeline_)    return PlaylistResult::NotFound;

    if (!timeline_->markForRemoval(path)) return PlaylistResult::NotFound;

    // Drain non-active matches immediately. The active match (if any) stays
    // marked pending_remove; Preloader::tick() reaps it at the natural slot
    // wrap after a smooth crossfade to fallback or the next clip.
    auto snap = timeline_->snapshot();
    const int active_idx = timeline_->getActiveIndex();
    std::string active_cache;
    if (active_idx >= 0 && static_cast<std::size_t>(active_idx) < snap->cache_paths.size())
        active_cache = snap->cache_paths[active_idx];
    auto evicted = timeline_->reapRemovable(active_cache);
    logger_->info("notify-deleted '{}' — {} entries evicted now",
                  path, evicted.size());
    requestStateSave();
    return PlaylistResult::Ok;
}

// ─── Schedule REST helpers (fix9 step 6) ──────────────────────────────────────

nlohmann::json ChannelInstance::scheduleJson() const {
    if (!scheduler_) return json::array();
    return liveqx::scheduling::serializeSchedule(scheduler_->entries());
}

nlohmann::json ChannelInstance::scheduleActiveJson() const {
    if (!scheduler_) {
        return json{
            {"mode",          "regular"},
            {"entry_id",      nullptr},
            {"window_end_ns", nullptr},
        };
    }
    using namespace std::chrono;
    const int64_t now_ns = duration_cast<nanoseconds>(
                               system_clock::now().time_since_epoch()).count();
    return scheduler_->statusJson(now_ns);
}

bool ChannelInstance::replaceSchedule(const json& items) {
    return updateConfig(json{{"schedule", items}});
}

nlohmann::json ChannelInstance::scheduleUpcomingJson(int64_t within_sec) const {
    auto out = json::array();
    if (!scheduler_) return out;

    // Clamp horizon to a safe range. Negative or zero → no upcoming.
    // 30 days upper bound is also the bounded loop budget in
    // Scheduler::nextActivation; passing more would waste cycles without
    // returning extra results.
    constexpr int64_t kMaxHorizonSec = 30LL * 24 * 3600;
    if (within_sec <= 0) return out;
    if (within_sec > kMaxHorizonSec) within_sec = kMaxHorizonSec;

    using namespace std::chrono;
    const int64_t now_ns = duration_cast<nanoseconds>(
                               system_clock::now().time_since_epoch()).count();

    // openapi ScheduleUpcomingItem: starts_at/ends_at — UnixTimestamp (sec).
    // Конвертим из ns в sec на границе REST, чтобы клиенты не делили сами.
    for (const auto& u : scheduler_->upcoming(now_ns, within_sec)) {
        out.push_back(json{
            {"entry_id",  u.entry_id},
            {"starts_at", u.starts_at_ns / 1'000'000'000LL},
            {"ends_at",   u.ends_at_ns   / 1'000'000'000LL},
        });
    }
    return out;
}

// ─── Playback log (fix8) ──────────────────────────────────────────────────────

void ChannelInstance::setSqliteSink(
    liveqx::logging::SqlitePlaybackSink* sink) {
    sqlite_sink_ = sink;
}

void ChannelInstance::initPlaybackSink() {
    std::lock_guard<std::mutex> lk(state_mu_);
    setupPlaybackSink();
}

// ─── EventBus wiring (fix23) ─────────────────────────────────────────────────

void ChannelInstance::setEventBus(liveqx::events::EventBus* bus) {
    event_bus_ = bus;
    // ChannelHealth needs the bus + the integer channel id (for SSE role
    // filtering keyed on channel_grants.channel_id) to publish HealthChange.
    if (health_) health_->setEventBus(bus, id_);
}

// ─── PreviewManager wiring (fix34 D2.8) ──────────────────────────────────────

void ChannelInstance::setPreviewManager(
    liveqx::preview::PreviewManager* pv) {
    preview_mgr_ = pv;
    // Hot-attach to a running channel: install the tap immediately so the
    // operator can subscribe to preview without restarting playback.
    if (loop_ && running_.load(std::memory_order_acquire)) {
        if (pv) {
            loop_->setFrameTap(
                [pm = pv, id = id_](const Frame& f) {
                    pm->onChannelFrame(id, f);
                });
        } else {
            loop_->setFrameTap(nullptr);
        }
    }
}

void ChannelInstance::setupPlaybackSink() {
    using namespace liveqx::logging;

    std::string sink_type = "none";
    int retention_days    = 0;
    if (cfg_.contains("playback_log")) {
        const auto& pl = cfg_["playback_log"];
        sink_type      = pl.value("sink", "none");
        retention_days = pl.value("retention_days", 0);
    }

    // Already-bound sink survives play/stop only when the cfg matches what we
    // built last. Otherwise (operator stopped → PATCH playback_log → play
    // again) we need to swap. Without this re-check the very first play()
    // — when cfg often has no playback_log block — would lock the channel
    // to NullSink for the lifetime of the process.
    if (effective_sink_
        && current_sink_type_ == sink_type
        && current_retention_days_ == retention_days) {
        return;
    }

    // Tear down the previous sink before building the new one. own_sink_
    // (Null/File) is destroyed in place; the shared SqlitePlaybackSink is
    // unregistered so its writer thread stops touching the per-channel db.
    if (effective_sink_) {
        if (current_sink_type_ == "db" && sqlite_sink_) {
            sqlite_sink_->unregisterChannel(id_);
        }
        own_sink_.reset();
        effective_sink_ = nullptr;
    }

    auto fall_back_to_null = [&](std::string_view why) {
        if (logger_) logger_->error("playback_log: {} — using null sink", why);
        own_sink_              = std::make_unique<NullPlaybackSink>();
        effective_sink_        = own_sink_.get();
        current_sink_type_     = "none";
        current_retention_days_ = 0;
    };

    if (sink_type == "none") {
        own_sink_              = std::make_unique<NullPlaybackSink>();
        effective_sink_        = own_sink_.get();
        current_sink_type_     = "none";
        current_retention_days_ = 0;
    } else if (sink_type == "file") {
        if (channel_dir_.empty()) {
            fall_back_to_null("sink=file requires per-channel layout (channel_dir)");
        } else {
            try {
                own_sink_              = std::make_unique<FilePlaybackSink>(id_, channel_dir_);
                effective_sink_        = own_sink_.get();
                current_sink_type_     = "file";
                current_retention_days_ = 0;
                logger_->info("playback_log: file sink at {}/playback",
                              channel_dir_.string());
            } catch (const std::exception& e) {
                fall_back_to_null(std::string("FilePlaybackSink ctor failed: ") + e.what());
            }
        }
    } else if (sink_type == "db") {
        if (channel_dir_.empty()) {
            fall_back_to_null("sink=db requires per-channel layout");
        } else if (!sqlite_sink_) {
            fall_back_to_null("sink=db but no SqlitePlaybackSink injected (call setSqliteSink before play)");
        } else {
            try {
                sqlite_sink_->registerChannel(id_, channel_dir_, retention_days);
                effective_sink_         = sqlite_sink_;
                current_sink_type_      = "db";
                current_retention_days_ = retention_days;
                logger_->info("playback_log: db sink (retention_days={})",
                              retention_days);
            } catch (const std::exception& e) {
                fall_back_to_null(std::string("registerChannel failed: ") + e.what());
            }
        }
    } else {
        fall_back_to_null("unknown sink type '" + sink_type + "'");
    }
}

void ChannelInstance::onClipBoundary(const ClipBoundaryEvent& ev) {
    if (effective_sink_) {
        liveqx::logging::PlaybackEvent pe;
        pe.channel_id      = id_;
        pe.clip_path       = ev.prev_path;
        pe.clip_type       = ev.prev_type;
        pe.started_at_ns   = ev.started_at_ns;
        pe.ended_at_ns     = ev.ended_at_ns;
        pe.played_sec      = ev.played_sec;
        pe.transition_type = transitionTypeName(ev.transition);
        pe.status          = ev.status;
        pe.error_reason    = ev.error_reason;
        effective_sink_->log(pe);
    }
    // fix17 — boundary is the most precise cursor save point: slot_pos_sec
    // just rolled back to 0, playlist_index advanced. Debouncer collapses
    // back-to-back boundaries with the periodic poll.
    requestStateSave();
    // fix23 — surface clip transitions to UI subscribers. Payload mirrors
    // the playback log shape so SSE consumers and the proof-of-play feed
    // stay aligned.
    if (event_bus_) {
        event_bus_->publish(
            liveqx::events::EventType::ClipChange, id_,
            {{"channel_id",      id_},
             {"prev_path",       ev.prev_path},
             {"prev_type",       ev.prev_type},
             {"played_sec",      ev.played_sec},
             {"transition_type", transitionTypeName(ev.transition)},
             {"status",          ev.status},
             {"error_reason",    ev.error_reason}});
    }
    // fix9: drive the schedule state machine. ev.ended_at_ns is the wall-clock
    // instant at which the boundary was observed — using it (rather than
    // re-sampling system_clock here) keeps decisions stable if the dispatcher
    // is briefly delayed.
    applyScheduleBoundary(ev.ended_at_ns);
}

// ─── Schedule integration (fix9) ──────────────────────────────────────────────

void ChannelInstance::applyScheduleBoundary(int64_t now_ns) {
    if (!schedule_ctrl_) return;
    std::lock_guard<std::mutex> lk(schedule_swap_mu_);
    const auto a = schedule_ctrl_->onBoundary(now_ns);
    using Kind = liveqx::scheduling::ScheduleController::ActionKind;
    switch (a.kind) {
        case Kind::None:
            return;
        case Kind::EnterSchedule:
        case Kind::SwitchEntry:
            logger_->info("schedule: {} → '{}' ({} clips, until ns={})",
                          a.kind == Kind::EnterSchedule ? "ENTER" : "SWITCH",
                          a.new_entry_id, a.playlist.size(), a.window_end_ns);
            swapToSchedulePlaylist(a.playlist, a.transition);
            if (event_bus_) {
                event_bus_->publish(
                    liveqx::events::EventType::ScheduleActive, id_,
                    {{"channel_id",      id_},
                     {"mode",            a.kind == Kind::EnterSchedule
                                            ? "entered" : "switched"},
                     {"entry_id",        a.new_entry_id},
                     {"window_end_ns",   a.window_end_ns}});
            }
            return;
        case Kind::ExitToRegular:
            logger_->info("schedule: EXIT → regular playlist ({} items)",
                          regular_playlist_items_.size());
            swapToRegularPlaylist();
            if (event_bus_) {
                event_bus_->publish(
                    liveqx::events::EventType::ScheduleActive, id_,
                    {{"channel_id", id_},
                     {"mode",       "exited"},
                     {"entry_id",   nullptr},
                     {"window_end_ns", 0}});
            }
            return;
    }
}

void ChannelInstance::swapToSchedulePlaylist(
    const std::vector<std::string>& paths, const TransitionConfig& tc) {
    std::vector<std::unique_ptr<IClip>> clips;
    std::vector<TransitionConfig>       transitions;
    std::vector<std::string>            cache_paths;
    clips.reserve(paths.size());
    transitions.reserve(paths.size());
    cache_paths.reserve(paths.size());

    json item;
    item["transition"] = {
        {"type", [&] {
            switch (tc.type) {
                case TransitionType::CrossFade: return "crossfade";
                case TransitionType::WipeLeft:  return "wipe_left";
                case TransitionType::WipeRight: return "wipe_right";
                case TransitionType::WipeUp:    return "wipe_up";
                case TransitionType::WipeDown:  return "wipe_down";
                case TransitionType::PushLeft:  return "push_left";
                case TransitionType::PushRight: return "push_right";
                case TransitionType::PushUp:    return "push_up";
                case TransitionType::PushDown:  return "push_down";
                case TransitionType::Dissolve:    return "dissolve";
                case TransitionType::FadeToBlack: return "fade_black";
                case TransitionType::HardCut:     return "hardcut";
            }
            return "hardcut";
        }()},
        {"mode", [&] {
            switch (tc.mode) {
                case TransitionMode::HardCut:    return "hard_cut";
                case TransitionMode::FreezeFade: return "freeze_fade";
                case TransitionMode::LiveMix:    return "live_mix";
            }
            return "freeze_fade";
        }()},
        {"duration", tc.duration_sec},
        {"easing",   easingName(tc.easing)},
    };

    for (const auto& path : paths) {
        item["path"] = path;
        std::string out_path;
        TransitionConfig out_tc;
        try {
            auto clip = buildClipFromItem(item, out_path, out_tc);
            clips.push_back(std::move(clip));
            transitions.push_back(out_tc);
            cache_paths.push_back(std::move(out_path));
        } catch (const std::exception& e) {
            logger_->error("schedule: clip '{}' failed: {} — skipped", path, e.what());
        }
    }
    timeline_->setPlaylist(wrapClips(std::move(clips), graveyard_),
                           std::move(transitions),
                           std::move(cache_paths));
}

void ChannelInstance::startHardSwitchPoll() {
    if (!schedule_ctrl_) return;
    if (hard_switch_poll_.joinable()) return;
    hard_switch_stop_.store(false, std::memory_order_release);
    hard_switch_poll_ = std::jthread([this](std::stop_token st) {
        using namespace std::chrono;
        const auto period = milliseconds(1000);
        while (!st.stop_requested() &&
               !hard_switch_stop_.load(std::memory_order_acquire)) {
            const int64_t now_ns = duration_cast<nanoseconds>(
                system_clock::now().time_since_epoch()).count();
            using Kind = liveqx::scheduling::ScheduleController::ActionKind;
            std::lock_guard<std::mutex> lk(schedule_swap_mu_);
            const auto a = schedule_ctrl_->tryHardSwitch(now_ns);
            if (a.kind == Kind::EnterSchedule || a.kind == Kind::SwitchEntry) {
                logger_->info("schedule[hard]: {} → '{}' ({} clips)",
                              a.kind == Kind::EnterSchedule ? "ENTER" : "SWITCH",
                              a.new_entry_id, a.playlist.size());
                swapToSchedulePlaylist(a.playlist, a.transition);
            }
            // Sleep is interruptible via stop_token + flag; use a short
            // condition-friendly wait by chunking the period.
            for (int i = 0; i < 10 && !st.stop_requested() &&
                            !hard_switch_stop_.load(std::memory_order_acquire); ++i)
                std::this_thread::sleep_for(period / 10);
        }
    });
}

void ChannelInstance::stopHardSwitchPoll() {
    hard_switch_stop_.store(true, std::memory_order_release);
    if (hard_switch_poll_.joinable()) {
        hard_switch_poll_.request_stop();
        hard_switch_poll_.join();
    }
}

void ChannelInstance::swapToRegularPlaylist() {
    std::vector<std::unique_ptr<IClip>> clips;
    std::vector<TransitionConfig>       transitions;
    std::vector<std::string>            cache_paths;
    clips.reserve(regular_playlist_items_.size());
    transitions.reserve(regular_playlist_items_.size());
    cache_paths.reserve(regular_playlist_items_.size());

    for (const auto& item_json : regular_playlist_items_) {
        if (!item_json.is_object()) continue;
        std::string path; TransitionConfig tc;
        try {
            auto clip = buildClipFromItem(item_json, path, tc);
            clips.push_back(std::move(clip));
            transitions.push_back(tc);
            cache_paths.push_back(std::move(path));
        } catch (const std::exception& e) {
            logger_->error("regular rebuild failed for '{}': {} — skipped",
                           item_json.value("path", std::string{}), e.what());
        }
    }
    timeline_->setPlaylist(wrapClips(std::move(clips), graveyard_),
                           std::move(transitions),
                           std::move(cache_paths));
}

nlohmann::json ChannelInstance::playbackLogStatusJson() const {
    if (!effective_sink_)
        return json{{"sink_type", "none"},
                    {"queue_depth", 0},
                    {"dropped_count", 0},
                    {"last_write_ns", nullptr}};
    return effective_sink_->statusJson();
}

nlohmann::json ChannelInstance::queryPlaybackLog(
    int channel_id,
    const std::optional<int64_t>& from_ns,
    const std::optional<int64_t>& to_ns,
    const std::optional<int64_t>& after_ns,
    int limit, int offset) const {
    if (!effective_sink_)
        return json{{"events", json::array()}, {"next_after_ns", nullptr}};
    liveqx::logging::IPlaybackSink::QueryParams qp;
    qp.channel_id = channel_id;
    qp.from_ns    = from_ns;
    qp.to_ns      = to_ns;
    qp.after_ns   = after_ns;
    qp.limit      = limit;
    qp.offset     = offset;
    return effective_sink_->query(qp);
}

nlohmann::json ChannelInstance::purgePlaybackLog(
    int channel_id,
    const std::optional<int64_t>& from_ns,
    const std::optional<int64_t>& to_ns) {
    if (!effective_sink_)
        return json{{"deleted_rows", 0}, {"removed_files", 0}};
    liveqx::logging::IPlaybackSink::PurgeParams pp;
    pp.channel_id = channel_id;
    pp.from_ns    = from_ns;
    pp.to_ns      = to_ns;
    return effective_sink_->purge(pp);
}

// ─── State persistence (fix17) ───────────────────────────────────────────────

void ChannelInstance::requestStateSave() {
    if (state_saver_) state_saver_->scheduleSave();
}

liveqx::persistence::ChannelStateSnapshot
ChannelInstance::captureChannelState() const {
    liveqx::persistence::ChannelStateSnapshot snap;
    // fix17 — paused reflects operator intent, not raw run-state. SIGTERM
    // stops a running channel without flipping paused_intent_ so the next
    // boot resumes; an explicit pause() flips it so the stop survives.
    snap.paused = paused_intent_;

    if (timeline_) {
        const auto cs = timeline_->getCursorSnapshot();
        if (cs.active_idx >= 0) {
            snap.playlist_index = cs.active_idx;
            snap.slot_pos_sec   = cs.slot_pos_sec;
            // Cache_paths is the parallel array of clip paths in the
            // current snapshot — empty string for non-cache-backed clips
            // (e.g. live inputs). Use it to record the resolved path so
            // restore can sanity-check the cursor against the playlist.
            if (auto plsnap = timeline_->snapshot()) {
                if (cs.active_idx < static_cast<int>(plsnap->cache_paths.size())) {
                    const auto& p = plsnap->cache_paths[cs.active_idx];
                    if (!p.empty()) snap.clip_path = p;
                }
            }
        } else {
            snap.playlist_index = -1;
        }
    }

    if (scheduler_) {
        using namespace std::chrono;
        const int64_t now_ns = duration_cast<nanoseconds>(
            system_clock::now().time_since_epoch()).count();
        snap.schedule_active = scheduler_->statusJson(now_ns);
    }

    return snap;
}

void ChannelInstance::startPeriodicStatePoll() {
    if (!state_saver_) return;
    state_periodic_stop_.store(false, std::memory_order_release);
    state_periodic_poll_ = std::jthread([this](std::stop_token st) {
        using namespace std::chrono;
        // 2s mid-clip cadence — kill -9 loses at most ~2s of playback
        // position. Still cheap thanks to the 500ms debouncer (boundary +
        // 2s poll combine into ≤2 SQLite writes/sec).
        constexpr auto kPeriod = seconds(2);
        while (!st.stop_requested() &&
               !state_periodic_stop_.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(kPeriod);
            if (running_.load(std::memory_order_acquire))
                requestStateSave();
        }
    });
}

void ChannelInstance::stopPeriodicStatePoll() {
    state_periodic_stop_.store(true, std::memory_order_release);
    if (state_periodic_poll_.joinable()) {
        state_periodic_poll_.request_stop();
        state_periodic_poll_.join();
    }
}

// ── fix33 C — per-channel timezone helpers ───────────────────────────────────

void ChannelInstance::setServerTimezoneGetter(ServerTimezoneGetter g) {
    std::lock_guard<std::mutex> lk(state_mu_);
    server_tz_getter_ = std::move(g);
    // Если канал инхеритит и getter поменялся — стянуть актуальную TZ в scheduler.
    if (inherits_server_tz_ && scheduler_) {
        const std::string eff = server_tz_getter_ ? server_tz_getter_() : std::string{"UTC"};
        scheduler_->setChannelTz(eff.empty() ? std::string{"UTC"} : eff);
    }
}

std::string ChannelInstance::effectiveTimezone() const {
    if (!inherits_server_tz_) {
        // explicit override stored in cfg_
        return cfg_.value("channel_timezone", std::string("UTC"));
    }
    if (server_tz_getter_) {
        auto s = server_tz_getter_();
        if (!s.empty()) return s;
    }
    return std::string{"UTC"};
}

void ChannelInstance::applyServerTimezoneChange() {
    std::lock_guard<std::mutex> lk(state_mu_);
    if (!inherits_server_tz_ || !scheduler_) return;
    const std::string eff = server_tz_getter_ ? server_tz_getter_() : std::string{"UTC"};
    scheduler_->setChannelTz(eff.empty() ? std::string{"UTC"} : eff);
    if (logger_)
        logger_->info("server TZ change → channel TZ now {}", eff);
}
