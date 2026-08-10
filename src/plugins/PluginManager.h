#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "plugins/IPlugin.h"

namespace spdlog { class logger; }

namespace liveqx::plugins {

// Per-plugin record kept in the manager registry. The handle is a void*
// that stores the dlopen() result; PluginManager calls dlclose() on it
// at shutdown (best-effort — see uninstall()).
struct PluginRecord {
    std::string                name;
    std::string                version;
    std::string                sha256;          // hex, lowercase
    std::int64_t               installed_at{0}; // unix seconds
    std::int64_t               installer_user_id{-1};
    bool                       forced{false};   // installed despite missing/bad allow-list
    bool                       eula_accepted{false};
    bool                       pending_unload{false};

    PluginCapabilities         capabilities;
    std::string                attribution;     // copied out of plugin

    void*                      dl_handle{nullptr};
    std::unique_ptr<IPlugin>   instance;        // null while pending_unload
};

// Outcome codes for install(). Each result corresponds to a single
// REST status code in ControlApi.
enum class InstallStatus {
    Ok,               // 200/201 — plugin loaded
    AlreadyInstalled, // 409 — name collision
    InvalidUpload,    // 400 — non-ELF, wrong arch, oversize blob
    NotInAllowList,   // 412 — sha not allow-listed and force not set
    BadHash,          // 412 — sha mismatch with allow-list and force not set
    BadAbi,           // 422 — abi_version mismatch / missing symbol
    InitFailed,       // 422 — plugin_init returned null or threw
    IoError,          // 500 — couldn't write to disk
};

// Outcome codes for uninstall().
enum class UninstallStatus {
    Ok,               // marked pending_unload, will drop on shutdown
    NotInstalled,
    IoError,
};

// Options forwarded by ControlApi from URL query parameters.
struct InstallOptions {
    bool force{false};
    bool i_understand{false};
    std::int64_t installer_user_id{-1};
};

// PluginManager is the single registry of installed plugins for the
// running process. It is created in main.cpp once, after AuthService
// (so audit + RBAC are usable) and before ChannelManager.loadFromRoot()
// (so plugin-provided drivers are visible when channels start).
//
// Thread-safety: install/uninstall happen under mutex; lookup APIs are
// snapshot-consistent (callers receive copies of the relevant fields).
class PluginManager {
public:
    // root_dir = absolute path of "<cfg>/plugins". Created on demand.
    PluginManager(std::filesystem::path root_dir,
                  std::shared_ptr<spdlog::logger> log);
    ~PluginManager();

    PluginManager(const PluginManager&) = delete;
    PluginManager& operator=(const PluginManager&) = delete;

    // Read every "<root>/<name>/manifest.json" + matching ".so" and
    // dlopen it. Returns the count of successfully-loaded plugins.
    // Failures are logged at warn level and do not abort startup —
    // missing or broken plugins surface in /api/plugins as gone.
    std::size_t scanAndLoad();

    // Install pipeline (commit 4 of fix19 fills the body of the
    // validator; the manager declares the entry-point now so REST
    // wiring in c3 has a target):
    //   1. validate blob (ELF, size, sha256 vs allow-list)
    //   2. write to <root>/<name>/<name>.so atomically
    //   3. dlopen + abi check + init()
    //   4. record manifest.json
    //   5. invoke `audit_cb_` if set (commit 5 wires this to AuthService)
    InstallStatus install(std::string_view name,
                          const std::vector<std::uint8_t>& blob,
                          const InstallOptions& opts,
                          std::string* out_sha256 = nullptr);

    // Mark plugin pending_unload, drop instance pointer. Real dlclose
    // happens in destructor of PluginManager (process exit). See
    // fix19 design doc for the libndi caveat.
    UninstallStatus uninstall(std::string_view name);

    // Mark EULA accepted. Persists into manifest.json. Returns false
    // if plugin not installed.
    bool acceptEula(std::string_view name);

    // Snapshot accessors for REST/listing.
    struct Listing {
        std::string  name;
        std::string  version;
        std::string  sha256;
        std::int64_t installed_at{0};
        bool         forced{false};
        bool         eula_accepted{false};
        bool         pending_unload{false};
        std::vector<std::string> output_drivers;
        std::vector<std::string> input_drivers;
    };

    std::vector<Listing>  list() const;
    std::optional<Listing> get(std::string_view name) const;
    std::vector<std::string> attributions() const;

    // Capability lookup used by OutputManager / LiveInputFactory.
    // Returns nullptr if no plugin advertises this driver name; the
    // factory then falls through to its built-in dispatch.
    bool hasOutputDriver(std::string_view name) const;
    bool hasInputDriver(std::string_view name)  const;

    // Audit hook — set by main.cpp once AuthService is constructed.
    // Called inside the install/uninstall mutex with details_json
    // already serialized. Implementation in commit 5.
    using AuditCallback = std::function<void(std::string_view event,
                                             std::string_view name,
                                             std::int64_t user_id,
                                             std::string_view details_json)>;
    void setAuditCallback(AuditCallback cb);

    // Where install() looks for SHA256 allow-lists. Each plugin name maps
    // to "<dir>/<name>.json" with body { "sha256": "<hex>" } or
    // { "sha256": ["<hex>", ...] }. Empty path (default) disables the
    // allow-list — every install must use force+i_understand.
    // Set once in main.cpp from the global config.
    void setAllowListDir(std::filesystem::path dir);

    // Maximum accepted upload size. Hard-coded at 32 MiB; configurable
    // setter is overkill until a real plugin needs more.
    static constexpr std::size_t kMaxPluginBytes = 32u * 1024u * 1024u;

private:
    std::filesystem::path                            root_;
    std::filesystem::path                            allow_list_dir_;
    std::shared_ptr<spdlog::logger>                  log_;
    mutable std::mutex                               mu_;
    std::unordered_map<std::string, PluginRecord>    records_;
    AuditCallback                                    audit_cb_;
};

}  // namespace liveqx::plugins
