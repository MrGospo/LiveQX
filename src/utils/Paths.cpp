// fix35 (ROADMAP_1 A1) — Paths resolver implementation.
//
// Resolution semantics per primary root, in order of precedence:
//   1. CLI override (Args field non-empty)
//   2. Environment variable
//   3. Config JSON field
//   4. FHS default if /var/lib/liveqx exists, else CWD-relative
//
// Defaults that derive off state_dir (channels_dir, gateways_dir,
// plugins_allow_list_dir) chain through the same hierarchy: explicit
// override wins, otherwise we fall back to a sensible derivation.

#include "utils/Paths.h"

#include <cstdlib>
#include <sstream>

#include <nlohmann/json.hpp>

namespace liveqx::paths {
namespace {

using json = nlohmann::json;

// Returns cfg[key] if it's a non-empty string; std::nullopt otherwise.
std::optional<std::string> cfgString(const json& cfg, const char* key) {
    if (!cfg.is_object()) return std::nullopt;
    auto it = cfg.find(key);
    if (it == cfg.end() || !it->is_string()) return std::nullopt;
    auto s = it->get<std::string>();
    if (s.empty()) return std::nullopt;
    return s;
}

// CLI > ENV > cfg > default. Each step is optional; first non-empty wins.
std::filesystem::path resolveOne(
    const std::optional<std::string>&             cli_override,
    const char*                                   env_var,
    const json&                                   cfg,
    const char*                                   cfg_key,
    const std::filesystem::path&                  default_value,
    const Paths::Env&                             env) {
    if (cli_override.has_value() && !cli_override->empty())
        return *cli_override;
    if (env_var) {
        auto e = env.get_env(env_var);
        if (e.has_value() && !e->empty()) return *e;
    }
    if (cfg_key) {
        auto c = cfgString(cfg, cfg_key);
        if (c.has_value()) return *c;
    }
    return default_value;
}

// Default resolution: <fhs_root>/<sub> if FHS root exists, else ./<sub>.
std::filesystem::path defaultUnderRoot(bool fhs_detected, const char* sub) {
    if (fhs_detected) return std::filesystem::path(kFhsRoot) / sub;
    return std::filesystem::path(sub);
}

}  // namespace

Paths::Env Paths::defaultEnv() {
    Env e;
    e.get_env = [](const char* name) -> std::optional<std::string> {
        const char* v = std::getenv(name);
        if (!v || *v == '\0') return std::nullopt;
        return std::string(v);
    };
    e.exists = [](const std::filesystem::path& p) {
        std::error_code ec;
        return std::filesystem::exists(p, ec);
    };
    return e;
}

Paths Paths::resolve(const Args&           cli,
                     const json&           cfg,
                     const Env&            env) {
    Paths p;
    p.fhs_root_detected_ = env.exists(std::filesystem::path(kFhsRoot));

    // ── primary roots ───────────────────────────────────────────────
    p.state_dir_ = resolveOne(
        cli.state_dir, "LIVEQX_STATE_DIR",
        cfg, "state_dir",
        defaultUnderRoot(p.fhs_root_detected_, "state"),
        env);

    p.log_dir_ = resolveOne(
        cli.log_dir, "LIVEQX_LOG_DIR",
        cfg, "log_dir",
        defaultUnderRoot(p.fhs_root_detected_, "logs"),
        env);

    p.plugin_dir_ = resolveOne(
        cli.plugin_dir, "LIVEQX_PLUGIN_DIR",
        cfg, "plugin_dir",
        defaultUnderRoot(p.fhs_root_detected_, "plugins"),
        env);

    // UI dir is special: empty default = "no UI deployed, headless mode".
    // The FHS install puts UI under /usr/share/liveqx, not
    // /var/lib/liveqx, because it's read-only application data,
    // not operator state. Keep it empty unless explicitly configured.
    p.ui_dir_ = resolveOne(
        cli.ui_dir, "LIVEQX_UI_DIR",
        cfg, "ui_dir",
        std::filesystem::path{},
        env);

    // channels/gateways: previous default lived at CWD-relative
    // "channels"/"gateways". With a state_dir-aware install they belong
    // under state_dir/<sub>. We resolve through the standard chain so
    // operators with an existing config/channel_root_dir keep working.
    p.channels_dir_ = resolveOne(
        cli.channels_dir, "LIVEQX_CHANNELS_DIR",
        cfg, "channels_dir",
        // Backward-compat: pre-fix35 config used "channel_root_dir".
        cfgString(cfg, "channel_root_dir").value_or(
            (p.state_dir_ / "channels").string()),
        env);

    p.gateways_dir_ = resolveOne(
        cli.gateways_dir, "LIVEQX_GATEWAYS_DIR",
        cfg, "gateways_dir",
        cfgString(cfg, "gateway_root_dir").value_or(
            (p.state_dir_ / "gateways").string()),
        env);

    // plugins_allow_list_dir lives in the source tree, not under
    // plugin_dir/. ENV/CLI/config can move it; default is unchanged.
    p.plugins_allow_list_dir_ = resolveOne(
        std::nullopt, "LIVEQX_PLUGINS_ALLOW_LIST_DIR",
        cfg, "plugins_allow_list_dir",
        std::filesystem::path("docs/plugin_hashes"),
        env);

    // Optional file-level overrides.
    if (cli.auth_db.has_value() && !cli.auth_db->empty()) {
        p.auth_db_override_ = *cli.auth_db;
    } else if (auto c = cfgString(cfg, "auth_db_path"); c.has_value()) {
        p.auth_db_override_ = *c;
    } else if (auto e = env.get_env("LIVEQX_AUTH_DB"); e.has_value()) {
        p.auth_db_override_ = *e;
    }

    if (cli.master_key.has_value() && !cli.master_key->empty()) {
        p.master_key_override_ = *cli.master_key;
    } else if (auto c = cfgString(cfg, "master_key_path"); c.has_value()) {
        p.master_key_override_ = *c;
    } else if (auto e = env.get_env("LIVEQX_MASTER_KEY_FILE"); e.has_value()) {
        p.master_key_override_ = *e;
    }

    return p;
}

Paths Paths::resolve(const Args& cli, const json& cfg) {
    return resolve(cli, cfg, defaultEnv());
}

std::filesystem::path Paths::authDb() const {
    return auth_db_override_.empty()
               ? state_dir_ / "auth.db"
               : auth_db_override_;
}

std::filesystem::path Paths::masterKey() const {
    return master_key_override_.empty()
               ? state_dir_ / "master.key"
               : master_key_override_;
}

std::string Paths::summary() const {
    std::ostringstream os;
    os << "state_dir="    << state_dir_.string()
       << " log_dir="     << log_dir_.string()
       << " plugin_dir="  << plugin_dir_.string()
       << " channels_dir="<< channels_dir_.string()
       << " gateways_dir="<< gateways_dir_.string()
       << " ui_dir="      << (ui_dir_.empty() ? "<headless>" : ui_dir_.string())
       << " auth_db="     << authDb().string()
       << " master_key="  << masterKey().string()
       << " fhs_root="    << (fhs_root_detected_ ? "detected" : "absent");
    return os.str();
}

}  // namespace liveqx::paths
