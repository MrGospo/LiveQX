#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "clips/RtmpInputCfg.h"

using nlohmann::json;
using namespace liveqx::rtmp;

namespace {

// ─── happy path ──────────────────────────────────────────────────────────────

TEST(RtmpInputCfg, MinimalClientJson) {
    auto j = json{{"url", "rtmp://stream.example.com/live/abc"}};
    auto c = parseInputCfg(j);
    EXPECT_EQ(c.mode, "client");                  // default
    EXPECT_EQ(c.url,  "rtmp://stream.example.com/live/abc");
    EXPECT_EQ(c.reconnect_initial_ms, 500);
    EXPECT_EQ(c.reconnect_max_ms,     30000);
    EXPECT_EQ(c.buffer_ms,            200);
    EXPECT_TRUE(c.stream_key.empty());
}

TEST(RtmpInputCfg, FullValidClientJson) {
    json j = {
        {"mode",                 "client"},
        {"url",                  "rtmp://h/live/k"},
        {"reconnect_initial_ms", 1000},
        {"reconnect_max_ms",     60000},
        {"buffer_ms",            500},
    };
    auto c = parseInputCfg(j);
    EXPECT_EQ(c.mode, "client");
    EXPECT_EQ(c.reconnect_initial_ms, 1000);
    EXPECT_EQ(c.reconnect_max_ms,     60000);
    EXPECT_EQ(c.buffer_ms,            500);
}

TEST(RtmpInputCfg, AcceptsRtmpsScheme) {
    auto c = parseInputCfg(json{{"url", "rtmps://secure.example.com/app/key"}});
    EXPECT_EQ(c.url, "rtmps://secure.example.com/app/key");
}

TEST(RtmpInputCfg, AcceptsServerModeShape) {
    json j = {
        {"mode",       "server"},
        {"url",        "rtmp://0.0.0.0:1935/live"},
        {"stream_key", "abc-secret"},
    };
    auto c = parseInputCfg(j);
    EXPECT_EQ(c.mode, "server");
    EXPECT_EQ(c.stream_key, "abc-secret");
}

TEST(RtmpInputCfg, RejectsServerWithoutStreamKey) {
    // Server mode with an empty key would let any publisher push to /live —
    // we refuse to ship an open relay by accident.
    json j1 = {{"mode", "server"}, {"url", "rtmp://0.0.0.0:1935/live"}};
    EXPECT_THROW(parseInputCfg(j1), std::invalid_argument);
    json j2 = {{"mode", "server"}, {"url", "rtmp://0.0.0.0:1935/live"},
               {"stream_key", ""}};
    EXPECT_THROW(parseInputCfg(j2), std::invalid_argument);
}

// ─── validation ──────────────────────────────────────────────────────────────

TEST(RtmpInputCfg, RejectsNonObject) {
    EXPECT_THROW(parseInputCfg(json::array()), std::invalid_argument);
}

TEST(RtmpInputCfg, RejectsMissingUrl) {
    EXPECT_THROW(parseInputCfg(json{{"mode", "client"}}), std::invalid_argument);
}

TEST(RtmpInputCfg, RejectsEmptyUrl) {
    EXPECT_THROW(parseInputCfg(json{{"url", ""}}), std::invalid_argument);
}

TEST(RtmpInputCfg, RejectsUnknownScheme) {
    EXPECT_THROW(parseInputCfg(json{{"url", "http://x.example/live"}}),
                 std::invalid_argument);
}

TEST(RtmpInputCfg, RejectsUnknownMode) {
    EXPECT_THROW(parseInputCfg(json{{"url", "rtmp://x"}, {"mode", "relay"}}),
                 std::invalid_argument);
}

TEST(RtmpInputCfg, RejectsMaxBelowInitial) {
    json j = {
        {"url",                  "rtmp://x/live/k"},
        {"reconnect_initial_ms", 5000},
        {"reconnect_max_ms",     1000},
    };
    EXPECT_THROW(parseInputCfg(j), std::invalid_argument);
}

TEST(RtmpInputCfg, RejectsBufferOutOfRange) {
    EXPECT_THROW(parseInputCfg(json{{"url", "rtmp://x"}, {"buffer_ms", -1}}),
                 std::invalid_argument);
    EXPECT_THROW(parseInputCfg(json{{"url", "rtmp://x"}, {"buffer_ms", 999999}}),
                 std::invalid_argument);
}

// ─── TLS ─────────────────────────────────────────────────────────────────────

TEST(RtmpInputCfg, TlsVerifyDefaultsTrue) {
    auto c = parseInputCfg(json{{"url", "rtmps://x.example/app/key"}});
    EXPECT_TRUE(c.tls_verify);
    EXPECT_TRUE(c.tls_ca_file.empty());
}

TEST(RtmpInputCfg, AcceptsTlsKnobs) {
    json j = {
        {"url",         "rtmps://x.example/app/key"},
        {"tls_verify",  false},
        {"tls_ca_file", "/etc/ssl/custom-ca.pem"},
    };
    auto c = parseInputCfg(j);
    EXPECT_FALSE(c.tls_verify);
    EXPECT_EQ(c.tls_ca_file, "/etc/ssl/custom-ca.pem");
}

TEST(RtmpInputCfg, RejectsBadTlsTypes) {
    EXPECT_THROW(parseInputCfg(json{{"url", "rtmps://x/app/k"},
                                    {"tls_verify", "yes"}}),
                 std::invalid_argument);
    EXPECT_THROW(parseInputCfg(json{{"url", "rtmps://x/app/k"},
                                    {"tls_ca_file", 42}}),
                 std::invalid_argument);
}

// ─── URL building ────────────────────────────────────────────────────────────

TEST(RtmpInputCfg, BuildUrlClientPassthrough) {
    InputCfg c;
    c.mode = "client";
    c.url  = "rtmp://h.example.com/live/abc";
    EXPECT_EQ(buildFfmpegUrl(c), "rtmp://h.example.com/live/abc");
}

TEST(RtmpInputCfg, BuildUrlServerAppendsKey) {
    // Server-mode embeds the stream key in the path so libavformat's
    // RTMP server only accepts publishers pushing to /live/{key}.
    // The listen=1 flag rides in AVDictionary, NOT the URL — keeping it
    // out of the query string lets path-matching against incoming
    // publishers work without spurious "Unexpected stream" rejections.
    InputCfg c;
    c.mode       = "server";
    c.url        = "rtmp://0.0.0.0:1935/live";
    c.stream_key = "abc-secret";
    const auto url = buildFfmpegUrl(c);
    EXPECT_EQ(url, "rtmp://0.0.0.0:1935/live/abc-secret");
    EXPECT_EQ(url.find("?listen=1"), std::string::npos);
}

} // namespace
