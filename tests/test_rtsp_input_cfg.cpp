#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "clips/RtspInputCfg.h"

using nlohmann::json;
using namespace liveqx::rtsp;

namespace {

// fix15 c9 — RtspInputCfg parser tests.
//
// These pin the field-level error contracts the REST handler relies
// on: a malformed cfg should fail loudly with a precise message, not
// silently fall through to a generic "bad cfg" rejection. Coverage
// here is symmetric with test_rtmp_input_cfg.cpp so the two parsers
// can't drift apart on shared invariants (must-be-string url, etc).

// ─── happy path ──────────────────────────────────────────────────────────────

TEST(RtspInputCfg, MinimalJson) {
    auto c = parseInputCfg(json{{"url", "rtsp://192.168.1.10:554/main"}});
    EXPECT_EQ(c.url, "rtsp://192.168.1.10:554/main");
    EXPECT_EQ(c.transport, "tcp");                      // default
    EXPECT_TRUE(c.user.empty());
    EXPECT_TRUE(c.password.empty());
    EXPECT_EQ(c.reconnect_max_backoff_sec, 10);         // default
    EXPECT_TRUE(c.tls_verify);                          // default
    EXPECT_EQ(c.rw_timeout_ms, 5000);
    EXPECT_EQ(c.reorder_queue_size, 2048);
    EXPECT_EQ(c.user_agent, "LiveQX/1.0");
}

TEST(RtspInputCfg, AcceptsRtspsScheme) {
    auto c = parseInputCfg(json{{"url", "rtsps://secure.cam:8554/main"}});
    EXPECT_EQ(c.url, "rtsps://secure.cam:8554/main");
}

TEST(RtspInputCfg, AcceptsAllKnobs) {
    json j = {
        {"url",                       "rtsps://camera/path"},
        {"transport",                 "udp"},
        {"user",                      "admin"},
        {"password",                  "s3cr3t"},
        {"reconnect_max_backoff_sec", 30},
        {"tls_verify",                false},
        {"tls_ca_file",               "/etc/ssl/private-ca.pem"},
        {"rw_timeout_ms",             10000},
        {"reorder_queue_size",        4096},
        {"user_agent",                "LibVLC/3.0.18 LibVLC/3.0.18"},
    };
    auto c = parseInputCfg(j);
    EXPECT_EQ(c.transport, "udp");
    EXPECT_EQ(c.user,     "admin");
    EXPECT_EQ(c.password, "s3cr3t");
    EXPECT_EQ(c.reconnect_max_backoff_sec, 30);
    EXPECT_FALSE(c.tls_verify);
    EXPECT_EQ(c.tls_ca_file, "/etc/ssl/private-ca.pem");
    EXPECT_EQ(c.rw_timeout_ms, 10000);
    EXPECT_EQ(c.reorder_queue_size, 4096);
    EXPECT_EQ(c.user_agent, "LibVLC/3.0.18 LibVLC/3.0.18");
}

// ─── url ─────────────────────────────────────────────────────────────────────

TEST(RtspInputCfg, RejectsNonObject) {
    EXPECT_THROW(parseInputCfg(json::array()), std::invalid_argument);
}

TEST(RtspInputCfg, RejectsMissingUrl) {
    EXPECT_THROW(parseInputCfg(json{{"transport", "tcp"}}),
                 std::invalid_argument);
}

TEST(RtspInputCfg, RejectsEmptyUrl) {
    EXPECT_THROW(parseInputCfg(json{{"url", ""}}), std::invalid_argument);
}

TEST(RtspInputCfg, RejectsNonStringUrl) {
    EXPECT_THROW(parseInputCfg(json{{"url", 42}}), std::invalid_argument);
}

TEST(RtspInputCfg, RejectsUnknownScheme) {
    EXPECT_THROW(parseInputCfg(json{{"url", "http://h/p"}}),
                 std::invalid_argument);
    EXPECT_THROW(parseInputCfg(json{{"url", "rtmp://h/p"}}),
                 std::invalid_argument);
    EXPECT_THROW(parseInputCfg(json{{"url", "host-only"}}),
                 std::invalid_argument);
}

// ─── transport ───────────────────────────────────────────────────────────────

TEST(RtspInputCfg, AcceptsTcpAndUdp) {
    auto c1 = parseInputCfg(json{{"url", "rtsp://h/p"}, {"transport", "tcp"}});
    auto c2 = parseInputCfg(json{{"url", "rtsp://h/p"}, {"transport", "udp"}});
    EXPECT_EQ(c1.transport, "tcp");
    EXPECT_EQ(c2.transport, "udp");
}

TEST(RtspInputCfg, RejectsUnknownTransport) {
    // udp_multicast belongs on MulticastInput, http is a CDN edge case
    // we don't ship for cameras.
    EXPECT_THROW(parseInputCfg(json{{"url", "rtsp://h/p"},
                                    {"transport", "udp_multicast"}}),
                 std::invalid_argument);
    EXPECT_THROW(parseInputCfg(json{{"url", "rtsp://h/p"},
                                    {"transport", "http"}}),
                 std::invalid_argument);
}

TEST(RtspInputCfg, RejectsNonStringTransport) {
    EXPECT_THROW(parseInputCfg(json{{"url", "rtsp://h/p"}, {"transport", 1}}),
                 std::invalid_argument);
}

// ─── auth ────────────────────────────────────────────────────────────────────

TEST(RtspInputCfg, RejectsHalfSetCreds) {
    // Half-set creds are almost always a typo and FFmpeg silently drops
    // the half — operator stares at unauthorized errors with no clue.
    EXPECT_THROW(parseInputCfg(json{{"url", "rtsp://h/p"},
                                    {"user", "admin"}}),
                 std::invalid_argument);
    EXPECT_THROW(parseInputCfg(json{{"url", "rtsp://h/p"},
                                    {"password", "x"}}),
                 std::invalid_argument);
}

// ─── reconnect_max_backoff_sec ───────────────────────────────────────────────

TEST(RtspInputCfg, BackoffOutOfRangeRejected) {
    EXPECT_THROW(parseInputCfg(json{{"url", "rtsp://h/p"},
                                    {"reconnect_max_backoff_sec", 0}}),
                 std::invalid_argument);
    EXPECT_THROW(parseInputCfg(json{{"url", "rtsp://h/p"},
                                    {"reconnect_max_backoff_sec", 601}}),
                 std::invalid_argument);
    EXPECT_THROW(parseInputCfg(json{{"url", "rtsp://h/p"},
                                    {"reconnect_max_backoff_sec", "10"}}),
                 std::invalid_argument);
}

// ─── TLS ─────────────────────────────────────────────────────────────────────

TEST(RtspInputCfg, RejectsBadTlsTypes) {
    EXPECT_THROW(parseInputCfg(json{{"url", "rtsps://h/p"},
                                    {"tls_verify", "yes"}}),
                 std::invalid_argument);
    EXPECT_THROW(parseInputCfg(json{{"url", "rtsps://h/p"},
                                    {"tls_ca_file", 42}}),
                 std::invalid_argument);
}

// ─── ffmpeg I/O knobs ────────────────────────────────────────────────────────

TEST(RtspInputCfg, RwTimeoutOutOfRangeRejected) {
    EXPECT_THROW(parseInputCfg(json{{"url", "rtsp://h/p"},
                                    {"rw_timeout_ms", 100}}),
                 std::invalid_argument);
    EXPECT_THROW(parseInputCfg(json{{"url", "rtsp://h/p"},
                                    {"rw_timeout_ms", 99'999}}),
                 std::invalid_argument);
}

TEST(RtspInputCfg, ReorderQueueOutOfRangeRejected) {
    EXPECT_THROW(parseInputCfg(json{{"url", "rtsp://h/p"},
                                    {"reorder_queue_size", -1}}),
                 std::invalid_argument);
    EXPECT_THROW(parseInputCfg(json{{"url", "rtsp://h/p"},
                                    {"reorder_queue_size", 99'999}}),
                 std::invalid_argument);
}

TEST(RtspInputCfg, EmptyUserAgentRejected) {
    // FFmpeg accepts an empty UA but most cameras don't — fail loudly.
    EXPECT_THROW(parseInputCfg(json{{"url", "rtsp://h/p"},
                                    {"user_agent", ""}}),
                 std::invalid_argument);
    EXPECT_THROW(parseInputCfg(json{{"url", "rtsp://h/p"},
                                    {"user_agent", 1}}),
                 std::invalid_argument);
}

} // namespace
