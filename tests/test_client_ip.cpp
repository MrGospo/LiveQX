// utils/ClientIp — who is the client when the server sits behind a reverse
// proxy.
//
// Behind a proxy every request arrives from the proxy's address, so per-address
// rate limits become one shared bucket and the audit trail names the proxy.
// X-Forwarded-For fixes that, but only if it is believed from proxies we
// trust and read the right way: a client can put anything at the START of the
// header, so the answer is the rightmost entry that is not one of our proxies.
//
// What these tests protect:
//   * with no trusted proxies configured the header is ignored (safe default);
//   * a header from an untrusted peer is ignored (spoofing);
//   * a forged leading entry does not win over the one the proxy appended;
//   * malformed input never turns into a made-up address;
//   * bad CIDR entries are reported and ignored — fewer trusted proxies is the
//     safe direction to fail in.

#include "utils/ClientIp.h"

#include <gtest/gtest.h>

using liveqx::net::TrustedProxies;

namespace {

TrustedProxies make(std::vector<std::string> cidrs, std::vector<std::string>* bad = nullptr) {
    return TrustedProxies::parse(cidrs, bad);
}

}  // namespace

TEST(ClientIp, WithoutTrustedProxiesTheHeaderIsIgnored) {
    auto p = make({});
    EXPECT_EQ(p.resolve("10.0.0.1", "203.0.113.5"), "10.0.0.1");
    EXPECT_TRUE(p.empty());
}

TEST(ClientIp, AHeaderFromAnUntrustedPeerIsIgnored) {
    auto p = make({"10.0.0.0/8"});
    EXPECT_EQ(p.resolve("198.51.100.9", "203.0.113.5"), "198.51.100.9")
        << "anyone can send the header; only our proxies are believed";
}

TEST(ClientIp, ATrustedProxyNamesTheClient) {
    auto p = make({"10.0.0.0/8"});
    EXPECT_EQ(p.resolve("10.1.2.3", "203.0.113.5"), "203.0.113.5");
}

TEST(ClientIp, TheRightmostUntrustedEntryWinsOverAForgedLeadingOne) {
    auto p = make({"10.0.0.0/8"});
    // The client claimed 6.6.6.6; the proxy appended the address it really saw.
    EXPECT_EQ(p.resolve("10.0.0.1", "6.6.6.6, 203.0.113.5"), "203.0.113.5");
}

TEST(ClientIp, ChainedProxiesAreSkipped) {
    auto p = make({"10.0.0.0/8"});
    EXPECT_EQ(p.resolve("10.0.0.1", "203.0.113.5, 10.0.0.2"), "203.0.113.5");
}

TEST(ClientIp, AChainMadeOnlyOfTrustedProxiesFallsBackToThePeer) {
    auto p = make({"10.0.0.0/8"});
    EXPECT_EQ(p.resolve("10.0.0.1", "10.0.0.7, 10.0.0.2"), "10.0.0.1");
}

TEST(ClientIp, MalformedHeadersNeverInventAnAddress) {
    auto p = make({"10.0.0.0/8"});
    EXPECT_EQ(p.resolve("10.0.0.1", ""), "10.0.0.1");
    EXPECT_EQ(p.resolve("10.0.0.1", "garbage"), "10.0.0.1");
    EXPECT_EQ(p.resolve("10.0.0.1", "203.0.113.5, garbage"), "10.0.0.1")
        << "an unreadable entry from a trusted hop means the header cannot be trusted";
    EXPECT_EQ(p.resolve("10.0.0.1", "203.0.113.5:8080"), "10.0.0.1") << "no ports";
    EXPECT_EQ(p.resolve("10.0.0.1", "unknown"), "10.0.0.1");
    EXPECT_EQ(p.resolve("10.0.0.1", std::string(5000, '1')), "10.0.0.1") << "absurdly long";
}

TEST(ClientIp, WhitespaceAroundEntriesIsTolerated) {
    auto p = make({"10.0.0.0/8"});
    EXPECT_EQ(p.resolve("10.0.0.1", "  203.0.113.5 ,  10.0.0.2  "), "203.0.113.5");
}

TEST(ClientIp, Ipv6IsSupported) {
    auto p = make({"fd00::/8", "::1"});
    EXPECT_EQ(p.resolve("fd00::5", "2001:db8::1"), "2001:db8::1");
    EXPECT_EQ(p.resolve("::1", "2001:db8::2"), "2001:db8::2");
    EXPECT_EQ(p.resolve("2001:db8::99", "2001:db8::2"), "2001:db8::99");
}

TEST(ClientIp, AnIpv4MappedPeerMatchesAnIpv4Network) {
    // A dual-stack listener reports IPv4 peers as ::ffff:a.b.c.d.
    auto p = make({"10.0.0.0/8"});
    EXPECT_EQ(p.resolve("::ffff:10.0.0.1", "203.0.113.5"), "203.0.113.5");
}

TEST(ClientIp, ABareAddressIsAHostRoute) {
    auto p = make({"192.0.2.10"});
    EXPECT_EQ(p.resolve("192.0.2.10", "203.0.113.5"), "203.0.113.5");
    EXPECT_EQ(p.resolve("192.0.2.11", "203.0.113.5"), "192.0.2.11");
}

TEST(ClientIp, PrefixLengthsAreRespected) {
    auto p = make({"192.168.1.0/25"});
    EXPECT_EQ(p.resolve("192.168.1.100", "203.0.113.5"), "203.0.113.5");
    EXPECT_EQ(p.resolve("192.168.1.200", "203.0.113.5"), "192.168.1.200");
    auto zero = make({"0.0.0.0/0"});
    // /0 trusts every IPv4 address, the client in the header included, so no
    // "foreign" entry is left and the peer stays. Pointless, but harmless.
    EXPECT_EQ(zero.resolve("8.8.8.8", "203.0.113.5"), "8.8.8.8");
}

TEST(ClientIp, BadEntriesAreReportedAndIgnored) {
    std::vector<std::string> bad;
    auto p = make({"10.0.0.0/8", "10.0.0.0/33", "nonsense", "fd00::/129", "", "10.0.0.0/"}, &bad);
    EXPECT_EQ(bad.size(), 5u);
    EXPECT_EQ(p.resolve("10.0.0.1", "203.0.113.5"), "203.0.113.5") << "the valid one still works";
    EXPECT_EQ(p.resolve("172.16.0.1", "203.0.113.5"), "172.16.0.1");
}
