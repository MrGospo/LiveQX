#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "clips/MulticastInputCfg.h"

using nlohmann::json;
using namespace liveqx::multicast;

namespace {

// ─── happy path ──────────────────────────────────────────────────────────────

TEST(MulticastInputCfg, MinimalValidJson) {
    auto j = json{{"address", "239.0.0.1"}, {"port", 5004}};
    auto c = parseInputCfg(j);
    EXPECT_EQ(c.address, "239.0.0.1");
    EXPECT_EQ(c.port, 5004);
    EXPECT_EQ(c.container, "mpegts");          // default
    EXPECT_EQ(c.jitter_buffer_ms, 100);        // default
    EXPECT_EQ(c.reconnect_on_silence_sec, 10); // default
    EXPECT_TRUE(c.interface_addr.empty());
}

TEST(MulticastInputCfg, FullValidJson) {
    json j = {
        {"address",                  "239.1.2.3"},
        {"port",                     5004},
        {"interface",                "192.168.1.10"},
        {"container",                "rtp"},
        {"jitter_buffer_ms",         250},
        {"reconnect_on_silence_sec", 30},
    };
    auto c = parseInputCfg(j);
    EXPECT_EQ(c.address,                  "239.1.2.3");
    EXPECT_EQ(c.port,                     5004);
    EXPECT_EQ(c.interface_addr,           "192.168.1.10");
    EXPECT_EQ(c.container,                "rtp");
    EXPECT_EQ(c.jitter_buffer_ms,         250);
    EXPECT_EQ(c.reconnect_on_silence_sec, 30);
}

// ─── validation ──────────────────────────────────────────────────────────────

TEST(MulticastInputCfg, RejectsNonObject) {
    EXPECT_THROW(parseInputCfg(json::array()), std::invalid_argument);
}

TEST(MulticastInputCfg, RejectsMissingAddress) {
    EXPECT_THROW(parseInputCfg(json{{"port", 5004}}), std::invalid_argument);
}

TEST(MulticastInputCfg, RejectsEmptyAddress) {
    EXPECT_THROW(parseInputCfg(json{{"address", ""}, {"port", 5004}}),
                 std::invalid_argument);
}

TEST(MulticastInputCfg, RejectsMissingPort) {
    EXPECT_THROW(parseInputCfg(json{{"address", "239.0.0.1"}}),
                 std::invalid_argument);
}

TEST(MulticastInputCfg, RejectsPortOutOfRange) {
    EXPECT_THROW(parseInputCfg(json{{"address", "239.0.0.1"}, {"port", 0}}),
                 std::invalid_argument);
    EXPECT_THROW(parseInputCfg(json{{"address", "239.0.0.1"}, {"port", 70000}}),
                 std::invalid_argument);
}

TEST(MulticastInputCfg, RejectsUnknownContainer) {
    EXPECT_THROW(parseInputCfg(json{{"address", "239.0.0.1"},
                                    {"port", 5004},
                                    {"container", "srt"}}),
                 std::invalid_argument);
}

TEST(MulticastInputCfg, RejectsJitterOutOfRange) {
    EXPECT_THROW(parseInputCfg(json{{"address", "239.0.0.1"},
                                    {"port", 5004},
                                    {"jitter_buffer_ms", -1}}),
                 std::invalid_argument);
    EXPECT_THROW(parseInputCfg(json{{"address", "239.0.0.1"},
                                    {"port", 5004},
                                    {"jitter_buffer_ms", 9999}}),
                 std::invalid_argument);
}

// ─── URL building ────────────────────────────────────────────────────────────

TEST(MulticastInputCfg, BuildUrlMinimal) {
    InputCfg c;
    c.address = "239.0.0.1";
    c.port    = 5004;
    auto url  = buildFfmpegUrl(c);
    EXPECT_TRUE(url.starts_with("udp://239.0.0.1:5004?"));
    EXPECT_NE(url.find("reuse=1"),      std::string::npos);
    EXPECT_NE(url.find("fifo_size="),   std::string::npos);
    EXPECT_EQ(url.find("localaddr="),   std::string::npos);
}

TEST(MulticastInputCfg, BuildUrlWithInterface) {
    InputCfg c;
    c.address        = "239.0.0.1";
    c.port           = 5004;
    c.interface_addr = "192.168.1.10";
    auto url = buildFfmpegUrl(c);
    EXPECT_NE(url.find("localaddr=192.168.1.10"), std::string::npos);
}

} // namespace
