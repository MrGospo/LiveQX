#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "output/RtmpOutputCfg.h"

using nlohmann::json;
using namespace liveqx::rtmp;

namespace {

// ─── happy path ──────────────────────────────────────────────────────────────

TEST(RtmpOutputCfg, MinimalValidJson) {
    auto c = parseOutputCfg(json{{"url", "rtmp://a.rtmp.youtube.com/live2/key"}});
    EXPECT_EQ(c.url,                      "rtmp://a.rtmp.youtube.com/live2/key");
    EXPECT_EQ(c.container,                "flv");
    EXPECT_EQ(c.reconnect_initial_ms,     1000);
    EXPECT_EQ(c.reconnect_max_ms,         60000);
    EXPECT_EQ(c.drop_after_disconnect_sec, 5);
}

TEST(RtmpOutputCfg, FullValidJson) {
    json j = {
        {"url",                       "rtmps://secure.example/app/key"},
        {"container",                 "flv"},
        {"reconnect_initial_ms",      500},
        {"reconnect_max_ms",          30000},
        {"drop_after_disconnect_sec", 10},
    };
    auto c = parseOutputCfg(j);
    EXPECT_EQ(c.url,                       "rtmps://secure.example/app/key");
    EXPECT_EQ(c.reconnect_initial_ms,      500);
    EXPECT_EQ(c.reconnect_max_ms,          30000);
    EXPECT_EQ(c.drop_after_disconnect_sec, 10);
}

TEST(RtmpOutputCfg, AcceptsRtmpsScheme) {
    auto c = parseOutputCfg(json{{"url", "rtmps://x.example/app/key"}});
    EXPECT_EQ(c.url, "rtmps://x.example/app/key");
}

// ─── validation ──────────────────────────────────────────────────────────────

TEST(RtmpOutputCfg, RejectsNonObject) {
    EXPECT_THROW(parseOutputCfg(json::array()), std::invalid_argument);
}

TEST(RtmpOutputCfg, RejectsMissingUrl) {
    EXPECT_THROW(parseOutputCfg(json{{"container", "flv"}}),
                 std::invalid_argument);
}

TEST(RtmpOutputCfg, RejectsEmptyUrl) {
    EXPECT_THROW(parseOutputCfg(json{{"url", ""}}), std::invalid_argument);
}

TEST(RtmpOutputCfg, RejectsUnknownScheme) {
    EXPECT_THROW(parseOutputCfg(json{{"url", "srt://h:1234"}}),
                 std::invalid_argument);
}

TEST(RtmpOutputCfg, RejectsUnsupportedContainer) {
    EXPECT_THROW(parseOutputCfg(json{{"url", "rtmp://h/a/k"}, {"container", "mpegts"}}),
                 std::invalid_argument);
}

TEST(RtmpOutputCfg, RejectsBackoffMaxBelowInitial) {
    json j = {
        {"url",                  "rtmp://h/a/k"},
        {"reconnect_initial_ms", 5000},
        {"reconnect_max_ms",     2000},
    };
    EXPECT_THROW(parseOutputCfg(j), std::invalid_argument);
}

TEST(RtmpOutputCfg, RejectsDropOutOfRange) {
    EXPECT_THROW(parseOutputCfg(json{{"url", "rtmp://h/a/k"},
                                     {"drop_after_disconnect_sec", -1}}),
                 std::invalid_argument);
    EXPECT_THROW(parseOutputCfg(json{{"url", "rtmp://h/a/k"},
                                     {"drop_after_disconnect_sec", 999999}}),
                 std::invalid_argument);
}

// ─── TLS ─────────────────────────────────────────────────────────────────────

TEST(RtmpOutputCfg, TlsVerifyDefaultsTrue) {
    auto c = parseOutputCfg(json{{"url", "rtmps://x.example/app/key"}});
    EXPECT_TRUE(c.tls_verify);
    EXPECT_TRUE(c.tls_ca_file.empty());
}

TEST(RtmpOutputCfg, AcceptsTlsKnobs) {
    json j = {
        {"url",         "rtmps://x.example/app/key"},
        {"tls_verify",  false},
        {"tls_ca_file", "/etc/ssl/custom-ca.pem"},
    };
    auto c = parseOutputCfg(j);
    EXPECT_FALSE(c.tls_verify);
    EXPECT_EQ(c.tls_ca_file, "/etc/ssl/custom-ca.pem");
}

TEST(RtmpOutputCfg, RejectsBadTlsTypes) {
    EXPECT_THROW(parseOutputCfg(json{{"url", "rtmps://x/app/k"},
                                     {"tls_verify", "yes"}}),
                 std::invalid_argument);
    EXPECT_THROW(parseOutputCfg(json{{"url", "rtmps://x/app/k"},
                                     {"tls_ca_file", 42}}),
                 std::invalid_argument);
}

} // namespace
