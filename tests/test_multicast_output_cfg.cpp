#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "output/MulticastOutputCfg.h"

using nlohmann::json;
using namespace liveqx::multicast;

namespace {

TEST(MulticastOutputCfg, MinimalValid) {
    auto c = parseOutputCfg(json{{"address", "239.1.1.1"}, {"port", 6000}});
    EXPECT_EQ(c.address, "239.1.1.1");
    EXPECT_EQ(c.port, 6000);
    EXPECT_EQ(c.ttl, 16);                 // default
    EXPECT_EQ(c.send_buffer_kb, 256);     // default
    EXPECT_EQ(c.container, "mpegts");     // default
}

TEST(MulticastOutputCfg, FullValid) {
    auto c = parseOutputCfg(json{
        {"address",        "239.1.1.1"},
        {"port",           6000},
        {"interface",      "192.168.1.10"},
        {"ttl",            32},
        {"send_buffer_kb", 1024},
        {"container",      "mpegts"},
    });
    EXPECT_EQ(c.bind_address,    "192.168.1.10");
    EXPECT_EQ(c.ttl,             32);
    EXPECT_EQ(c.send_buffer_kb,  1024);
}

TEST(MulticastOutputCfg, AcceptsBindAddressKey) {
    auto c = parseOutputCfg(json{
        {"address",      "239.1.1.1"},
        {"port",         5004},
        {"bind_address", "10.0.0.5"},
    });
    EXPECT_EQ(c.bind_address, "10.0.0.5");
}

TEST(MulticastOutputCfg, BindAddressOverridesLegacyInterface) {
    // When both keys are present, the canonical `bind_address` wins.
    auto c = parseOutputCfg(json{
        {"address",      "239.1.1.1"},
        {"port",         5004},
        {"bind_address", "10.0.0.5"},
        {"interface",    "192.168.1.10"},
    });
    EXPECT_EQ(c.bind_address, "10.0.0.5");
}

TEST(MulticastOutputCfg, RejectsMissingAddress) {
    EXPECT_THROW(parseOutputCfg(json{{"port", 6000}}), std::invalid_argument);
}
TEST(MulticastOutputCfg, RejectsBadPort) {
    EXPECT_THROW(parseOutputCfg(json{{"address","239.1.1.1"},{"port",0}}),
                 std::invalid_argument);
    EXPECT_THROW(parseOutputCfg(json{{"address","239.1.1.1"},{"port",70000}}),
                 std::invalid_argument);
}
TEST(MulticastOutputCfg, RejectsBadTtl) {
    EXPECT_THROW(parseOutputCfg(json{{"address","239.1.1.1"},{"port",6000},{"ttl",0}}),
                 std::invalid_argument);
    EXPECT_THROW(parseOutputCfg(json{{"address","239.1.1.1"},{"port",6000},{"ttl",1000}}),
                 std::invalid_argument);
}
TEST(MulticastOutputCfg, RejectsUnknownContainer) {
    EXPECT_THROW(parseOutputCfg(json{{"address","239.1.1.1"},{"port",6000},
                                     {"container","srt"}}),
                 std::invalid_argument);
}
TEST(MulticastOutputCfg, RejectsBadSendBuffer) {
    EXPECT_THROW(parseOutputCfg(json{{"address","239.1.1.1"},{"port",6000},
                                     {"send_buffer_kb", 4}}),
                 std::invalid_argument);
}

} // namespace
