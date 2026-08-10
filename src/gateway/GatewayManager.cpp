#include "gateway/GatewayManager.h"

#include <algorithm>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <vector>

#include <spdlog/spdlog.h>

#include "events/EventBus.h"
#include "utils/Log.h"
#include "utils/PathUtils.h"

namespace fs = std::filesystem;
using nlohmann::json;

namespace liveqx::gateway {

// ─── Result names ────────────────────────────────────────────────────────────

const char* gatewayManagerResultName(GatewayManager::Result r) noexcept {
    switch (r) {
        case GatewayManager::Result::Ok:               return "ok";
        case GatewayManager::Result::NotFound:         return "not_found";
        case GatewayManager::Result::AlreadyExists:    return "already_exists";
        case GatewayManager::Result::AlreadyRunning:   return "already_running";
        case GatewayManager::Result::AlreadyStopped:   return "already_stopped";
        case GatewayManager::Result::StartFailed:      return "start_failed";
        case GatewayManager::Result::BadJson:          return "bad_json";
        case GatewayManager::Result::BadPatch:         return "bad_patch";
        case GatewayManager::Result::OutputIdConflict: return "output_id_conflict";
        case GatewayManager::Result::OutputNotFound:   return "output_not_found";
    }
    return "unknown";
}

namespace {

// Auto-assign sub-ids ("out0", "out1", …) to outputs that omitted "id".
// Returns BadPatch if duplicates remain after assignment. We assign in the
// order we see outputs so the same JSON always yields the same ids.
GatewayManager::Result normaliseOutputIds(GatewayCfg& cfg) {
    std::set<std::string> taken;
    for (const auto& o : cfg.outputs) {
        if (!o.id.empty()) {
            if (!taken.insert(o.id).second)
                return GatewayManager::Result::OutputIdConflict;
        }
    }
    int n = 0;
    for (auto& o : cfg.outputs) {
        if (!o.id.empty()) continue;
        std::string candidate;
        do {
            candidate = "out" + std::to_string(n++);
        } while (taken.count(candidate));
        o.id = candidate;
        taken.insert(candidate);
    }
    return GatewayManager::Result::Ok;
}

} // namespace

// ─── Construction ────────────────────────────────────────────────────────────

GatewayManager::GatewayManager(fs::path gateway_root)
    : gateway_root_(std::move(gateway_root)) {
    if (!gateway_root_.empty()) {
        std::error_code ec;
        fs::create_directories(gateway_root_, ec);
        if (ec) {
            LOG_ERROR("GatewayManager: cannot create gateway_root '{}': {}",
                      gateway_root_.string(), ec.message());
        }
    }
}

GatewayManager::~GatewayManager() {
    stopAll();
}

// ─── Helpers ─────────────────────────────────────────────────────────────────

fs::path GatewayManager::gatewayDirFor(int id, const std::string& name) const {
    if (gateway_root_.empty()) return {};
    const std::string sanitized = PathUtils::sanitizeForPath(name);
    const std::string folder = sanitized.empty()
        ? ("gw" + std::to_string(id))
        : ("gw" + std::to_string(id) + "-" + sanitized);
    return gateway_root_ / folder;
}

int GatewayManager::chooseIdLocked(const json& cfg) const {
    if (cfg.contains("id")) {
        if (!cfg["id"].is_number_integer())
            throw std::invalid_argument("'id' must be integer");
        return cfg["id"].get<int>();
    }
    int next = 1;
    for (const auto& [k, _] : gateways_) next = std::max(next, k + 1);
    return next;
}

IGateway* GatewayManager::findLocked(int id) const {
    auto it = gateways_.find(id);
    return it == gateways_.end() ? nullptr : it->second.get();
}

void GatewayManager::persistConfigLocked(int id,
                                         const std::string& name,
                                         const GatewayCfg& cfg) const {
    if (gateway_root_.empty()) return;
    const auto dir = gatewayDirFor(id, name);
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) throw std::runtime_error(
        "mkdir " + dir.string() + " failed: " + ec.message());
    json j = toJson(cfg);
    j["id"]   = id;
    j["name"] = name;
    PathUtils::atomicWriteFile(dir / "config.json", j.dump(2));
}

// ─── CRUD ────────────────────────────────────────────────────────────────────

GatewayManager::Result
GatewayManager::create(const json& cfg, int* out_id) {
    if (!cfg.is_object()) return Result::BadJson;

    int chosen_id = 0;
    std::string name;
    GatewayCfg parsed_cfg;
    try {
        parsed_cfg = parseGatewayCfg(cfg);
        if (cfg.contains("name")) {
            if (!cfg["name"].is_string()) return Result::BadJson;
            name = cfg["name"].get<std::string>();
        }
    } catch (const std::exception& e) {
        LOG_ERROR("GatewayManager::create: parse failed: {}", e.what());
        return Result::BadJson;
    }
    if (auto r = normaliseOutputIds(parsed_cfg); r != Result::Ok) return r;

    fs::path created_dir;     // empty if we did not create one
    bool created_new = false;
    {
        std::unique_lock lk(mu_);
        try {
            chosen_id = chooseIdLocked(cfg);
        } catch (const std::exception& e) {
            LOG_ERROR("GatewayManager::create: bad id: {}", e.what());
            return Result::BadJson;
        }
        if (gateways_.count(chosen_id)) return Result::AlreadyExists;

        const auto dir = gatewayDirFor(chosen_id, name);
        if (!dir.empty()) {
            std::error_code ec;
            const bool pre_existed = fs::exists(dir);
            fs::create_directories(dir, ec);
            if (ec) {
                LOG_ERROR("GatewayManager::create: mkdir {} failed: {}",
                          dir.string(), ec.message());
                return Result::StartFailed;
            }
            if (!pre_existed) {
                created_dir = dir;
                created_new = true;
            }
        }

        try {
            persistConfigLocked(chosen_id, name, parsed_cfg);
        } catch (const std::exception& e) {
            LOG_ERROR("GatewayManager::create: persistConfig failed: {}", e.what());
            if (created_new) {
                std::error_code ec;
                fs::remove_all(created_dir, ec);
            }
            return Result::StartFailed;
        }

        std::unique_ptr<IGateway> gw;
        try {
            gw = makeGateway(chosen_id, name, parsed_cfg);
        } catch (const std::exception& e) {
            LOG_ERROR("GatewayManager::create: makeGateway failed: {}", e.what());
            if (created_new) {
                std::error_code ec;
                fs::remove_all(created_dir, ec);
            }
            return Result::BadJson;
        }
        gateways_.emplace(chosen_id, std::move(gw));
    }

    if (out_id) *out_id = chosen_id;
    return Result::Ok;
}

GatewayManager::Result
GatewayManager::createAndPlay(const json& cfg, int* out_id) {
    int id = 0;
    auto r = create(cfg, &id);
    if (r != Result::Ok) return r;
    if (out_id) *out_id = id;
    auto pr = play(id);
    if (pr != Result::Ok) {
        LOG_WARN("GatewayManager: auto-play failed for {}: {}",
                 id, gatewayManagerResultName(pr));
    }
    return Result::Ok;
}

GatewayManager::Result GatewayManager::remove(int id) {
    std::unique_ptr<IGateway> evicted;
    fs::path dir;
    {
        std::unique_lock lk(mu_);
        auto it = gateways_.find(id);
        if (it == gateways_.end()) return Result::NotFound;
        dir     = gatewayDirFor(id, it->second->name());
        evicted = std::move(it->second);
        gateways_.erase(it);
    }
    if (evicted) evicted->stop();

    if (!dir.empty() && !gateway_root_.empty()) {
        std::error_code ec;
        const auto canon_root = fs::weakly_canonical(gateway_root_, ec);
        const auto canon_dir  = fs::weakly_canonical(dir,           ec);
        const auto rel = fs::relative(canon_dir, canon_root, ec);
        const bool inside = !rel.empty() && rel.native().rfind("..", 0) != 0;
        if (inside) {
            fs::remove_all(dir, ec);
            if (ec) LOG_ERROR("GatewayManager::remove: rm -rf {} failed: {}",
                              dir.string(), ec.message());
        } else {
            LOG_WARN("GatewayManager::remove: refusing to delete {} — "
                     "outside gateway_root {}", dir.string(),
                     gateway_root_.string());
        }
    }
    return Result::Ok;
}

GatewayManager::Result GatewayManager::play(int id) {
    IGateway* gw = nullptr;
    {
        std::shared_lock lk(mu_);
        gw = findLocked(id);
        if (!gw) return Result::NotFound;
    }
    if (gw->isRunning()) return Result::AlreadyRunning;
    if (gw->start()) {
        publishStateChange(gw->id(), gw->name(), "running");
        return Result::Ok;
    }
    publishStateChange(gw->id(), gw->name(), "failed");
    return Result::StartFailed;
}

GatewayManager::Result GatewayManager::stop(int id) {
    IGateway* gw = nullptr;
    {
        std::shared_lock lk(mu_);
        gw = findLocked(id);
        if (!gw) return Result::NotFound;
    }
    if (!gw->isRunning()) return Result::AlreadyStopped;
    gw->stop();
    publishStateChange(gw->id(), gw->name(), "stopped");
    return Result::Ok;
}

GatewayManager::Result
GatewayManager::patch(int id, const json& body) {
    if (!body.is_object()) return Result::BadJson;
    if (body.contains("input")) {
        // Rebind requires close+open of the recv socket; we punt to
        // delete+create so the operator gets explicit control.
        return Result::BadPatch;
    }
    if (!body.contains("name") && !body.contains("outputs") && !body.contains("fec"))
        return Result::BadPatch;

    std::unique_lock lk(mu_);
    IGateway* gw = findLocked(id);
    if (!gw) return Result::NotFound;

    GatewayCfg new_cfg = gw->cfg();
    std::string new_name = gw->name();

    if (body.contains("name")) {
        if (!body["name"].is_string()) return Result::BadJson;
        new_name = body["name"].get<std::string>();
    }
    if (body.contains("outputs")) {
        if (!body["outputs"].is_array() || body["outputs"].empty())
            return Result::BadPatch;
        try {
            new_cfg.outputs.clear();
            for (const auto& jo : body["outputs"])
                new_cfg.outputs.push_back(parseOutputCfg(jo));
        } catch (const std::exception& e) {
            LOG_ERROR("GatewayManager::patch: parse outputs failed: {}", e.what());
            return Result::BadJson;
        }
        if (auto r = normaliseOutputIds(new_cfg); r != Result::Ok) return r;
    }
    if (body.contains("fec")) {
        try {
            new_cfg.fec = parseFecCfg(body["fec"]);
        } catch (const std::exception& e) {
            LOG_ERROR("GatewayManager::patch: parse fec failed: {}", e.what());
            return Result::BadJson;
        }
        // Re-run cross-validation: when FEC is on, output port + max FEC
        // offset must fit in the UDP port range. Mirrors parseGatewayCfg().
        if (new_cfg.fec.enabled) {
            const int max_off = std::max<int>(
                new_cfg.fec.column_port_offset,
                new_cfg.fec.mode == FecCfg::Mode::TwoD
                    ? new_cfg.fec.row_port_offset : 0);
            for (const auto& o : new_cfg.outputs) {
                if (o.port + max_off > 65535) {
                    LOG_ERROR("GatewayManager::patch: fec offset exceeds "
                              "65535 for output port {}", o.port);
                    return Result::BadJson;
                }
            }
        }
    }

    const fs::path old_dir = gatewayDirFor(id, gw->name());
    const fs::path new_dir = gatewayDirFor(id, new_name);

    // Persist new cfg first — if disk write fails, runtime stays untouched.
    try {
        persistConfigLocked(id, new_name, new_cfg);
    } catch (const std::exception& e) {
        LOG_ERROR("GatewayManager::patch: persistConfig failed: {}", e.what());
        return Result::StartFailed;
    }

    // Hot-swap: stop, swap cfg, restart on a brand-new Gateway instance so
    // Impl/socket state is fully fresh. We could in theory mutate cfg in
    // place and call start() again, but rebuilding is simpler and safer.
    const bool was_running = gw->isRunning();
    if (was_running) gw->stop();

    std::unique_ptr<IGateway> new_gw;
    try {
        new_gw = makeGateway(id, new_name, new_cfg);
    } catch (const std::exception& e) {
        LOG_ERROR("GatewayManager::patch: makeGateway failed: {}", e.what());
        return Result::BadJson;
    }
    auto& slot = gateways_[id];
    slot = std::move(new_gw);

    if (was_running) {
        if (!slot->start()) {
            LOG_ERROR("GatewayManager::patch: start() failed after swap, "
                      "gateway[{}] left stopped", id);
            publishStateChange(id, new_name, "failed");
            return Result::StartFailed;
        }
        publishStateChange(id, new_name, "running");
    }

    if (old_dir != new_dir && !old_dir.empty() && fs::exists(old_dir)) {
        std::error_code ec;
        fs::remove_all(old_dir, ec);
        if (ec) LOG_WARN("GatewayManager::patch: rm old dir {} failed: {}",
                         old_dir.string(), ec.message());
    }

    return Result::Ok;
}

// ─── Read-only access ────────────────────────────────────────────────────────

json GatewayManager::listJson() const {
    std::shared_lock lk(mu_);
    json arr = json::array();
    for (const auto& [_, gw] : gateways_) arr.push_back(gw->statusJson());
    return arr;
}

json GatewayManager::statusJson(int id) const {
    std::shared_lock lk(mu_);
    IGateway* gw = findLocked(id);
    if (!gw) return nullptr;
    return gw->statusJson();
}

void GatewayManager::forEachGateway(
        const std::function<void(const IGateway&)>& fn) const {
    std::shared_lock lk(mu_);
    for (const auto& [_, gw] : gateways_) fn(*gw);
}

std::size_t GatewayManager::size() const {
    std::shared_lock lk(mu_);
    return gateways_.size();
}

// ─── Bootstrap ───────────────────────────────────────────────────────────────

std::size_t GatewayManager::loadFromRoot() {
    if (gateway_root_.empty()) return 0;

    std::error_code ec;
    if (!fs::exists(gateway_root_, ec)) return 0;

    std::vector<fs::path> entries;
    for (const auto& de : fs::directory_iterator(gateway_root_, ec)) {
        if (ec) break;
        if (!de.is_directory()) continue;
        const auto leaf = de.path().filename().string();
        if (leaf.rfind("gw", 0) != 0) continue;
        entries.push_back(de.path());
    }
    std::sort(entries.begin(), entries.end());

    std::size_t loaded = 0;
    for (const auto& dir : entries) {
        const auto cfg_path = dir / "config.json";
        if (!fs::exists(cfg_path, ec)) {
            LOG_WARN("GatewayManager::loadFromRoot: {} missing config.json, skipping",
                     dir.string());
            continue;
        }
        std::ifstream f(cfg_path);
        json j;
        try {
            f >> j;
        } catch (const std::exception& e) {
            LOG_ERROR("GatewayManager::loadFromRoot: parse {} failed: {}",
                      cfg_path.string(), e.what());
            continue;
        }
        int id = 0;
        std::string name;
        try {
            if (!j.contains("id") || !j["id"].is_number_integer())
                throw std::runtime_error("missing 'id'");
            id = j["id"].get<int>();
            if (j.contains("name") && j["name"].is_string())
                name = j["name"].get<std::string>();
        } catch (const std::exception& e) {
            LOG_ERROR("GatewayManager::loadFromRoot: bad cfg in {}: {}",
                      cfg_path.string(), e.what());
            continue;
        }
        GatewayCfg gcfg;
        try {
            gcfg = parseGatewayCfg(j);
        } catch (const std::exception& e) {
            LOG_ERROR("GatewayManager::loadFromRoot: parse cfg in {}: {}",
                      cfg_path.string(), e.what());
            continue;
        }
        if (auto r = normaliseOutputIds(gcfg); r != Result::Ok) {
            LOG_ERROR("GatewayManager::loadFromRoot: normalise output ids "
                      "in {} failed: {}", cfg_path.string(),
                      gatewayManagerResultName(r));
            continue;
        }

        std::unique_lock lk(mu_);
        if (gateways_.count(id)) {
            LOG_ERROR("GatewayManager::loadFromRoot: duplicate id={} ({}), skipping",
                      id, cfg_path.string());
            continue;
        }
        std::unique_ptr<IGateway> gw;
        try {
            gw = makeGateway(id, name, gcfg);
        } catch (const std::exception& e) {
            LOG_ERROR("GatewayManager::loadFromRoot: makeGateway in {} failed: {}",
                      cfg_path.string(), e.what());
            continue;
        }
        gateways_.emplace(id, std::move(gw));
        ++loaded;
    }

    LOG_INFO("GatewayManager: loaded {} gateway(s) from {}",
             loaded, gateway_root_.string());
    return loaded;
}

void GatewayManager::stopAll() {
    std::vector<IGateway*> snapshot;
    {
        std::shared_lock lk(mu_);
        for (const auto& [_, gw] : gateways_) snapshot.push_back(gw.get());
    }
    for (auto* gw : snapshot) gw->stop();
}

// ─── Event publishing (fix33 D1) ─────────────────────────────────────────────

void GatewayManager::publishStateChange(int id, const std::string& name,
                                        const char* state) const {
    if (!events_) return;
    events_->publish(events::EventType::GatewayStateChange,
                     /*channel_id=*/-1,
                     {{"id", id}, {"name", name}, {"state", state}});
}

} // namespace liveqx::gateway
