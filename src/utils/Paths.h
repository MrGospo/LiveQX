#pragma once
//
// fix35 (ROADMAP_1 A1) — central path resolver.
//
// All filesystem paths used at startup go through this class. Hardcoded
// "state/auth.db" / "logs" / "plugins" / etc. were scattered across
// main.cpp and made the binary unrunnable outside the project root.
// Paths::resolve() folds CLI flags, environment variables, the loaded
// config JSON, and FHS-aware defaults into a single immutable bundle
// that downstream subsystems (AuthDb, MasterKey, ChannelManager,
// GatewayManager, Log, PluginManager, StressService) consume.
//
// Resolution order per primary root:
//   1. CLI flag        (e.g. --state-dir /opt/sc/state)
//   2. ENV variable    (LIVEQX_STATE_DIR)
//   3. Config field    ("state_dir": "...")
//   4. Default:
//        if /var/lib/liveqx exists → /var/lib/liveqx/<sub>
//        else                              → ./<sub>  (CWD-relative, dev)
//
// Tests inject a custom Env to make resolution deterministic; production
// uses defaultEnv() which calls std::getenv and std::filesystem::exists.

#include <filesystem>
#include <functional>
#include <optional>
#include <string>

#include <nlohmann/json_fwd.hpp>

namespace liveqx::paths {

// CLI overrides parsed from argv. Each field is optional — empty means
// "no CLI override given, fall through to ENV/config/default".
struct Args {
    std::optional<std::string> state_dir;
    std::optional<std::string> log_dir;
    std::optional<std::string> plugin_dir;
    std::optional<std::string> ui_dir;
    std::optional<std::string> config_dir;
    std::optional<std::string> channels_dir;
    std::optional<std::string> gateways_dir;

    // Pre-existing flags (`--auth-db`, `--master-key-file`) that allow
    // pinning these two files independently of state_dir. Recovery-CLI
    // tooling already accepts them, so the resolver honours them too.
    std::optional<std::string> auth_db;
    std::optional<std::string> master_key;
};

class Paths {
public:
    // Test seam. get_env returns std::nullopt when the variable is unset
    // (matches getenv()==nullptr semantics); exists tells us whether the
    // FHS root /var/lib/liveqx is materialised.
    struct Env {
        std::function<std::optional<std::string>(const char*)> get_env;
        std::function<bool(const std::filesystem::path&)>      exists;
    };
    static Env defaultEnv();

    // Build an immutable Paths object from CLI flags + parsed config.
    // `cfg` may be a null/empty json — fields are optional everywhere.
    static Paths resolve(const Args&             cli,
                         const nlohmann::json&   cfg,
                         const Env&              env);

    // Convenience overload using the production environment.
    static Paths resolve(const Args& cli, const nlohmann::json& cfg);

    // Primary roots — referenced by name from main.cpp.
    const std::filesystem::path& stateDir()    const noexcept { return state_dir_; }
    const std::filesystem::path& logDir()      const noexcept { return log_dir_; }
    const std::filesystem::path& pluginDir()   const noexcept { return plugin_dir_; }
    const std::filesystem::path& uiDir()       const noexcept { return ui_dir_; }
    const std::filesystem::path& channelsDir() const noexcept { return channels_dir_; }
    const std::filesystem::path& gatewaysDir() const noexcept { return gateways_dir_; }

    // Derived paths off state_dir. authDb() and masterKey() honour the
    // `--auth-db` / `--master-key-file` overrides if those were supplied.
    std::filesystem::path authDb() const;
    std::filesystem::path masterKey() const;
    std::filesystem::path stressJson() const                  { return state_dir_ / "stress.json"; }
    std::filesystem::path initialAdminPasswordFile() const    { return state_dir_ / "initial_admin_password.txt"; }
    std::filesystem::path playbackDb() const                  { return state_dir_ / "playback.db"; }
    std::filesystem::path mountsDb() const                    { return state_dir_ / "mounts.db"; }
    // Enterprise audit trail. Separate DB (not auth.db) so retention,
    // backup profile and BEGIN IMMEDIATE contention are independent from
    // the auth path — audit must never be starved by login load and
    // vice versa.
    std::filesystem::path auditDb() const                     { return state_dir_ / "audit.db"; }
    // Fallback JSONL file the AuditLogger writes to when audit.db is
    // unavailable (fsync error, disk full). Ops recovers the trail from
    // here into audit.db once the DB is back.
    std::filesystem::path auditEmergencyFile() const          { return state_dir_ / "audit-emergency.jsonl"; }

    // Per-install bearer token guarding GET /api/metrics. Lives in state_dir
    // (writable by the engine) rather than in /etc/liveqx/config.json, so
    // that revoke/regenerate via the UI does not require root and so that
    // shipped config templates never carry a real secret.
    std::filesystem::path metricsBearerTokenFile() const      { return state_dir_ / "metrics-bearer-token"; }

    // fix38 — TLS material directory. Holds ca.{crt,key} + server.{crt,key}
    // when the operator runs in `auto` mode (the default). In `provided`
    // mode the cert/key paths come from TlsConfig directly and this dir
    // may be empty / unused.
    std::filesystem::path tlsDir() const                      { return state_dir_ / "tls"; }

    // Plugin allow-list. Lives outside plugin_dir/ because it ships with
    // the source tree (docs/plugin_hashes/) and is read-only at runtime.
    const std::filesystem::path& pluginsAllowListDir() const noexcept { return plugins_allow_list_dir_; }

    // Multi-line summary for boot logs ("state_dir: ..., log_dir: ...").
    std::string summary() const;

    // The FHS root probed at resolve(). Useful for telling the operator
    // why /var/lib/liveqx was or wasn't picked.
    bool fhsRootDetected() const noexcept { return fhs_root_detected_; }

private:
    Paths() = default;

    std::filesystem::path state_dir_;
    std::filesystem::path log_dir_;
    std::filesystem::path plugin_dir_;
    std::filesystem::path ui_dir_;            // empty when not configured (headless)
    std::filesystem::path channels_dir_;
    std::filesystem::path gateways_dir_;
    std::filesystem::path plugins_allow_list_dir_;

    // Optional explicit overrides. When empty we derive from state_dir_.
    std::filesystem::path auth_db_override_;
    std::filesystem::path master_key_override_;

    bool fhs_root_detected_{false};
};

// Stable identifier used for the FHS default probe and for the install
// scripts. A single source of truth so the C-phase install.sh and the
// Docker C1 image agree on this constant.
inline constexpr const char* kFhsRoot = "/var/lib/liveqx";

}  // namespace liveqx::paths
