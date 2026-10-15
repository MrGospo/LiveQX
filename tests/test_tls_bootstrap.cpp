// Unit tests for api/TlsBootstrap — resolves the TLS settings of the control
// API port (shared by the normal start and the sealed start).

#include "api/TlsBootstrap.h"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <filesystem>

#include "utils/Paths.h"
#include "utils/Tls.h"

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

class TlsBootstrapTest : public ::testing::Test {
protected:
    fs::path root_;

    void SetUp() override {
        static std::atomic<int> seq{0};
        root_ = fs::temp_directory_path() /
                ("lqx-tlsboot-" + std::to_string(::getpid()) + "-" +
                 std::to_string(seq.fetch_add(1)) + "-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(root_ / "state");
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(root_, ec);
    }

    liveqx::paths::Paths makePaths() const {
        liveqx::paths::Args a;
        a.state_dir  = (root_ / "state").string();
        a.log_dir    = (root_ / "logs").string();
        a.plugin_dir = (root_ / "plugins").string();
        liveqx::paths::Paths::Env env;
        env.get_env = [](const char*) { return std::optional<std::string>{}; };
        env.exists  = [](const fs::path&) { return false; };
        return liveqx::paths::Paths::resolve(a, json::object(), env);
    }
};

}  // namespace

TEST_F(TlsBootstrapTest, AutoModeCreatesCaAndServerCertificate) {
    bool failed = false;
    const auto b = resolveTlsBindings(json::object(), makePaths(), "0.0.0.0", failed);
    ASSERT_FALSE(failed);
    EXPECT_EQ(b.mode, "auto");
    EXPECT_EQ(b.bind, "0.0.0.0");
    EXPECT_TRUE(fs::exists(b.cert_path));
    EXPECT_TRUE(fs::exists(b.key_path));
    EXPECT_EQ(b.tls_dir, root_ / "state" / "tls");
    EXPECT_TRUE(fs::exists(b.tls_dir / "ca.crt"));
}

TEST_F(TlsBootstrapTest, AutoModeReusesAnExistingServerCertificate) {
    bool failed = false;
    const auto first = resolveTlsBindings(json::object(), makePaths(), "0.0.0.0", failed);
    ASSERT_FALSE(failed);
    const auto fp1 = liveqx::tls::readCertInfo(first.cert_path).fingerprint_sha256;

    const auto second = resolveTlsBindings(json::object(), makePaths(), "0.0.0.0", failed);
    ASSERT_FALSE(failed);
    EXPECT_EQ(liveqx::tls::readCertInfo(second.cert_path).fingerprint_sha256, fp1)
        << "a fresh certificate must not be minted on every call";
}

TEST_F(TlsBootstrapTest, DisabledOnLoopbackIsPlainHttp) {
    bool failed = false;
    const auto b = resolveTlsBindings({{"tls", {{"mode", "disabled"}}}},
                                      makePaths(), "127.0.0.1", failed);
    EXPECT_FALSE(failed);
    EXPECT_EQ(b.mode, "disabled");
    EXPECT_TRUE(b.cert_path.empty());
    EXPECT_TRUE(b.key_path.empty());
}

TEST_F(TlsBootstrapTest, DisabledOnPublicBindFailsUnlessExplicitlyAllowed) {
    bool failed = false;
    resolveTlsBindings({{"tls", {{"mode", "disabled"}}}}, makePaths(), "0.0.0.0", failed);
    EXPECT_TRUE(failed);

    failed = false;
    resolveTlsBindings({{"tls", {{"mode", "disabled"}, {"allow_insecure_bind", true}}}},
                       makePaths(), "0.0.0.0", failed);
    EXPECT_FALSE(failed);
}

TEST_F(TlsBootstrapTest, ProvidedModeWithMissingFilesFails) {
    bool failed = false;
    resolveTlsBindings({{"tls", {{"mode", "provided"},
                                 {"cert_path", (root_ / "no.crt").string()},
                                 {"key_path",  (root_ / "no.key").string()}}}},
                       makePaths(), "0.0.0.0", failed);
    EXPECT_TRUE(failed);
}

TEST_F(TlsBootstrapTest, BehindProxyIsPlainHttp) {
    bool failed = false;
    const auto b = resolveTlsBindings({{"tls", {{"mode", "behind_proxy"}}}},
                                      makePaths(), "127.0.0.1", failed);
    EXPECT_FALSE(failed);
    EXPECT_EQ(b.mode, "behind_proxy");
    EXPECT_TRUE(b.cert_path.empty());
}

TEST_F(TlsBootstrapTest, InvalidTlsSectionFails) {
    bool failed = false;
    resolveTlsBindings({{"tls", {{"mode", "bogus"}}}}, makePaths(), "0.0.0.0", failed);
    EXPECT_TRUE(failed);
}
