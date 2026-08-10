#include "utils/LegacyMigration.h"

#include <fcntl.h>
#include <unistd.h>

#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>

#include <nlohmann/json.hpp>

#include "utils/Log.h"
#include "utils/PathUtils.h"

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

// tmp + fsync + rename + dir-fsync. Same pattern as
// ChannelInstance::persistConfig but kept local to avoid coupling
// utils/ to core/.
void atomicWriteJson(const fs::path& path, const json& body) {
    const fs::path tmp = path.string() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) throw std::runtime_error("cannot open " + tmp.string());
        f << body.dump(2);
        f.flush();
        if (!f) throw std::runtime_error("write failed: " + tmp.string());
    }
    {
        const int fd = ::open(tmp.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd >= 0) { ::fsync(fd); ::close(fd); }
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::copy_file(tmp, path, fs::copy_options::overwrite_existing, ec);
        fs::remove(tmp);
        if (ec) throw std::runtime_error("rename failed: " + ec.message());
    }
    const auto parent = path.parent_path().empty() ? fs::path(".")
                                                    : path.parent_path();
    const int dfd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd >= 0) { ::fsync(dfd); ::close(dfd); }
}

bool channelsRootHasFolders(const fs::path& root) {
    if (!fs::is_directory(root)) return false;
    for (const auto& e : fs::directory_iterator(root))
        if (e.is_directory()) return true;
    return false;
}

std::string channelFolderName(int id, const std::string& name) {
    const std::string sanitized = PathUtils::sanitizeForPath(name);
    return sanitized.empty() ? ("ch" + std::to_string(id))
                              : ("ch" + std::to_string(id) + "-" + sanitized);
}

// Best-effort transplant: rename src into dst_dir, falling back to copy
// when the source is on a different filesystem (rename(2) → EXDEV).
// Missing src is silently OK — old deployments may not have produced
// any logs yet. Returns true if anything was moved.
bool moveIntoDir(const fs::path& src, const fs::path& dst_dir) {
    std::error_code ec;
    if (!fs::exists(src, ec)) return false;
    fs::create_directories(dst_dir, ec);
    const auto dst = dst_dir / src.filename();
    fs::rename(src, dst, ec);
    if (!ec) return true;
    // Cross-FS: copy then unlink.
    if (fs::is_directory(src)) {
        fs::copy(src, dst, fs::copy_options::recursive, ec);
    } else {
        fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    }
    if (ec) {
        LOG_WARN("LegacyMigration: cannot move {} → {}: {}",
                 src.string(), dst.string(), ec.message());
        return false;
    }
    fs::remove_all(src, ec);
    return true;
}

// Migrate the per-channel artefacts (config.json + logs + cache) for a
// single legacy entry. Returns true on success. On any error the
// already-created channel dir is removed so the migration as a whole
// can roll back this entry without polluting channels_root.
bool migrateOne(const json& item, const fs::path& channels_root) {
    if (!item.contains("id")) {
        LOG_ERROR("LegacyMigration: entry missing 'id' — skipping");
        return false;
    }
    const int id = item.value("id", 0);
    const std::string name = item.value("name", std::string{});
    const fs::path dir = channels_root / channelFolderName(id, name);

    std::error_code ec;
    fs::create_directories(dir / "logs",  ec);
    fs::create_directories(dir / "cache", ec);
    if (ec) {
        LOG_ERROR("LegacyMigration: mkdir {} failed: {}",
                  dir.string(), ec.message());
        return false;
    }
    try {
        atomicWriteJson(dir / "config.json", item);
    } catch (const std::exception& e) {
        LOG_ERROR("LegacyMigration: cannot write config for channel {}: {}",
                  id, e.what());
        fs::remove_all(dir, ec);
        return false;
    }

    // Logs — patterns: logs/ch{id}-{name}.log{,.1,.2,...}
    const std::string lg_base = channelFolderName(id, name) + ".log";
    const fs::path legacy_logs("logs");
    if (fs::is_directory(legacy_logs, ec)) {
        for (const auto& e : fs::directory_iterator(legacy_logs)) {
            const auto fn = e.path().filename().string();
            if (fn.rfind(lg_base, 0) == 0) {
                moveIntoDir(e.path(), dir / "logs");
            }
        }
    }

    // Cache — only when item has share-mode content_source. cache_path
    // (if set) wins over the auto-resolved cache/<leaf>.
    if (item.contains("content_source")) {
        const auto& cs = item["content_source"];
        std::string legacy_cache = cs.value("cache_path", std::string{});
        if (legacy_cache.empty() && !cs.value("share_path", std::string{}).empty()) {
            std::string leaf = PathUtils::sanitizeForPath(name);
            if (leaf.empty()) leaf = "ch_" + std::to_string(id);
            legacy_cache = "cache/" + leaf;
        }
        if (!legacy_cache.empty() && fs::is_directory(legacy_cache, ec)) {
            for (const auto& e : fs::directory_iterator(legacy_cache)) {
                moveIntoDir(e.path(), dir / "cache");
            }
            fs::remove(legacy_cache, ec);   // empty dir, ignore failure
        }
    }
    LOG_INFO("LegacyMigration: migrated channel {} → {}", id, dir.string());
    return true;
}

}  // namespace

std::size_t LegacyMigration::run(const fs::path& main_cfg_path,
                                  const fs::path& channels_root) {
    json cfg;
    {
        std::ifstream f(main_cfg_path);
        if (!f) {
            // No main config — nothing to migrate. Not an error.
            return 0;
        }
        try {
            cfg = json::parse(f);
        } catch (const std::exception& e) {
            throw std::runtime_error(std::string("LegacyMigration: cannot parse ")
                                     + main_cfg_path.string() + ": " + e.what());
        }
    }

    if (!cfg.contains("channels") || !cfg["channels"].is_array()
        || cfg["channels"].empty()) {
        return 0;
    }
    if (channelsRootHasFolders(channels_root)) {
        LOG_INFO("LegacyMigration: {} already populated — skipping",
                 channels_root.string());
        return 0;
    }

    std::error_code ec;
    fs::create_directories(channels_root, ec);
    if (ec) {
        LOG_ERROR("LegacyMigration: cannot create {}: {}",
                  channels_root.string(), ec.message());
        return 0;
    }

    std::size_t ok = 0;
    for (const auto& item : cfg["channels"]) {
        if (migrateOne(item, channels_root)) ++ok;
    }

    // Erase the legacy array only when EVERY entry migrated. Partial
    // success leaves both representations in place so the operator can
    // see the discrepancy and re-run after fixing the failure cause.
    if (ok == cfg["channels"].size()) {
        cfg.erase("channels");
        try {
            atomicWriteJson(main_cfg_path, cfg);
        } catch (const std::exception& e) {
            LOG_ERROR("LegacyMigration: migrated {} channels but main config "
                      "rewrite failed: {} — folders kept; remove 'channels' "
                      "manually to silence this on next restart",
                      ok, e.what());
            return ok;
        }
        LOG_INFO("LegacyMigration: migrated {} channel(s); legacy array removed",
                 ok);
    } else {
        LOG_WARN("LegacyMigration: migrated {} of {} channel(s); main config "
                 "left intact for retry",
                 ok, cfg["channels"].size());
    }
    return ok;
}
