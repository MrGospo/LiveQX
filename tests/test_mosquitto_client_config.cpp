// mqtt/MosquittoClient — what it does with bad configuration and with a broker
// that is not there. (Talking to a real broker is tested in
// test_mosquitto_client_broker.cpp.)
//
// What these tests protect:
//   * unusable TLS material or identity is refused up front, with a reason an
//     administrator can read, and nothing is started;
//   * a missing broker is not an error of start(): the client keeps trying in
//     the background, reports each failure, and stops promptly when asked;
//   * publishing without a connection says so (NotConnected) rather than
//     queueing silently, and a message that can never be valid is Rejected;
//   * stop() and destruction are always safe, in any state.

#include "mqtt/MosquittoClient.h"
#include "utils/Tls.h"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

namespace fs   = std::filesystem;
namespace tls  = liveqx::tls;
namespace mqtt = liveqx::mqtt;
using namespace std::chrono_literals;

namespace {

std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

// A TCP port nobody is listening on.
int closedPort() {
    int s = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    ::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a);
    socklen_t len = sizeof a;
    ::getsockname(s, reinterpret_cast<sockaddr*>(&a), &len);
    const int port = ntohs(a.sin_port);
    ::close(s);
    return port;
}

class MosquittoClientConfigTest : public ::testing::Test {
protected:
    fs::path dir_;
    std::string ca_, cert_, key_;

    void SetUp() override {
        static std::atomic<int> seq{0};
        dir_ = fs::temp_directory_path() /
               ("lqx-mqttcfg-" + std::to_string(::getpid()) + "-" + std::to_string(seq.fetch_add(1)));
        fs::create_directories(dir_);
        ASSERT_TRUE(tls::ensureCa(dir_).ok());
        tls::SanList sans;
        sans.dns_names = {"localhost"};
        sans.ip_v4 = {"127.0.0.1"};
        ASSERT_TRUE(tls::issueServerCert(dir_, "localhost", sans).ok());
        ca_   = slurp(dir_ / "ca.crt");
        cert_ = slurp(dir_ / "server.crt");      // any matching certificate/key pair will do here
        key_  = slurp(dir_ / "server.key");
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    mqtt::ClientOptions good() {
        mqtt::ClientOptions o;
        o.host = "127.0.0.1";
        o.port = closedPort();
        o.client_id = "liveqx-server";
        o.ca_pem = ca_;
        o.cert_chain_pem = cert_;
        o.key_pem = key_;
        o.reconnect_min_s = 1;
        o.reconnect_max_s = 2;
        return o;
    }
};

}  // namespace

TEST_F(MosquittoClientConfigTest, AMissingClientIdOrAddressIsRefused) {
    auto o = good();
    o.client_id.clear();
    mqtt::MosquittoClient a(o);
    EXPECT_FALSE(a.start());
    EXPECT_FALSE(a.lastError().empty());

    o = good();
    o.host.clear();
    mqtt::MosquittoClient b(o);
    EXPECT_FALSE(b.start());

    o = good();
    o.port = 0;
    mqtt::MosquittoClient c(o);
    EXPECT_FALSE(c.start());
}

TEST_F(MosquittoClientConfigTest, UnusableTlsMaterialIsRefusedWithAReadableReason) {
    {
        auto o = good();
        o.ca_pem = "not a certificate";
        mqtt::MosquittoClient c(o);
        EXPECT_FALSE(c.start());
        EXPECT_NE(c.lastError().find("CA"), std::string::npos) << c.lastError();
    }
    {
        auto o = good();
        o.cert_chain_pem = "garbage";
        mqtt::MosquittoClient c(o);
        EXPECT_FALSE(c.start());
        EXPECT_NE(c.lastError().find("certificate"), std::string::npos) << c.lastError();
    }
    {
        auto o = good();
        o.key_pem = "garbage";
        mqtt::MosquittoClient c(o);
        EXPECT_FALSE(c.start());
        EXPECT_NE(c.lastError().find("key"), std::string::npos) << c.lastError();
    }
    {
        // A key that is real but belongs to another certificate.
        const fs::path other = dir_ / "other";
        fs::create_directories(other);
        ASSERT_TRUE(tls::ensureCa(other, "Other CA").ok());
        tls::SanList sans;
        sans.dns_names = {"localhost"};
        ASSERT_TRUE(tls::issueServerCert(other, "localhost", sans).ok());
        auto o = good();
        o.key_pem = slurp(other / "server.key");
        mqtt::MosquittoClient c(o);
        EXPECT_FALSE(c.start());
        EXPECT_NE(c.lastError().find("match"), std::string::npos) << c.lastError();
    }
}

TEST_F(MosquittoClientConfigTest, ABrokerThatIsNotThereIsRetriedInTheBackgroundNotAFailureOfStart) {
    mqtt::MosquittoClient c(good());
    std::mutex mu;
    std::vector<std::string> reasons;
    c.setStateHandler([&](bool up, const std::string& why) {
        std::lock_guard<std::mutex> lk(mu);
        EXPECT_FALSE(up);
        reasons.push_back(why);
    });
    ASSERT_TRUE(c.start()) << c.lastError();
    EXPECT_FALSE(c.connected());

    for (int i = 0; i < 100; ++i) {
        { std::lock_guard<std::mutex> lk(mu); if (reasons.size() >= 2) break; }
        std::this_thread::sleep_for(100ms);
    }
    {
        std::lock_guard<std::mutex> lk(mu);
        ASSERT_GE(reasons.size(), 2u) << "it keeps trying and says so each time";
        EXPECT_NE(reasons[0].find("cannot connect"), std::string::npos) << reasons[0];
    }
    EXPECT_NE(c.lastError().find("cannot connect"), std::string::npos);

    const auto t0 = std::chrono::steady_clock::now();
    c.stop();
    EXPECT_LT(std::chrono::steady_clock::now() - t0, 3s) << "stopping does not wait out a retry pause";
}

TEST_F(MosquittoClientConfigTest, PublishingWithoutAConnectionSaysSoAndAnImpossibleMessageIsRejected) {
    mqtt::MosquittoClient c(good());
    ASSERT_TRUE(c.start());

    mqtt::Message ok;
    ok.topic = "liveqx/v1/players/x/commands";
    ok.payload = "{}";
    EXPECT_EQ(c.publish(ok), mqtt::PublishResult::NotConnected);

    mqtt::Message wildcard = ok;
    wildcard.topic = "liveqx/v1/players/+/commands";
    EXPECT_EQ(c.publish(wildcard), mqtt::PublishResult::Rejected) << "one can only publish to a concrete topic";
    mqtt::Message hash = ok;
    hash.topic = "liveqx/#";
    EXPECT_EQ(c.publish(hash), mqtt::PublishResult::Rejected);
    mqtt::Message empty = ok;
    empty.topic.clear();
    EXPECT_EQ(c.publish(empty), mqtt::PublishResult::Rejected);
    mqtt::Message badQos = ok;
    badQos.qos = 3;
    EXPECT_EQ(c.publish(badQos), mqtt::PublishResult::Rejected);
    c.stop();
}

TEST_F(MosquittoClientConfigTest, StoppingAndDestroyingAreSafeInAnyState) {
    {
        mqtt::MosquittoClient never_started(good());
        never_started.stop();
        never_started.stop();
    }
    {
        mqtt::MosquittoClient c(good());
        ASSERT_TRUE(c.start());
        c.stop();
        c.stop();                          // twice
        EXPECT_EQ(c.publish(mqtt::Message{"liveqx/x", "p", 1, false, std::nullopt}), mqtt::PublishResult::NotConnected);
    }
    {
        mqtt::MosquittoClient c(good());
        ASSERT_TRUE(c.start());            // destroyed while it is still retrying
    }
    {
        auto o = good();
        o.client_id.clear();
        mqtt::MosquittoClient refused(o);
        EXPECT_FALSE(refused.start());     // destroyed after a refused start
    }
}

TEST_F(MosquittoClientConfigTest, AFilterThatCannotBeSubscribedIsIgnoredNotRemembered) {
    mqtt::MosquittoClient c(good());
    c.subscribe("liveqx/v1/#/bad", 1);     // '#' in the middle is not a valid filter
    c.subscribe("liveqx/v1/players/+/presence", 1);
    ASSERT_TRUE(c.start());
    c.stop();
    SUCCEED();
}
