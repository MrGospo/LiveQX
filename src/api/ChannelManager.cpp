#include "api/ChannelManager.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <vector>

#include "core/ChannelInstance.h"
#include "core/Watchdog.h"
#include "logging/SqlitePlaybackSink.h"
#include "metrics/ChannelHealth.h"
#include "metrics/ChannelProfiler.h"
#include "metrics/ProfileSampler.h"
#include "utils/Log.h"
#include "utils/PathUtils.h"

using nlohmann::json;
using Result = ChannelManager::Result;

const char* channelManagerResultName(Result r) noexcept {
    switch (r) {
        case Result::Ok:             return "ok";
        case Result::NotFound:       return "not_found";
        case Result::AlreadyExists:  return "already_exists";
        case Result::AlreadyRunning: return "already_running";
        case Result::AlreadyStopped: return "already_stopped";
        case Result::BuildFailed:    return "build_failed";
        case Result::StartFailed:    return "start_failed";
        case Result::BadPatch:       return "bad_patch";
        case Result::ManagedByContentSync: return "managed_by_content_sync";
        case Result::BadJson:        return "bad_json";
        case Result::ItemBuildFailed:return "item_build_failed";
        case Result::IndexOutOfRange:return "index_out_of_range";
        case Result::PathNotFound:   return "path_not_found";
        case Result::OutputIdConflict:  return "output_id_conflict";
        case Result::OutputBuildFailed: return "output_build_failed";
        case Result::OutputStartFailed: return "output_start_failed";
        case Result::OutputNotFound:    return "output_not_found";
    }
    return "unknown";
}

namespace {
using PR = ChannelInstance::PlaylistResult;
ChannelManager::Result mapPlaylist(PR r) {
    switch (r) {
        case PR::Ok:                   return ChannelManager::Result::Ok;
        case PR::ManagedByContentSync: return ChannelManager::Result::ManagedByContentSync;
        case PR::BadJson:              return ChannelManager::Result::BadJson;
        case PR::ItemBuildFailed:      return ChannelManager::Result::ItemBuildFailed;
        case PR::IndexOutOfRange:      return ChannelManager::Result::IndexOutOfRange;
        case PR::NotFound:             return ChannelManager::Result::PathNotFound;
    }
    return ChannelManager::Result::BadJson;
}

using OR = ChannelInstance::OutputResult;
ChannelManager::Result mapOutput(OR r) {
    switch (r) {
        case OR::Ok:           return ChannelManager::Result::Ok;
        case OR::BadJson:      return ChannelManager::Result::BadJson;
        case OR::DuplicateId:  return ChannelManager::Result::OutputIdConflict;
        case OR::BuildFailed:  return ChannelManager::Result::OutputBuildFailed;
        case OR::StartFailed:  return ChannelManager::Result::OutputStartFailed;
        case OR::NotFound:     return ChannelManager::Result::OutputNotFound;
    }
    return ChannelManager::Result::BadJson;
}
}  // namespace

ChannelManager::ChannelManager(Watchdog* watchdog,
                               std::filesystem::path channel_root)
    : watchdog_(watchdog), channel_root_(std::move(channel_root)) {
    if (!channel_root_.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(channel_root_, ec);
        if (ec) {
            LOG_ERROR("ChannelManager: cannot create channel_root '{}': {}",
                      channel_root_.string(), ec.message());
        }
    }
}
ChannelManager::~ChannelManager() { stopAll(); }

std::filesystem::path
ChannelManager::channelDirFor(int id, const std::string& name) const {
    if (channel_root_.empty()) return {};
    const std::string sanitized = PathUtils::sanitizeForPath(name);
    const std::string folder = sanitized.empty()
        ? ("ch" + std::to_string(id))
        : ("ch" + std::to_string(id) + "-" + sanitized);
    return channel_root_ / folder;
}

int ChannelManager::chooseIdLocked(const json& cfg) const {
    if (cfg.contains("id")) return cfg.value("id", 0);
    int next = 1;
    for (const auto& [k, _] : channels_) next = std::max(next, k + 1);
    return next;
}

ChannelInstance* ChannelManager::findLocked(int id) const {
    auto it = channels_.find(id);
    return it == channels_.end() ? nullptr : it->second.get();
}

Result ChannelManager::create(const json& cfg, int* out_id) {
    namespace fs = std::filesystem;
    std::unique_ptr<ChannelInstance> built;
    int chosen_id = 0;
    fs::path created_dir;     // empty if we did not create one (legacy mode)
    bool     dir_pre_existed = false;
    {
        std::unique_lock lk(mu_);
        chosen_id = chooseIdLocked(cfg);
        if (channels_.count(chosen_id)) return Result::AlreadyExists;

        json eff_cfg = cfg;
        eff_cfg["id"] = chosen_id;
        const std::string ch_name = eff_cfg.value("name", std::string{});
        const auto dir = channelDirFor(chosen_id, ch_name);
        if (!dir.empty()) {
            std::error_code ec;
            dir_pre_existed = fs::exists(dir);
            fs::create_directories(dir, ec);
            if (ec) {
                LOG_ERROR("ChannelManager::create: mkdir {} failed: {}",
                          dir.string(), ec.message());
                return Result::BuildFailed;
            }
            if (!dir_pre_existed) created_dir = dir;
        }
        try {
            built = ChannelInstance::build(eff_cfg, dir);
        } catch (const std::exception& e) {
            LOG_ERROR("ChannelManager::create: build failed: {}", e.what());
            if (!created_dir.empty()) {
                std::error_code ec;
                fs::remove_all(created_dir, ec);
            }
            return Result::BuildFailed;
        }
        if (!built) {
            if (!created_dir.empty()) {
                std::error_code ec;
                fs::remove_all(created_dir, ec);
            }
            return Result::BuildFailed;
        }

        // fix7: materialise config.json so the channel is recoverable on
        // restart even if the operator never sends a PATCH afterwards.
        // Only persist when we own the dir — an externally-provided dir
        // (loadFromRoot path) already has the file.
        if (!dir.empty() && !dir_pre_existed) {
            try {
                built->persistConfig();
            } catch (const std::exception& e) {
                LOG_ERROR("ChannelManager::create: persistConfig failed: {}",
                          e.what());
                std::error_code ec;
                fs::remove_all(created_dir, ec);
                return Result::BuildFailed;
            }
        }

        if (watchdog_) {
            ChannelInstance* raw = built.get();
            watchdog_->registerChannel(built->metrics(), built->health(),
                                       [raw] { return raw->outputsHealth(); });
        }
        if (sqlite_sink_) built->setSqliteSink(sqlite_sink_);
        if (event_bus_)   built->setEventBus(event_bus_);
        if (preview_mgr_) built->setPreviewManager(preview_mgr_);
        if (server_tz_getter_) built->setServerTimezoneGetter(server_tz_getter_);
        // Materialise the playback sink now so /playback-log answers correctly
        // for a stopped channel — otherwise the UI would only see history after
        // the operator hits play at least once.
        built->initPlaybackSink();
        channels_.emplace(chosen_id, std::move(built));
    }
    if (out_id) *out_id = chosen_id;
    return Result::Ok;
}

Result ChannelManager::createAndPlay(const json& cfg, int* out_id) {
    int id = 0;
    auto r = create(cfg, &id);
    if (r != Result::Ok) return r;
    if (out_id) *out_id = id;
    auto pr = play(id);
    if (pr != Result::Ok) LOG_WARN("ChannelManager: auto-play failed for {}: {}",
                                   id, channelManagerResultName(pr));
    return Result::Ok;
}

Result ChannelManager::remove(int id) {
    namespace fs = std::filesystem;
    std::unique_ptr<ChannelInstance> evicted;
    {
        std::unique_lock lk(mu_);
        auto it = channels_.find(id);
        if (it == channels_.end()) return Result::NotFound;
        evicted = std::move(it->second);
        channels_.erase(it);
    }
    // Stop + watchdog-unregister outside the manager lock — stop may take
    // hundreds of ms (encoder flush, jthread join).
    fs::path dir;
    if (evicted) {
        dir = evicted->channelDir();   // capture before destroying instance
        if (sampler_) sampler_->unregisterChannel(std::to_string(id));
        evicted->stop();
        if (watchdog_) watchdog_->unregisterChannel(evicted->metrics());
    }
    // fix7: tear down the on-disk folder only AFTER the channel is fully
    // stopped — encoder/SRT could still be flushing into logs/ otherwise.
    // Skipped in legacy mode (dir empty) and when the dir lives outside
    // channel_root_ (defensive: never rm something we don't own).
    if (!dir.empty() && !channel_root_.empty()) {
        std::error_code ec;
        const auto canon_root = fs::weakly_canonical(channel_root_, ec);
        const auto canon_dir  = fs::weakly_canonical(dir,           ec);
        const auto rel = fs::relative(canon_dir, canon_root, ec);
        const bool inside = !rel.empty() && rel.native().rfind("..", 0) != 0;
        if (inside) {
            fs::remove_all(dir, ec);
            if (ec) LOG_ERROR("ChannelManager::remove: rm -rf {} failed: {}",
                              dir.string(), ec.message());
        } else {
            LOG_WARN("ChannelManager::remove: refusing to delete {} — "
                     "outside channel_root {}", dir.string(),
                     channel_root_.string());
        }
    }
    return Result::Ok;
}

Result ChannelManager::play(int id) {
    ChannelInstance* ch = nullptr;
    {
        std::shared_lock lk(mu_);
        ch = findLocked(id);
        if (!ch) return Result::NotFound;
    }
    if (ch->isRunning()) return Result::AlreadyRunning;
    if (!ch->play()) return Result::StartFailed;
    if (sampler_) {
        if (auto* prof = ch->profiler())
            sampler_->registerChannel(std::to_string(id), prof);
    }
    return Result::Ok;
}

Result ChannelManager::stop(int id) {
    ChannelInstance* ch = nullptr;
    {
        std::shared_lock lk(mu_);
        ch = findLocked(id);
        if (!ch) return Result::NotFound;
    }
    if (!ch->isRunning()) return Result::AlreadyStopped;
    if (sampler_) sampler_->unregisterChannel(std::to_string(id));
    // fix17: REST stop is treated as an explicit pause — paused=true is
    // persisted so the next bootstrap honours the operator intent. The
    // SIGTERM path uses stop() directly (see stopAll) so a graceful
    // shutdown of a running channel does not flip paused.
    ch->pause();
    return Result::Ok;
}

Result ChannelManager::next(int id) {
    std::shared_lock lk(mu_);
    auto* ch = findLocked(id);
    if (!ch) return Result::NotFound;
    ch->skipToNext();
    return Result::Ok;
}

Result ChannelManager::updateConfig(int id, const json& patch) {
    std::shared_lock lk(mu_);
    auto* ch = findLocked(id);
    if (!ch) return Result::NotFound;
    return ch->updateConfig(patch) ? Result::Ok : Result::BadPatch;
}

json ChannelManager::playlistJson(int id) const {
    std::shared_lock lk(mu_);
    auto* ch = findLocked(id);
    if (!ch) return json();
    return ch->playlistJson();
}

Result ChannelManager::replacePlaylist(int id, const json& items) {
    std::shared_lock lk(mu_);
    auto* ch = findLocked(id);
    if (!ch) return Result::NotFound;
    return mapPlaylist(ch->replacePlaylist(items));
}

Result ChannelManager::appendPlaylist(int id, const json& items, int* out_first_idx) {
    std::shared_lock lk(mu_);
    auto* ch = findLocked(id);
    if (!ch) return Result::NotFound;
    return mapPlaylist(ch->appendPlaylist(items, out_first_idx));
}

Result ChannelManager::removeAt(int id, int idx, bool* out_was_active) {
    std::shared_lock lk(mu_);
    auto* ch = findLocked(id);
    if (!ch) return Result::NotFound;
    return mapPlaylist(ch->removeAt(idx, out_was_active));
}

Result ChannelManager::clearPlaylist(int id) {
    std::shared_lock lk(mu_);
    auto* ch = findLocked(id);
    if (!ch) return Result::NotFound;
    return mapPlaylist(ch->clearPlaylist());
}

Result ChannelManager::notifyDeleted(int id, const std::string& path) {
    std::shared_lock lk(mu_);
    auto* ch = findLocked(id);
    if (!ch) return Result::NotFound;
    return mapPlaylist(ch->notifyDeleted(path));
}

json ChannelManager::outputsJson(int id) const {
    std::shared_lock lk(mu_);
    auto* ch = findLocked(id);
    if (!ch) return json();
    return ch->outputsJson();
}

json ChannelManager::outputStatusJson(int id, const std::string& output_id) const {
    std::shared_lock lk(mu_);
    auto* ch = findLocked(id);
    if (!ch) return json();
    return ch->outputStatusJson(output_id);
}

json ChannelManager::liveStatusJson(int id) const {
    std::shared_lock lk(mu_);
    auto* ch = findLocked(id);
    if (!ch) return json();
    return ch->liveStatusJson();
}

Result ChannelManager::addOutput(int id, const json& body) {
    std::shared_lock lk(mu_);
    auto* ch = findLocked(id);
    if (!ch) return Result::NotFound;
    return mapOutput(ch->addOutput(body));
}

Result ChannelManager::removeOutput(int id, const std::string& output_id) {
    std::shared_lock lk(mu_);
    auto* ch = findLocked(id);
    if (!ch) return Result::NotFound;
    return mapOutput(ch->removeOutput(output_id));
}

Result ChannelManager::patchOutput(int id, const std::string& output_id,
                                    const json& body) {
    std::shared_lock lk(mu_);
    auto* ch = findLocked(id);
    if (!ch) return Result::NotFound;
    return mapOutput(ch->patchOutput(output_id, body));
}

json ChannelManager::scheduleJson(int id) const {
    std::shared_lock lk(mu_);
    auto* ch = findLocked(id);
    if (!ch) return json();
    return ch->scheduleJson();
}

json ChannelManager::scheduleActiveJson(int id) const {
    std::shared_lock lk(mu_);
    auto* ch = findLocked(id);
    if (!ch) return json();
    return ch->scheduleActiveJson();
}

Result ChannelManager::replaceSchedule(int id, const json& items) {
    std::shared_lock lk(mu_);
    auto* ch = findLocked(id);
    if (!ch) return Result::NotFound;
    return ch->replaceSchedule(items) ? Result::Ok : Result::BadPatch;
}

json ChannelManager::scheduleUpcomingJson(int id, int64_t within_sec) const {
    std::shared_lock lk(mu_);
    auto* ch = findLocked(id);
    if (!ch) return json();
    return ch->scheduleUpcomingJson(within_sec);
}

json ChannelManager::watcherStatus(int id) const {
    std::shared_lock lk(mu_);
    auto* ch = findLocked(id);
    if (!ch) return json();
    return ch->watcherStatus();
}

Result ChannelManager::requestRescan(int id) {
    std::shared_lock lk(mu_);
    auto* ch = findLocked(id);
    if (!ch) return Result::NotFound;
    return ch->requestRescan() ? Result::Ok : Result::NotFound;
}

json ChannelManager::listJson() const {
    json arr = json::array();
    std::shared_lock lk(mu_);
    for (const auto& [id, ch] : channels_) arr.push_back(ch->status());
    return arr;
}

json ChannelManager::statusJson(int id) const {
    std::shared_lock lk(mu_);
    auto* ch = findLocked(id);
    if (!ch) return json();
    return ch->status();
}

ChannelManager::StatusSnapshot ChannelManager::snapshotForStatus() const {
    StatusSnapshot snap;
    std::shared_lock lk(mu_);
    for (const auto& [id, ch] : channels_) {
        (void)id;
        ++snap.channels;
        const auto& h = ch->health();
        switch (h->state()) {
            case HealthState::Running:  ++snap.running;  break;
            case HealthState::Degraded: ++snap.degraded; break;
            case HealthState::Failed:   ++snap.failed;   break;
        }
        const auto oh = ch->outputsHealth();
        snap.outputs_ok    += oh.healthy;
        snap.outputs_total += oh.total;
    }
    return snap;
}

std::string ChannelManager::formatStatusLine(const StatusSnapshot& s) {
    // Compact single-line format consumed by `systemctl status` and journald.
    // Examples:
    //   ch=2 run=2 outputs=4/4
    //   ch=3 run=1 deg=1 fail=1 outputs=2/5
    std::string line = "ch=" + std::to_string(s.channels) +
                       " run=" + std::to_string(s.running);
    if (s.degraded) line += " deg="  + std::to_string(s.degraded);
    if (s.failed)   line += " fail=" + std::to_string(s.failed);
    line += " outputs=" + std::to_string(s.outputs_ok) +
            "/" + std::to_string(s.outputs_total);
    return line;
}

json ChannelManager::healthJson() const {
    json out;
    out["channels"] = json::array();
    bool any_failed = false, any_degraded = false;

    std::shared_lock lk(mu_);
    for (const auto& [id, ch] : channels_) {
        const auto& h = ch->health();
        const auto state = h->state();
        if (state == HealthState::Failed)   any_failed   = true;
        if (state == HealthState::Degraded) any_degraded = true;
        out["channels"].push_back({
            {"id",    h->channelId()},
            {"state", healthStateName(state)},
        });
    }
    out["overall"] = any_failed ? "failed" : (any_degraded ? "degraded" : "running");
    return out;
}

std::size_t ChannelManager::loadFromRoot() {
    namespace fs = std::filesystem;
    // fix16: signal "/readyz green" once we exit, regardless of how many
    // (possibly zero) channels actually came off disk. Empty channel_root_
    // and missing directory are still legitimate "loaded" states.
    struct LoadedFlagger {
        ChannelManager* self;
        ~LoadedFlagger() { self->loaded_.store(true, std::memory_order_release); }
    } flagger{this};
    if (channel_root_.empty()) return 0;
    if (!fs::is_directory(channel_root_)) return 0;

    // Deterministic order — folders sorted lexicographically so the same
    // disk layout always boots the same id sequence (matters for tests
    // and for operator expectations of "channel 7 came up first").
    std::vector<fs::path> dirs;
    for (const auto& e : fs::directory_iterator(channel_root_))
        if (e.is_directory()) dirs.push_back(e.path());
    std::sort(dirs.begin(), dirs.end());

    std::size_t loaded = 0;
    for (const auto& d : dirs) {
        const auto cfg_path = d / "config.json";
        if (!fs::exists(cfg_path)) {
            LOG_WARN("ChannelManager: skipping {} — no config.json",
                     d.filename().string());
            continue;
        }
        json cfg;
        try {
            std::ifstream f(cfg_path);
            cfg = json::parse(f);
        } catch (const std::exception& e) {
            LOG_ERROR("ChannelManager: failed to parse {}: {}",
                      cfg_path.string(), e.what());
            continue;
        }
        // Ephemeral channels (e.g. stress-test) are intentionally transient.
        // If they survived a hard kill they are garbage — delete and skip.
        if (cfg.value("_ephemeral", false)) {
            std::error_code ec;
            fs::remove_all(d, ec);
            LOG_WARN("ChannelManager: removed stale ephemeral channel dir {}",
                     d.filename().string());
            continue;
        }
        if (!cfg.contains("id")) {
            LOG_ERROR("ChannelManager: {} missing 'id' — skipping",
                      cfg_path.string());
            continue;
        }
        const int id = cfg.value("id", 0);

        std::unique_ptr<ChannelInstance> built;
        {
            std::unique_lock lk(mu_);
            if (channels_.count(id)) {
                LOG_ERROR("ChannelManager: duplicate id {} in {} — skipping",
                          id, d.string());
                continue;
            }
            try {
                // Build with the on-disk dir directly. channelDirFor would
                // re-derive the same path from (id, name), but using the
                // existing path keeps load() correct even if the folder
                // was renamed manually by an operator.
                built = ChannelInstance::build(cfg, d);
            } catch (const std::exception& e) {
                LOG_ERROR("ChannelManager: build failed for {}: {} — skipping",
                          d.string(), e.what());
                continue;
            }
            if (!built) continue;
            if (watchdog_) {
            ChannelInstance* raw = built.get();
            watchdog_->registerChannel(built->metrics(), built->health(),
                                       [raw] { return raw->outputsHealth(); });
        }
            if (sqlite_sink_) built->setSqliteSink(sqlite_sink_);
            if (event_bus_)   built->setEventBus(event_bus_);
            if (preview_mgr_) built->setPreviewManager(preview_mgr_);
            if (server_tz_getter_) built->setServerTimezoneGetter(server_tz_getter_);
            // Materialise the playback sink now so /playback-log answers
            // correctly for a stopped channel loaded from disk — otherwise the
            // UI would only see history after the operator hits play at least
            // once after every LiveQX restart.
            built->initPlaybackSink();
            channels_.emplace(id, std::move(built));
        }
        ++loaded;
        LOG_INFO("ChannelManager: loaded channel {} from {}",
                 id, d.filename().string());
    }
    return loaded;
}

void ChannelManager::setSqlitePlaybackSink(
    liveqx::logging::SqlitePlaybackSink* sink) {
    std::unique_lock lk(mu_);
    sqlite_sink_ = sink;
    // Forward to every existing channel so already-loaded channels can use
    // a sink injected after loadFromRoot (the main.cpp ordering).
    for (auto& [id, ch] : channels_) ch->setSqliteSink(sink);
}

void ChannelManager::setEventBus(liveqx::events::EventBus* bus) {
    std::unique_lock lk(mu_);
    event_bus_ = bus;
    for (auto& [id, ch] : channels_) ch->setEventBus(bus);
}

void ChannelManager::setPreviewManager(
    liveqx::preview::PreviewManager* pv) {
    std::unique_lock lk(mu_);
    preview_mgr_ = pv;
    for (auto& [id, ch] : channels_) ch->setPreviewManager(pv);
}

void ChannelManager::setServerTimezoneGetter(ServerTimezoneGetter g) {
    std::unique_lock lk(mu_);
    server_tz_getter_ = std::move(g);
    // Прокидываем getter в существующие каналы. У inherit-каналов это
    // одновременно стянет актуальную TZ в их Scheduler.
    for (auto& [id, ch] : channels_) {
        if (server_tz_getter_) ch->setServerTimezoneGetter(server_tz_getter_);
    }
}

void ChannelManager::notifyServerTimezoneChanged() {
    std::shared_lock lk(mu_);
    for (auto& [id, ch] : channels_) {
        ch->applyServerTimezoneChange();
    }
}

void ChannelManager::setProfileSampler(
        liveqx::profiler::ProfileSampler* sampler) {
    std::unique_lock lk(mu_);
    // Detach all from the previous sampler.
    if (sampler_) {
        for (auto& [id, ch] : channels_)
            sampler_->unregisterChannel(std::to_string(id));
    }
    sampler_ = sampler;
    if (sampler_) {
        for (auto& [id, ch] : channels_) {
            if (auto* prof = ch->profiler())
                sampler_->registerChannel(std::to_string(id), prof);
        }
    }
}

liveqx::profiler::ChannelProfiler*
ChannelManager::profilerFor(int id) noexcept {
    std::shared_lock lk(mu_);
    auto* ch = findLocked(id);
    return ch ? ch->profiler() : nullptr;
}

ChannelManager::Result ChannelManager::profilerStart(
        int id, liveqx::profiler::Mode mode, bool reset) {
    if (mode == liveqx::profiler::Mode::Off) return Result::BadPatch;
    std::shared_lock lk(mu_);
    auto* ch = findLocked(id);
    if (!ch) return Result::NotFound;
    auto* prof = ch->profiler();
    if (!prof) return Result::AlreadyStopped;
    if (reset) prof->reset();
    prof->setMode(mode);
    return Result::Ok;
}

ChannelManager::Result ChannelManager::profilerStop(int id) {
    std::shared_lock lk(mu_);
    auto* ch = findLocked(id);
    if (!ch) return Result::NotFound;
    auto* prof = ch->profiler();
    if (!prof) return Result::AlreadyStopped;
    prof->setMode(liveqx::profiler::Mode::Off);
    return Result::Ok;
}

json ChannelManager::profilerSnapshotJson(int id) const {
    std::shared_lock lk(mu_);
    auto* ch = findLocked(id);
    if (!ch) return json();
    const auto* prof = ch->profiler();
    if (!prof) {
        // Channel exists but is not running. Return an "off" snapshot so
        // REST callers can distinguish between unknown id (404) and stopped
        // channel (200 with mode=off).
        return json{{"mode", "off"},
                    {"running", false},
                    {"channel_id", id}};
    }
    const auto snap = prof->snapshot();
    json sampled = json::object();
    json instr   = json::object();
    json count   = json::object();
    for (std::size_t i = 0; i < liveqx::profiler::kStageCount; ++i) {
        const auto stage = static_cast<liveqx::profiler::Stage>(i);
        const std::string name = liveqx::profiler::stageName(stage);
        sampled[name] = snap.sampled_hits[i];
        instr[name]   = snap.stage_us[i];
        count[name]   = snap.stage_count[i];
    }
    return json{
        {"mode",         liveqx::profiler::modeName(snap.mode)},
        {"running",      snap.mode != liveqx::profiler::Mode::Off},
        {"channel_id",   id},
        {"active_ms",    snap.active_ms},
        {"sampled_hits", sampled},
        {"stage_us",     instr},
        {"stage_count",  count},
    };
}

json ChannelManager::playbackLogStatusJson(int id) const {
    std::shared_lock lk(mu_);
    auto* ch = findLocked(id);
    if (!ch) return json();
    return ch->playbackLogStatusJson();
}

json ChannelManager::queryPlaybackLog(int id,
                                       const std::optional<int64_t>& from_ns,
                                       const std::optional<int64_t>& to_ns,
                                       const std::optional<int64_t>& after_ns,
                                       int limit, int offset) const {
    std::shared_lock lk(mu_);
    auto* ch = findLocked(id);
    if (!ch) return json();
    return ch->queryPlaybackLog(id, from_ns, to_ns, after_ns, limit, offset);
}

json ChannelManager::purgePlaybackLog(int id,
                                       const std::optional<int64_t>& from_ns,
                                       const std::optional<int64_t>& to_ns) {
    std::shared_lock lk(mu_);
    auto* ch = findLocked(id);
    if (!ch) return json();
    return ch->purgePlaybackLog(id, from_ns, to_ns);
}

void ChannelManager::stopAll() {
    std::vector<std::pair<int, std::unique_ptr<ChannelInstance>>> all;
    {
        std::unique_lock lk(mu_);
        for (auto& [id, ch] : channels_) all.emplace_back(id, std::move(ch));
        channels_.clear();
    }
    for (auto& [id, ch] : all) {
        if (sampler_) sampler_->unregisterChannel(std::to_string(id));
        ch->stop();
        if (watchdog_) watchdog_->unregisterChannel(ch->metrics());
    }
}

std::size_t ChannelManager::size() const {
    std::shared_lock lk(mu_);
    return channels_.size();
}

void ChannelManager::forEachChannel(
    const std::function<void(const ChannelInstance&)>& fn) const {
    std::shared_lock lk(mu_);
    for (const auto& [id, inst] : channels_) {
        (void)id;
        fn(*inst);
    }
}
