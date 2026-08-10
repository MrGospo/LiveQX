// fix38 — TlsConfig parser unit tests.

#include "utils/TlsConfig.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

using nlohmann::json;
namespace tls = liveqx::tls;

TEST(TlsConfigParse, EmptyConfigDefaultsToAutoMode) {
    auto r = tls::parseConfig(json::object());
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(r.config.mode, tls::Mode::Auto);
    EXPECT_TRUE(r.config.cert_path.empty());
    EXPECT_TRUE(r.config.san_extra.empty());
    EXPECT_FALSE(r.config.allow_insecure_bind);
}

TEST(TlsConfigParse, MissingTlsKeyDefaultsToAuto) {
    json cfg = {{"unrelated", 1}};
    auto r = tls::parseConfig(cfg);
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(r.config.mode, tls::Mode::Auto);
}

TEST(TlsConfigParse, ProvidedModeRequiresCertAndKey) {
    json cfg = { { "tls", { { "mode", "provided" } } } };
    auto r = tls::parseConfig(cfg);
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(r.errors.size(), 2u);
}

TEST(TlsConfigParse, ProvidedModeAcceptsCertAndKey) {
    json cfg = { { "tls", {
        { "mode", "provided" },
        { "cert_path", "/etc/pki/server.crt" },
        { "key_path",  "/etc/pki/server.key" },
    } } };
    auto r = tls::parseConfig(cfg);
    ASSERT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0]);
    EXPECT_EQ(r.config.mode, tls::Mode::Provided);
    EXPECT_EQ(r.config.cert_path.string(), "/etc/pki/server.crt");
    EXPECT_EQ(r.config.key_path.string(),  "/etc/pki/server.key");
}

TEST(TlsConfigParse, BogusModeReportsError) {
    json cfg = { { "tls", { { "mode", "lolcrypt" } } } };
    auto r = tls::parseConfig(cfg);
    EXPECT_FALSE(r.ok());
    ASSERT_FALSE(r.errors.empty());
    EXPECT_NE(r.errors[0].find("auto, provided"), std::string::npos);
}

TEST(TlsConfigParse, SanExtraReadsStringArray) {
    json cfg = { { "tls", {
        { "san_extra", { "core.lan", "10.0.0.5", "fd00::1" } },
    } } };
    auto r = tls::parseConfig(cfg);
    ASSERT_TRUE(r.ok());
    ASSERT_EQ(r.config.san_extra.size(), 3u);
    EXPECT_EQ(r.config.san_extra[0], "core.lan");
    EXPECT_EQ(r.config.san_extra[1], "10.0.0.5");
    EXPECT_EQ(r.config.san_extra[2], "fd00::1");
}

TEST(TlsConfigParse, NonStringSanIsSkippedWithWarning) {
    json cfg = { { "tls", {
        { "san_extra", { "core.lan", 42, "ok.local" } },
    } } };
    auto r = tls::parseConfig(cfg);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.config.san_extra.size(), 2u);
    EXPECT_EQ(r.warnings.size(), 1u);
}

TEST(TlsConfigParse, TrustProxyCidrsReadsArray) {
    json cfg = { { "tls", {
        { "mode", "behind_proxy" },
        { "trust_proxy_cidrs", { "10.0.0.0/8", "192.168.1.0/24" } },
    } } };
    auto r = tls::parseConfig(cfg);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.config.mode, tls::Mode::BehindProxy);
    ASSERT_EQ(r.config.trust_proxy_cidrs.size(), 2u);
    EXPECT_EQ(r.config.trust_proxy_cidrs[1], "192.168.1.0/24");
}

TEST(TlsConfigParse, AllowInsecureBindIsBoolean) {
    json cfg = { { "tls", { { "allow_insecure_bind", true } } } };
    auto r = tls::parseConfig(cfg);
    ASSERT_TRUE(r.ok());
    EXPECT_TRUE(r.config.allow_insecure_bind);
}

TEST(TlsConfigParse, AllowInsecureBindWrongTypeFails) {
    json cfg = { { "tls", { { "allow_insecure_bind", "yes" } } } };
    auto r = tls::parseConfig(cfg);
    EXPECT_FALSE(r.ok());
}

TEST(TlsConfigParse, ModeStringRoundTrip) {
    bool ok = false;
    EXPECT_EQ(tls::modeFromString("auto", ok),         tls::Mode::Auto);         EXPECT_TRUE(ok);
    EXPECT_EQ(tls::modeFromString("provided", ok),     tls::Mode::Provided);     EXPECT_TRUE(ok);
    EXPECT_EQ(tls::modeFromString("behind_proxy", ok), tls::Mode::BehindProxy);  EXPECT_TRUE(ok);
    EXPECT_EQ(tls::modeFromString("disabled", ok),     tls::Mode::Disabled);     EXPECT_TRUE(ok);
    tls::modeFromString("nope", ok);
    EXPECT_FALSE(ok);

    EXPECT_STREQ(tls::modeToString(tls::Mode::Auto),        "auto");
    EXPECT_STREQ(tls::modeToString(tls::Mode::Provided),    "provided");
    EXPECT_STREQ(tls::modeToString(tls::Mode::BehindProxy), "behind_proxy");
    EXPECT_STREQ(tls::modeToString(tls::Mode::Disabled),    "disabled");
}
