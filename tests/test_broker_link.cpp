// mqtt/BrokerLink — the server's standing connection to the broker and the
// upkeep of the broker's player records, tested with an in-memory client.
//
// What these tests protect:
//   * with no identity or trust root yet, nothing connects and the server is
//     not troubled — the link just waits and says why;
//   * once it can, it connects and brings the broker in line with the players;
//   * a player created (or revoked) is reflected on the broker soon, on request;
//   * a broker that was down during a pairing catches up when it returns;
//   * when the server's identity is re-issued the link reconnects with it;
//   * stopping is prompt in every state.

#include "fake_mqtt_client.h"

#include "mqtt/BrokerLink.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <thread>

using liveqx::mqtt::BrokerIdentityPem;
using liveqx::mqtt::BrokerLink;
using liveqx::mqtt::BrokerLinkOptions;
using liveqx::mqtt::ClientOptions;
using liveqx::mqtt::IMqttClient;
using liveqx::testing::FakeMqttClient;
using namespace std::chrono_literals;

namespace {

const std::string A = "0b8f5c1e-3a2d-4c47-9a6e-1f2d3c4b5a69";
const std::string B = "7c1d2e3f-4a5b-4c6d-8e7f-90a1b2c3d4e5";

bool waitFor(const std::function<bool()>& pred, std::chrono::milliseconds limit = 8000ms) {
    const auto end = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < end) {
        if (pred()) return true;
        std::this_thread::sleep_for(10ms);
    }
    return pred();
}

// Everything the link needs, controllable from the test.
struct Rig {
    std::mutex mu;
    bool identity_ready{true};
    std::string serial{"serial-1"};
    std::string trust{"-----BEGIN CERTIFICATE-----\nroot\n-----END CERTIFICATE-----\n"};
    std::set<std::string> wanted;
    bool broker_up{true};
    std::vector<FakeMqttClient*> made;           // every client the link created
    std::vector<ClientOptions>   made_with;

    BrokerLinkOptions options() {
        BrokerLinkOptions o;
        o.retry_s = 1;
        o.reconcile_interval_s = 1;
        o.admin.reply_timeout = 500ms;
        o.identity = [this](BrokerIdentityPem& id, std::string& why) {
            std::lock_guard<std::mutex> lk(mu);
            if (!identity_ready) { why = "the device CA is not set up yet"; return false; }
            id.cert_chain_pem = "CERT";
            id.key_pem = "KEY";
            id.serial = serial;
            return true;
        };
        o.trust_pem = [this] { std::lock_guard<std::mutex> lk(mu); return trust; };
        o.wanted = [this] { std::lock_guard<std::mutex> lk(mu); return wanted; };
        o.make_client = [this](const ClientOptions& co) -> std::unique_ptr<IMqttClient> {
            auto f = std::make_unique<FakeMqttClient>();
            f->dynsec = true;
            f->clients.insert("liveqx-server");
            f->up = broker_up;
            std::lock_guard<std::mutex> lk(mu);
            made.push_back(f.get());
            made_with.push_back(co);
            return f;
        };
        return o;
    }
    std::size_t madeCount() { std::lock_guard<std::mutex> lk(mu); return made.size(); }
    FakeMqttClient* last() { std::lock_guard<std::mutex> lk(mu); return made.empty() ? nullptr : made.back(); }
    void setWanted(std::set<std::string> w) { std::lock_guard<std::mutex> lk(mu); wanted = std::move(w); }
};

}  // namespace

TEST(BrokerLink, WithNoIdentityYetItWaitsAndSaysWhy) {
    Rig rig;
    rig.identity_ready = false;
    BrokerLink link(rig.options());
    link.start();
    ASSERT_TRUE(waitFor([&] { return !link.status().detail.empty(); }));
    EXPECT_NE(link.status().detail.find("device CA"), std::string::npos) << link.status().detail;
    EXPECT_EQ(rig.madeCount(), 0u) << "nothing is connected without an identity";
    link.stop();
}

TEST(BrokerLink, WithNoTrustRootYetItWaitsToo) {
    Rig rig;
    rig.trust.clear();
    BrokerLink link(rig.options());
    link.start();
    ASSERT_TRUE(waitFor([&] { return !link.status().detail.empty(); }));
    EXPECT_EQ(rig.madeCount(), 0u);
    link.stop();
}

TEST(BrokerLink, WhenTheIdentityAppearsItConnectsAndBringsTheBrokerInLine) {
    Rig rig;
    rig.identity_ready = false;
    rig.setWanted({A});
    BrokerLink link(rig.options());
    link.start();
    ASSERT_TRUE(waitFor([&] { return !link.status().detail.empty(); }));
    { std::lock_guard<std::mutex> lk(rig.mu); rig.identity_ready = true; }
    link.requestSync();
    ASSERT_TRUE(waitFor([&] { return link.status().last_reconcile_ok; }));
    EXPECT_EQ(link.status().state, "connected");
    ASSERT_EQ(rig.madeCount(), 1u);
    EXPECT_EQ(rig.made_with[0].client_id, "liveqx-server");
    EXPECT_EQ(rig.made_with[0].cert_chain_pem, "CERT");
    EXPECT_TRUE(rig.made_with[0].verify_hostname);
    EXPECT_TRUE(rig.made[0]->clients.count(A)) << "the wanted player was issued";
    link.stop();
}

TEST(BrokerLink, ARequestedSyncIssuesTheNewPlayerAndRemovesTheRevokedOne) {
    Rig rig;
    rig.setWanted({A});
    BrokerLink link(rig.options());
    link.start();
    ASSERT_TRUE(waitFor([&] { return link.status().last_reconcile_ok; }));

    rig.setWanted({B});                                 // A revoked, B created
    link.requestSync();
    ASSERT_TRUE(waitFor([&] {
        auto st = link.status();
        return st.last_reconcile_ok && st.last_report.issued == 1 && st.last_report.removed == 1;
    })) << "the report of the pass that did the work";
    EXPECT_TRUE(rig.made[0]->clients.count(B));
    EXPECT_FALSE(rig.made[0]->clients.count(A));
    link.stop();
}

TEST(BrokerLink, ABrokerThatWasDownDuringThePairingCatchesUpWhenItReturns) {
    Rig rig;
    rig.broker_up = false;
    rig.setWanted({A});
    BrokerLink link(rig.options());
    link.start();
    ASSERT_TRUE(waitFor([&] { return rig.madeCount() == 1; }));
    ASSERT_TRUE(waitFor([&] { return link.status().state == "connecting"; }));
    EXPECT_EQ(link.status().last_reconcile_at, 0) << "nothing was reconciled with a broker that is not there";

    rig.last()->setLink(true);                          // the broker comes back
    ASSERT_TRUE(waitFor([&] { return link.status().last_reconcile_ok; }));
    EXPECT_EQ(link.status().state, "connected");
    EXPECT_TRUE(rig.made[0]->clients.count(A));
    link.stop();
}

TEST(BrokerLink, ALostConnectionIsReportedAndRecoveredFrom) {
    Rig rig;
    rig.setWanted({A});
    BrokerLink link(rig.options());
    link.start();
    ASSERT_TRUE(waitFor([&] { return link.status().state == "connected"; }));
    rig.last()->setLink(false);
    EXPECT_TRUE(waitFor([&] { return link.status().state == "connecting"; }));
    rig.last()->setLink(true);
    EXPECT_TRUE(waitFor([&] { return link.status().state == "connected"; }));
    link.stop();
}

TEST(BrokerLink, ARenewedIdentityMakesItReconnectWithTheNewOne) {
    Rig rig;
    BrokerLink link(rig.options());
    link.start();
    ASSERT_TRUE(waitFor([&] { return link.status().state == "connected"; }));
    ASSERT_EQ(rig.madeCount(), 1u);
    { std::lock_guard<std::mutex> lk(rig.mu); rig.serial = "serial-2"; }
    ASSERT_TRUE(waitFor([&] { return rig.madeCount() == 2; }, 10000ms)) << "noticed at the next tick";
    EXPECT_TRUE(waitFor([&] { return link.status().state == "connected"; }));
    link.stop();
}

TEST(BrokerLink, StoppingIsPromptWhetherWaitingOrConnected) {
    {
        Rig rig;
        rig.identity_ready = false;
        BrokerLinkOptions o = rig.options();
        o.retry_s = 30;
        o.reconcile_interval_s = 300;
        BrokerLink link(o);
        link.start();
        ASSERT_TRUE(waitFor([&] { return link.status().state == "waiting"; }));
        const auto t0 = std::chrono::steady_clock::now();
        link.stop();
        EXPECT_LT(std::chrono::steady_clock::now() - t0, 2s);
        EXPECT_EQ(link.status().state, "stopped");
    }
    {
        Rig rig;
        BrokerLinkOptions o = rig.options();
        o.retry_s = 30;
        o.reconcile_interval_s = 300;
        BrokerLink link(o);
        link.start();
        ASSERT_TRUE(waitFor([&] { return link.status().state == "connected"; }));
        const auto t0 = std::chrono::steady_clock::now();
        link.stop();
        EXPECT_LT(std::chrono::steady_clock::now() - t0, 2s);
    }
    {
        Rig rig;
        BrokerLink never_started(rig.options());      // destroyed without start()
    }
}
