// fix35 (ROADMAP_1 A1) — unit tests for the Paths resolver.
//
// Coverage: each level of the precedence chain in isolation, the FHS
// detection branch, the headless ui_dir behaviour, and the file-level
// overrides for auth_db / master_key.

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <map>
#include <optional>
#include <string>

#include "utils/Paths.h"

using nlohmann::json;
using liveqx::paths::Args;
using liveqx::paths::Paths;

namespace {

// In-memory env that pretends /var/lib/liveqx does NOT exist
// unless the test asks for it, and stubs out LIVEQX_* variables.
struct FakeEnv {
    std::map<std::string, std::string> vars;
    bool                               fhs_root_exists = false;

    Paths::Env make() const {
        Paths::Env e;
        // Capture by value to avoid dangling refs.
        auto v_copy   = vars;
        auto fhs_copy = fhs_root_exists;
        e.get_env = [v_copy](const char* name) -> std::optional<std::string> {
            auto it = v_copy.find(name);
            if (it == v_copy.end()) return std::nullopt;
            if (it->second.empty()) return std::nullopt;
            return it->second;
        };
        e.exists = [fhs_copy](const std::filesystem::path& p) {
            return p.string() == liveqx::paths::kFhsRoot ? fhs_copy
                                                                 : false;
        };
        return e;
    }
};

}  // namespace

// ── default branch ─────────────────────────────────────────────────────

TEST(PathsDefault, FhsRootMissingFallsBackToCwd) {
    FakeEnv env;  // fhs_root_exists = false
    Args args;
    auto p = Paths::resolve(args, json::object(), env.make());
    EXPECT_EQ(p.stateDir(),    std::filesystem::path("state"));
    EXPECT_EQ(p.logDir(),      std::filesystem::path("logs"));
    EXPECT_EQ(p.pluginDir(),   std::filesystem::path("plugins"));
    EXPECT_TRUE(p.uiDir().empty());  // headless by default
    EXPECT_EQ(p.channelsDir(), std::filesystem::path("state/channels"));
    EXPECT_EQ(p.gatewaysDir(), std::filesystem::path("state/gateways"));
    EXPECT_FALSE(p.fhsRootDetected());
}

TEST(PathsDefault, FhsRootPresentUsesVarLib) {
    FakeEnv env;
    env.fhs_root_exists = true;
    Args args;
    auto p = Paths::resolve(args, json::object(), env.make());
    EXPECT_EQ(p.stateDir(),
              std::filesystem::path("/var/lib/liveqx/state"));
    EXPECT_EQ(p.logDir(),
              std::filesystem::path("/var/lib/liveqx/logs"));
    EXPECT_EQ(p.pluginDir(),
              std::filesystem::path("/var/lib/liveqx/plugins"));
    EXPECT_EQ(p.channelsDir(),
              std::filesystem::path("/var/lib/liveqx/state/channels"));
    EXPECT_TRUE(p.fhsRootDetected());
}

// ── precedence chain ──────────────────────────────────────────────────

TEST(PathsPrecedence, ConfigOverridesDefault) {
    FakeEnv env;
    Args args;
    json cfg = {
        {"state_dir",  "/opt/sc/state"},
        {"log_dir",    "/opt/sc/logs"},
        {"plugin_dir", "/opt/sc/plugins"},
    };
    auto p = Paths::resolve(args, cfg, env.make());
    EXPECT_EQ(p.stateDir(),  std::filesystem::path("/opt/sc/state"));
    EXPECT_EQ(p.logDir(),    std::filesystem::path("/opt/sc/logs"));
    EXPECT_EQ(p.pluginDir(), std::filesystem::path("/opt/sc/plugins"));
    // channels/gateways still derive off state_dir.
    EXPECT_EQ(p.channelsDir(),
              std::filesystem::path("/opt/sc/state/channels"));
}

TEST(PathsPrecedence, EnvOverridesConfig) {
    FakeEnv env;
    env.vars["LIVEQX_STATE_DIR"] = "/env/state";
    Args args;
    json cfg = {{"state_dir", "/cfg/state"}};
    auto p = Paths::resolve(args, cfg, env.make());
    EXPECT_EQ(p.stateDir(), std::filesystem::path("/env/state"));
    EXPECT_EQ(p.channelsDir(),
              std::filesystem::path("/env/state/channels"));
}

TEST(PathsPrecedence, CliOverridesEnv) {
    FakeEnv env;
    env.vars["LIVEQX_STATE_DIR"] = "/env/state";
    Args args;
    args.state_dir = "/cli/state";
    json cfg = {{"state_dir", "/cfg/state"}};
    auto p = Paths::resolve(args, cfg, env.make());
    EXPECT_EQ(p.stateDir(), std::filesystem::path("/cli/state"));
    EXPECT_EQ(p.gatewaysDir(),
              std::filesystem::path("/cli/state/gateways"));
}

TEST(PathsPrecedence, EmptyCliFallsThroughToEnv) {
    FakeEnv env;
    env.vars["LIVEQX_LOG_DIR"] = "/env/logs";
    Args args;
    args.log_dir = "";  // empty CLI value should NOT win
    auto p = Paths::resolve(args, json::object(), env.make());
    EXPECT_EQ(p.logDir(), std::filesystem::path("/env/logs"));
}

// ── ui_dir ────────────────────────────────────────────────────────────

TEST(PathsUiDir, EmptyByDefault) {
    FakeEnv env;
    auto p = Paths::resolve(Args{}, json::object(), env.make());
    EXPECT_TRUE(p.uiDir().empty());
}

TEST(PathsUiDir, ConfigSetsIt) {
    FakeEnv env;
    json cfg = {{"ui_dir", "/usr/share/liveqx/ui"}};
    auto p = Paths::resolve(Args{}, cfg, env.make());
    EXPECT_EQ(p.uiDir(),
              std::filesystem::path("/usr/share/liveqx/ui"));
}

TEST(PathsUiDir, CliWinsOverEnvAndConfig) {
    FakeEnv env;
    env.vars["LIVEQX_UI_DIR"] = "/env/ui";
    json cfg = {{"ui_dir", "/cfg/ui"}};
    Args args;
    args.ui_dir = "/cli/ui";
    auto p = Paths::resolve(args, cfg, env.make());
    EXPECT_EQ(p.uiDir(), std::filesystem::path("/cli/ui"));
}

// ── derived paths ─────────────────────────────────────────────────────

TEST(PathsDerived, AuthDbAndMasterKeyDeriveOffStateDir) {
    FakeEnv env;
    json cfg = {{"state_dir", "/opt/sc/state"}};
    auto p = Paths::resolve(Args{}, cfg, env.make());
    EXPECT_EQ(p.authDb(),
              std::filesystem::path("/opt/sc/state/auth.db"));
    EXPECT_EQ(p.masterKey(),
              std::filesystem::path("/opt/sc/state/master.key"));
    EXPECT_EQ(p.stressJson(),
              std::filesystem::path("/opt/sc/state/stress.json"));
    EXPECT_EQ(p.initialAdminPasswordFile(),
              std::filesystem::path(
                  "/opt/sc/state/initial_admin_password.txt"));
    EXPECT_EQ(p.tlsDir(),
              std::filesystem::path("/opt/sc/state/tls"));
}

TEST(PathsDerived, AuthDbCliOverrideWins) {
    FakeEnv env;
    json cfg = {
        {"state_dir",     "/opt/sc/state"},
        {"auth_db_path",  "/legacy/auth.db"},
    };
    Args args;
    args.auth_db = "/cli/auth.db";
    auto p = Paths::resolve(args, cfg, env.make());
    EXPECT_EQ(p.authDb(), std::filesystem::path("/cli/auth.db"));
}

TEST(PathsDerived, AuthDbConfigOverrideWinsWhenNoCli) {
    FakeEnv env;
    json cfg = {
        {"state_dir",    "/opt/sc/state"},
        {"auth_db_path", "/legacy/auth.db"},
    };
    auto p = Paths::resolve(Args{}, cfg, env.make());
    EXPECT_EQ(p.authDb(), std::filesystem::path("/legacy/auth.db"));
}

TEST(PathsDerived, MasterKeyEnvOverrideWinsWhenNoCliOrConfig) {
    FakeEnv env;
    env.vars["LIVEQX_MASTER_KEY_FILE"] = "/env/master.key";
    json cfg = {{"state_dir", "/opt/sc/state"}};
    auto p = Paths::resolve(Args{}, cfg, env.make());
    EXPECT_EQ(p.masterKey(), std::filesystem::path("/env/master.key"));
}

// ── backward-compat config keys ───────────────────────────────────────

TEST(PathsBackwardCompat, ChannelRootDirHonoured) {
    FakeEnv env;
    json cfg = {
        {"state_dir",          "/opt/sc/state"},
        {"channel_root_dir",   "/legacy/channels"},
    };
    auto p = Paths::resolve(Args{}, cfg, env.make());
    EXPECT_EQ(p.channelsDir(),
              std::filesystem::path("/legacy/channels"));
}

TEST(PathsBackwardCompat, GatewayRootDirHonoured) {
    FakeEnv env;
    json cfg = {
        {"state_dir",        "/opt/sc/state"},
        {"gateway_root_dir", "/legacy/gateways"},
    };
    auto p = Paths::resolve(Args{}, cfg, env.make());
    EXPECT_EQ(p.gatewaysDir(),
              std::filesystem::path("/legacy/gateways"));
}

TEST(PathsBackwardCompat, NewKeyWinsOverLegacyKey) {
    FakeEnv env;
    json cfg = {
        {"state_dir",        "/opt/sc/state"},
        {"channel_root_dir", "/legacy/channels"},
        {"channels_dir",     "/new/channels"},
    };
    auto p = Paths::resolve(Args{}, cfg, env.make());
    EXPECT_EQ(p.channelsDir(),
              std::filesystem::path("/new/channels"));
}

// ── plugins_allow_list_dir ────────────────────────────────────────────

TEST(PathsAllowList, DefaultIsRepoTreePath) {
    FakeEnv env;
    auto p = Paths::resolve(Args{}, json::object(), env.make());
    EXPECT_EQ(p.pluginsAllowListDir(),
              std::filesystem::path("docs/plugin_hashes"));
}

TEST(PathsAllowList, ConfigOverrides) {
    FakeEnv env;
    json cfg = {{"plugins_allow_list_dir", "/etc/sc/allow"}};
    auto p = Paths::resolve(Args{}, cfg, env.make());
    EXPECT_EQ(p.pluginsAllowListDir(),
              std::filesystem::path("/etc/sc/allow"));
}

// ── summary string ────────────────────────────────────────────────────

TEST(PathsSummary, IncludesPrimaryRoots) {
    FakeEnv env;
    json cfg = {
        {"state_dir",  "/opt/sc/state"},
        {"log_dir",    "/opt/sc/logs"},
        {"plugin_dir", "/opt/sc/plugins"},
    };
    auto p = Paths::resolve(Args{}, cfg, env.make());
    const auto s = p.summary();
    EXPECT_NE(s.find("state_dir=/opt/sc/state"),  std::string::npos);
    EXPECT_NE(s.find("log_dir=/opt/sc/logs"),     std::string::npos);
    EXPECT_NE(s.find("plugin_dir=/opt/sc/plugins"), std::string::npos);
    EXPECT_NE(s.find("ui_dir=<headless>"),        std::string::npos);
    EXPECT_NE(s.find("auth_db=/opt/sc/state/auth.db"), std::string::npos);
}
