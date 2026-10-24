// Unit tests for api/TlsBootstrap — resolves the TLS settings of the control
// API port (shared by the normal start and the sealed start).

#include "api/TlsBootstrap.h"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>

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

TEST_F(TlsBootstrapTest, BehindProxyPassesTheTrustedProxyListOn) {
    bool failed = false;
    const auto b = resolveTlsBindings(
        {{"tls", {{"mode", "behind_proxy"}, {"trust_proxy_cidrs", {"10.0.0.0/8", "192.0.2.1"}}}}},
        makePaths(), "127.0.0.1", failed);
    EXPECT_FALSE(failed);
    EXPECT_EQ(b.trust_proxy_cidrs, (std::vector<std::string>{"10.0.0.0/8", "192.0.2.1"}));
}

TEST_F(TlsBootstrapTest, TrustedProxiesAreHonouredOnlyBehindAProxy) {
    // In any other mode the server terminates TLS itself and the peer IS the
    // client: a forwarded-for header there can only be a forgery.
    bool failed = false;
    const auto b = resolveTlsBindings(
        {{"tls", {{"mode", "auto"}, {"trust_proxy_cidrs", {"10.0.0.0/8"}}}}},
        makePaths(), "0.0.0.0", failed);
    EXPECT_FALSE(failed);
    EXPECT_TRUE(b.trust_proxy_cidrs.empty());
}

TEST_F(TlsBootstrapTest, InvalidTlsSectionFails) {
    bool failed = false;
    resolveTlsBindings({{"tls", {{"mode", "bogus"}}}}, makePaths(), "0.0.0.0", failed);
    EXPECT_TRUE(failed);
}

// ─── The certificate is renewed while the server runs, not only at start ────
namespace {
std::int64_t realNow() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
}  // namespace

TEST_F(TlsBootstrapTest, ACertificateIsDueOnlyWithinTheLastThirtyDays) {
    bool failed = false;
    const auto b = resolveTlsBindings(json::object(), makePaths(), "0.0.0.0", failed);
    ASSERT_FALSE(failed);
    const auto end = liveqx::tls::readCertInfo(b.cert_path).not_after_unix;
    const std::int64_t day = 86400;
    EXPECT_FALSE(serverCertDue(b.cert_path));
    EXPECT_FALSE(serverCertDue(b.cert_path, end - 31 * day));
    EXPECT_TRUE(serverCertDue(b.cert_path, end - 30 * day + 1));
    EXPECT_TRUE(serverCertDue(b.cert_path, end + day)) << "one that has ended is due, not skipped";
    EXPECT_TRUE(serverCertDue(b.tls_dir / "missing.crt")) << "none at all is due";
}

TEST_F(TlsBootstrapTest, AnUnreadableCertificateIsDue) {
    bool failed = false;
    const auto b = resolveTlsBindings(json::object(), makePaths(), "0.0.0.0", failed);
    ASSERT_FALSE(failed);
    { std::ofstream(b.cert_path, std::ios::trunc) << "not a certificate"; }
    EXPECT_TRUE(serverCertDue(b.cert_path));
}

TEST_F(TlsBootstrapTest, TheTimerLeavesAGoodCertificateAloneAndRenewsOneThatIsDue) {
    bool failed = false;
    const auto b = resolveTlsBindings(json::object(), makePaths(), "0.0.0.0", failed);
    ASSERT_FALSE(failed);
    const auto before = liveqx::tls::readCertInfo(b.cert_path);
    std::string err;

    EXPECT_EQ(renewServerCertIfDue(b.tls_dir, {}, err), ServerCertRenewal::NotDue);
    EXPECT_EQ(liveqx::tls::readCertInfo(b.cert_path).fingerprint_sha256, before.fingerprint_sha256);

    // A year and a bit later, as the clock sees it: due. The certificate is replaced, key and all.
    const auto later = before.not_after_unix - 5 * 86400;
    ASSERT_EQ(renewServerCertIfDue(b.tls_dir, {}, err, later), ServerCertRenewal::Renewed) << err;
    const auto after = liveqx::tls::readCertInfo(b.cert_path);
    EXPECT_NE(after.fingerprint_sha256, before.fingerprint_sha256);
    EXPECT_GE(after.not_after_unix, before.not_after_unix) << "issued from the real clock, so not earlier than the old one";
    EXPECT_TRUE(liveqx::tls::verifyKeyPair(b.cert_path, b.key_path).ok) << "the new certificate matches the new key";
    EXPECT_FALSE(serverCertDue(b.cert_path, realNow())) << "and it is good for a year again";
}

TEST_F(TlsBootstrapTest, AFailedRenewalLeavesTheOldCertificateServing) {
    bool failed = false;
    const auto b = resolveTlsBindings(json::object(), makePaths(), "0.0.0.0", failed);
    ASSERT_FALSE(failed);
    const auto before = liveqx::tls::readCertInfo(b.cert_path);
    // The CA key is gone: nothing can be signed.
    fs::remove(b.tls_dir / "ca.key");
    std::string err;
    EXPECT_EQ(renewServerCertIfDue(b.tls_dir, {}, err, before.not_after_unix - 5 * 86400), ServerCertRenewal::Failed);
    EXPECT_FALSE(err.empty());
    EXPECT_EQ(liveqx::tls::readCertInfo(b.cert_path).fingerprint_sha256, before.fingerprint_sha256);
}
