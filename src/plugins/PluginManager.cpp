#include "plugins/PluginManager.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <utility>

#include <dlfcn.h>
#include <sodium.h>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "utils/PathUtils.h"

namespace liveqx::plugins {

namespace fs = std::filesystem;

namespace {

bool isValidName(std::string_view s) {
    if (s.empty() || s.size() > 64) return false;
    for (char c : s) {
        const bool ok = (c >= 'a' && c <= 'z')
                     || (c >= 'A' && c <= 'Z')
                     || (c >= '0' && c <= '9')
                     || c == '-' || c == '_';
        if (!ok) return false;
    }
    return true;
}

std::int64_t nowSec() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

// Only ET_DYN ELF64 LSB v1 with the host's e_machine is allowed in. We
// reject statically-linked executables and 32-bit/wrong-arch loads early
// so we never even get to dlopen() with mismatched binaries.
bool isAcceptableElf(const std::vector<std::uint8_t>& blob) {
    if (blob.size() < 64) return false;
    if (!(blob[0] == 0x7F && blob[1] == 'E' && blob[2] == 'L' && blob[3] == 'F'))
        return false;
    if (blob[4] != 2)  return false;   // EI_CLASS = ELFCLASS64
    if (blob[5] != 1)  return false;   // EI_DATA  = ELFDATA2LSB
    if (blob[6] != 1)  return false;   // EI_VERSION
    const std::uint16_t e_type    =
        static_cast<std::uint16_t>(blob[16]) |
        static_cast<std::uint16_t>(blob[17] << 8);
    if (e_type != 3) return false;     // ET_DYN — shared object
    const std::uint16_t e_machine =
        static_cast<std::uint16_t>(blob[18]) |
        static_cast<std::uint16_t>(blob[19] << 8);
#if defined(__x86_64__)
    if (e_machine != 0x3E) return false;     // EM_X86_64
#elif defined(__aarch64__)
    if (e_machine != 0xB7) return false;     // EM_AARCH64
#endif
    return true;
}

std::string sha256Hex(const std::vector<std::uint8_t>& blob) {
    unsigned char digest[crypto_hash_sha256_BYTES];
    crypto_hash_sha256(digest, blob.data(), blob.size());
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.resize(crypto_hash_sha256_BYTES * 2);
    for (std::size_t i = 0; i < crypto_hash_sha256_BYTES; ++i) {
        out[i * 2]     = kHex[(digest[i] >> 4) & 0xF];
        out[i * 2 + 1] = kHex[digest[i]        & 0xF];
    }
    return out;
}

enum class AllowList { Ok, NotInList, BadHash };

// Look up "<allow_dir>/<name>.json" and accept either { "sha256": "<hex>" }
// or { "sha256": ["<hex>", ...] }. An empty allow_dir or missing/invalid
// file is treated as NotInList (operator must use force).
AllowList checkAllowList(const std::filesystem::path& allow_dir,
                         std::string_view name,
                         const std::string& sha) {
    if (allow_dir.empty()) return AllowList::NotInList;
    std::error_code ec;
    const auto p = allow_dir / (std::string(name) + ".json");
    if (!std::filesystem::exists(p, ec)) return AllowList::NotInList;

    std::ifstream f(p);
    nlohmann::json j;
    try { f >> j; } catch (...) { return AllowList::NotInList; }
    if (!j.contains("sha256")) return AllowList::NotInList;

    if (j["sha256"].is_string()) {
        return j["sha256"].get<std::string>() == sha
                 ? AllowList::Ok : AllowList::BadHash;
    }
    if (j["sha256"].is_array()) {
        for (const auto& v : j["sha256"]) {
            if (v.is_string() && v.get<std::string>() == sha)
                return AllowList::Ok;
        }
        return AllowList::BadHash;
    }
    return AllowList::NotInList;
}

// Try every documented entry-point on a freshly-dlopen'd handle. Returns
// nullptr-IPlugin and rejects if anything is missing or returns
// inconsistent values. Detailed reason logged at warn level.
std::unique_ptr<IPlugin> bindAndInit(void* handle,
                                     const PluginContext& ctx,
                                     std::string& out_attribution,
                                     spdlog::logger* log,
                                     std::string& reject_reason) {
    using AbiFn = std::uint32_t (*)();
    using InitFn = IPlugin* (*)(const PluginContext*);
    using AttrFn = const char* (*)();

    auto abi_fn = reinterpret_cast<AbiFn>(::dlsym(handle, kSymAbiVersion));
    if (!abi_fn) {
        reject_reason = std::string("missing symbol: ") + kSymAbiVersion;
        return nullptr;
    }
    const std::uint32_t got = abi_fn();
    if (got != kPluginAbiVersion) {
        reject_reason = "abi mismatch: plugin=" + std::to_string(got)
                      + " host=" + std::to_string(kPluginAbiVersion);
        return nullptr;
    }

    auto init_fn = reinterpret_cast<InitFn>(::dlsym(handle, kSymInit));
    if (!init_fn) {
        reject_reason = std::string("missing symbol: ") + kSymInit;
        return nullptr;
    }
    IPlugin* raw = init_fn(&ctx);
    if (!raw) {
        reject_reason = "plugin_init returned nullptr";
        return nullptr;
    }

    if (auto attr_fn = reinterpret_cast<AttrFn>(::dlsym(handle, kSymAttribution))) {
        if (const char* a = attr_fn()) out_attribution = a;
    }
    if (out_attribution.empty()) {
        if (const char* a = raw->attributionText()) out_attribution = a;
    }

    if (log) {
        log->info("plugin '{}' v{} loaded (abi={})",
                  raw->name() ? raw->name() : "?",
                  raw->version() ? raw->version() : "?",
                  got);
    }
    return std::unique_ptr<IPlugin>(raw);
}

nlohmann::json manifestToJson(const PluginRecord& r) {
    return nlohmann::json{
        {"name",              r.name},
        {"version",           r.version},
        {"sha256",            r.sha256},
        {"installed_at",      r.installed_at},
        {"installer_user_id", r.installer_user_id},
        {"forced",            r.forced},
        {"eula_accepted",     r.eula_accepted},
    };
}

}  // namespace

PluginManager::PluginManager(fs::path root_dir,
                             std::shared_ptr<spdlog::logger> log)
    : root_(std::move(root_dir)),
      log_(std::move(log)) {
    if (!log_) log_ = spdlog::default_logger();
    std::error_code ec;
    fs::create_directories(root_, ec);
    if (ec) {
        log_->warn("PluginManager: cannot create {}: {}",
                   root_.string(), ec.message());
    }
}

PluginManager::~PluginManager() {
    std::lock_guard lk(mu_);
    for (auto& [name, rec] : records_) {
        if (rec.instance) {
            try { rec.instance->onShutdown(); }
            catch (const std::exception& e) {
                log_->warn("plugin '{}' onShutdown threw: {}", name, e.what());
            }
            rec.instance.reset();
        }
        if (rec.dl_handle) {
            ::dlclose(rec.dl_handle);
            rec.dl_handle = nullptr;
        }
    }
}

std::size_t PluginManager::scanAndLoad() {
    std::lock_guard lk(mu_);
    std::error_code ec;
    if (!fs::exists(root_, ec)) return 0;

    std::size_t loaded = 0;
    for (const auto& entry : fs::directory_iterator(root_, ec)) {
        if (!entry.is_directory()) continue;
        const std::string name = entry.path().filename().string();
        if (!isValidName(name)) {
            log_->warn("plugins: skipping unexpected dir '{}'", name);
            continue;
        }
        const fs::path manifest_path = entry.path() / "manifest.json";
        const fs::path so_path       = entry.path() / (name + ".so");
        if (!fs::exists(manifest_path) || !fs::exists(so_path)) {
            log_->warn("plugins: '{}' incomplete (missing manifest or .so)", name);
            continue;
        }

        std::ifstream f(manifest_path);
        nlohmann::json j;
        try { f >> j; }
        catch (const std::exception& e) {
            log_->warn("plugins: '{}' bad manifest: {}", name, e.what());
            continue;
        }

        PluginRecord rec;
        rec.name              = j.value("name", name);
        rec.version           = j.value("version", "unknown");
        rec.sha256            = j.value("sha256", "");
        rec.installed_at      = j.value("installed_at", static_cast<std::int64_t>(0));
        rec.installer_user_id = j.value("installer_user_id", static_cast<std::int64_t>(-1));
        rec.forced            = j.value("forced", false);
        rec.eula_accepted     = j.value("eula_accepted", false);

        rec.dl_handle = ::dlopen(so_path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!rec.dl_handle) {
            log_->warn("plugins: dlopen('{}') failed: {}",
                       so_path.string(), ::dlerror() ? ::dlerror() : "?");
            continue;
        }

        PluginContext ctx;
        ctx.logger     = log_.get();
        std::string dir_holder = entry.path().string();
        ctx.plugin_dir = dir_holder.c_str();

        std::string reason;
        auto inst = bindAndInit(rec.dl_handle, ctx, rec.attribution, log_.get(), reason);
        if (!inst) {
            log_->warn("plugins: '{}' init failed: {}", name, reason);
            ::dlclose(rec.dl_handle);
            continue;
        }
        rec.capabilities = inst->capabilities();
        rec.instance     = std::move(inst);

        records_[rec.name] = std::move(rec);
        ++loaded;
    }
    log_->info("plugins: loaded {} plugin(s) from {}", loaded, root_.string());
    return loaded;
}

InstallStatus PluginManager::install(std::string_view name,
                                     const std::vector<std::uint8_t>& blob,
                                     const InstallOptions& opts,
                                     std::string* out_sha256) {
    if (out_sha256) out_sha256->clear();
    if (!isValidName(name))                   return InstallStatus::InvalidUpload;
    if (blob.empty() || blob.size() > kMaxPluginBytes)
        return InstallStatus::InvalidUpload;
    if (!isAcceptableElf(blob))               return InstallStatus::InvalidUpload;

    const std::string sha = sha256Hex(blob);
    if (out_sha256) *out_sha256 = sha;

    const auto al = checkAllowList(allow_list_dir_, name, sha);
    const bool effective_force = opts.force && opts.i_understand;
    if (al == AllowList::NotInList && !effective_force) return InstallStatus::NotInAllowList;
    if (al == AllowList::BadHash   && !effective_force) return InstallStatus::BadHash;

    std::lock_guard lk(mu_);
    const std::string sname(name);
    if (records_.find(sname) != records_.end()) return InstallStatus::AlreadyInstalled;

    std::error_code ec;
    const fs::path dir = root_ / sname;
    fs::create_directories(dir, ec);
    if (ec) {
        log_->warn("plugin '{}' mkdir({}) failed: {}",
                   sname, dir.string(), ec.message());
        return InstallStatus::IoError;
    }
    const fs::path so = dir / (sname + ".so");

    // 6. Atomic .so write. atomicWriteFile takes a std::string payload —
    // safe for binary content (no NUL handling issues; ofstream<<string
    // writes the underlying bytes).
    try {
        std::string payload(reinterpret_cast<const char*>(blob.data()), blob.size());
        PathUtils::atomicWriteFile(so, payload);
    } catch (const std::exception& e) {
        log_->warn("plugin '{}' write({}) failed: {}",
                   sname, so.string(), e.what());
        fs::remove(dir, ec);
        return InstallStatus::IoError;
    }

    // 7. dlopen. Failure here typically means missing transitive .so
    // (e.g. libndi). We surface that as InitFailed — operator can fix the
    // host environment and reinstall.
    void* handle = ::dlopen(so.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        const char* dlerr = ::dlerror();
        log_->warn("plugin '{}' dlopen failed: {}",
                   sname, dlerr ? dlerr : "?");
        fs::remove(so, ec);
        fs::remove(dir, ec);
        return InstallStatus::InitFailed;
    }

    // 8-9. ABI probe + plugin_init.
    PluginContext ctx;
    ctx.logger = log_.get();
    const std::string dir_holder = dir.string();
    ctx.plugin_dir = dir_holder.c_str();

    PluginRecord rec;
    rec.dl_handle = handle;
    std::string reject_reason;
    auto inst = bindAndInit(handle, ctx, rec.attribution, log_.get(), reject_reason);
    if (!inst) {
        log_->warn("plugin '{}' init rejected: {}", sname, reject_reason);
        ::dlclose(handle);
        fs::remove(so, ec);
        fs::remove(dir, ec);
        const bool is_abi =
            reject_reason.find("abi") != std::string::npos ||
            reject_reason.find(kSymAbiVersion) != std::string::npos;
        return is_abi ? InstallStatus::BadAbi : InstallStatus::InitFailed;
    }

    rec.name              = sname;
    rec.version           = inst->version() ? inst->version() : "";
    rec.sha256            = sha;
    rec.installed_at      = nowSec();
    rec.installer_user_id = opts.installer_user_id;
    rec.forced            = (al != AllowList::Ok);
    rec.eula_accepted     = false;
    rec.capabilities      = inst->capabilities();
    rec.instance          = std::move(inst);

    // 10. Manifest. Failure here unwinds — we don't want a half-installed
    // plugin where the .so is loaded but the manifest is missing.
    try {
        PathUtils::atomicWriteFile(dir / "manifest.json",
                                   manifestToJson(rec).dump(2));
    } catch (const std::exception& e) {
        log_->warn("plugin '{}' manifest write failed: {}", sname, e.what());
        try { rec.instance->onShutdown(); } catch (...) {}
        rec.instance.reset();
        ::dlclose(rec.dl_handle);
        fs::remove(so, ec);
        fs::remove(dir, ec);
        return InstallStatus::IoError;
    }

    if (audit_cb_) {
        const nlohmann::json details = {
            {"sha256",  sha},
            {"forced",  rec.forced},
            {"version", rec.version},
        };
        audit_cb_("plugin.install", rec.name,
                  opts.installer_user_id, details.dump());
    }

    log_->info("plugin '{}' installed (version={}, forced={})",
               rec.name, rec.version, rec.forced);

    records_[sname] = std::move(rec);
    return InstallStatus::Ok;
}

void PluginManager::setAllowListDir(std::filesystem::path dir) {
    std::lock_guard lk(mu_);
    allow_list_dir_ = std::move(dir);
}

UninstallStatus PluginManager::uninstall(std::string_view name) {
    std::lock_guard lk(mu_);
    auto it = records_.find(std::string(name));
    if (it == records_.end()) return UninstallStatus::NotInstalled;

    auto& rec = it->second;
    if (rec.instance) {
        try { rec.instance->onShutdown(); }
        catch (const std::exception& e) {
            log_->warn("plugin '{}' onShutdown threw: {}", rec.name, e.what());
        }
        rec.instance.reset();
    }
    rec.pending_unload = true;

    // Remove on-disk manifest + .so so a restart drops the plugin.
    std::error_code ec;
    fs::remove(root_ / rec.name / (rec.name + ".so"), ec);
    fs::remove(root_ / rec.name / "manifest.json",   ec);
    fs::remove(root_ / rec.name, ec);  // best-effort; non-empty dirs survive

    if (audit_cb_) {
        nlohmann::json details = {{"pending_unload", true}};
        audit_cb_("plugin.uninstall", rec.name, /*user_id=*/-1, details.dump());
    }
    return UninstallStatus::Ok;
}

bool PluginManager::acceptEula(std::string_view name) {
    std::lock_guard lk(mu_);
    auto it = records_.find(std::string(name));
    if (it == records_.end()) return false;
    it->second.eula_accepted = true;
    try {
        const auto path = root_ / it->second.name / "manifest.json";
        PathUtils::atomicWriteFile(path, manifestToJson(it->second).dump(2));
    } catch (const std::exception& e) {
        log_->warn("plugin '{}' manifest write failed: {}", it->second.name, e.what());
        return false;
    }
    return true;
}

std::vector<PluginManager::Listing> PluginManager::list() const {
    std::lock_guard lk(mu_);
    std::vector<Listing> out;
    out.reserve(records_.size());
    for (const auto& [_, r] : records_) {
        out.push_back(Listing{
            r.name, r.version, r.sha256, r.installed_at, r.forced,
            r.eula_accepted, r.pending_unload,
            r.capabilities.output_drivers, r.capabilities.input_drivers,
        });
    }
    std::sort(out.begin(), out.end(),
              [](const auto& a, const auto& b){ return a.name < b.name; });
    return out;
}

std::optional<PluginManager::Listing>
PluginManager::get(std::string_view name) const {
    std::lock_guard lk(mu_);
    auto it = records_.find(std::string(name));
    if (it == records_.end()) return std::nullopt;
    const auto& r = it->second;
    return Listing{r.name, r.version, r.sha256, r.installed_at, r.forced,
                   r.eula_accepted, r.pending_unload,
                   r.capabilities.output_drivers, r.capabilities.input_drivers};
}

std::vector<std::string> PluginManager::attributions() const {
    std::lock_guard lk(mu_);
    std::vector<std::string> out;
    for (const auto& [_, r] : records_) {
        if (!r.attribution.empty()) out.push_back(r.attribution);
    }
    return out;
}

bool PluginManager::hasOutputDriver(std::string_view name) const {
    std::lock_guard lk(mu_);
    for (const auto& [_, r] : records_) {
        for (const auto& d : r.capabilities.output_drivers) {
            if (d == name) return true;
        }
    }
    return false;
}

bool PluginManager::hasInputDriver(std::string_view name) const {
    std::lock_guard lk(mu_);
    for (const auto& [_, r] : records_) {
        for (const auto& d : r.capabilities.input_drivers) {
            if (d == name) return true;
        }
    }
    return false;
}

void PluginManager::setAuditCallback(AuditCallback cb) {
    std::lock_guard lk(mu_);
    audit_cb_ = std::move(cb);
}

}  // namespace liveqx::plugins
