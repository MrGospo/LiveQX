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

#include <algorithm>
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
    std::vector<FakeMqttClient*> setups;         // the short connections that give the server its rights first
    std::vector<ClientOptions>   setups_with;
    std::vector<std::string>     order;          // "setup" / "standing", in the order they were made

    BrokerLinkOptions options() {
        BrokerLinkOptions o;
        o.retry_s = 1;
        o.probe_timeout_s = 1;
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
            if (co.client_id == "liveqx-server-setup") {
                setups.push_back(f.get());
                setups_with.push_back(co);
                order.push_back("setup");
                return f;
            }
            made.push_back(f.get());
            made_with.push_back(co);
            order.push_back("standing");
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

TEST(BrokerLink, TheServerIsGivenItsRightsOnAShortConnectionBeforeTheStandingOneExists) {
    // The broker drops every connection of a user whose roles change; done on the standing connection
    // that would cut it at the very first start. So it is done before, on a throwaway connection.
    Rig rig;
    BrokerLink link(rig.options());
    link.start();
    ASSERT_TRUE(waitFor([&] { return link.status().state == "connected"; }));
    ASSERT_EQ(rig.setups.size(), 1u);
    ASSERT_EQ(rig.order.size(), 2u);
    EXPECT_EQ(rig.order[0], "setup") << "first the rights, then the standing connection";
    EXPECT_EQ(rig.setups_with[0].client_id, "liveqx-server-setup") << "another id: it must not displace the standing one";
    EXPECT_TRUE(rig.setups_with[0].clean_start);
    EXPECT_EQ(rig.setups_with[0].session_expiry_s, 0u);
    EXPECT_EQ(rig.setups_with[0].cert_chain_pem, "CERT") << "the same identity, so the same rights";
    EXPECT_GE(rig.setups[0]->countCommand("addClientRole"), 1u) << "the server's own role was bound there";
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

TEST(BrokerLink, TheComplaintOfAFailedPassIsGoneOnceAPassWorks) {
    Rig rig;
    rig.setWanted({A});
    BrokerLink link(rig.options());
    link.start();
    ASSERT_TRUE(waitFor([&] { return link.status().state == "connected" && link.status().last_reconcile_ok; }));
    rig.last()->dynsec = false;                         // the broker stops answering administration
    link.requestSync();
    ASSERT_TRUE(waitFor([&] { return !link.status().last_reconcile_ok; }));
    EXPECT_FALSE(link.status().detail.empty());
    rig.last()->dynsec = true;
    ASSERT_TRUE(waitFor([&] { return link.status().last_reconcile_ok; }));
    EXPECT_EQ(link.status().detail, "") << "a good pass leaves no old complaint on the page";
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

TEST(BrokerLink, ItListensToTheTopicsItIsGivenAndPassesOnWhatIsNotAReplyToItself) {
    Rig rig;
    std::mutex mu;
    std::vector<std::string> got;
    BrokerLinkOptions o = rig.options();
    o.subscriptions = {"liveqx/v1/players/+/presence"};
    o.on_message = [&](const liveqx::mqtt::Message& m) {
        std::lock_guard<std::mutex> lk(mu);
        got.push_back(m.topic + "=" + m.payload);
    };
    BrokerLink link(std::move(o));
    link.start();
    ASSERT_TRUE(waitFor([&] { return link.status().state == "connected"; }));
    FakeMqttClient* fake = rig.last();
    ASSERT_NE(fake, nullptr);
    EXPECT_NE(std::find(fake->subscriptions.begin(), fake->subscriptions.end(), "liveqx/v1/players/+/presence"),
              fake->subscriptions.end());
    EXPECT_NE(std::find(fake->subscriptions.begin(), fake->subscriptions.end(), "$CONTROL/dynamic-security/v1/response"),
              fake->subscriptions.end()) << "the replies to its own administration too";

    liveqx::mqtt::Message presence;
    presence.topic = "liveqx/v1/players/" + A + "/presence";
    presence.payload = "online";
    fake->deliver(presence);
    liveqx::mqtt::Message reply;
    reply.topic = "$CONTROL/dynamic-security/v1/response";
    reply.payload = R"({"responses":[{"command":"x","correlationData":"lqx-999"}]})";
    fake->deliver(reply);
    link.stop();
    std::lock_guard<std::mutex> lk(mu);
    ASSERT_EQ(got.size(), 1u) << "the administrative reply is not for the others";
    EXPECT_EQ(got[0], "liveqx/v1/players/" + A + "/presence=online");
}

TEST(BrokerLink, ARetainedMessageAskedToBeWipedIsSentEmptyOnceTheLinkIsUp) {
    Rig rig;
    rig.broker_up = false;
    BrokerLink link(rig.options());
    link.start();
    ASSERT_TRUE(waitFor([&] { return rig.madeCount() == 1; }));
    link.clearRetained("liveqx/v1/players/" + A + "/presence");       // asked while the broker is away
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    auto wipes = [&] {
        int n = 0;
        for (const auto& m : rig.last()->published)
            if (m.topic == "liveqx/v1/players/" + A + "/presence" && m.retain && m.payload.empty()) ++n;
        return n;
    };
    EXPECT_EQ(wipes(), 0);
    rig.last()->setLink(true);
    ASSERT_TRUE(waitFor([&] { return wipes() == 1; })) << "sent when the broker came back";
    link.clearRetained("liveqx/v1/players/" + A + "/presence");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_GE(wipes(), 1);
    link.stop();
}

TEST(BrokerLink, AskingFromTheMessageHandlerDoesNotDeadlockOrBlock) {
    Rig rig;
    BrokerLink* self = nullptr;
    BrokerLinkOptions o = rig.options();
    o.subscriptions = {"liveqx/v1/players/+/presence"};
    o.on_message = [&](const liveqx::mqtt::Message& m) { self->clearRetained(m.topic); };
    BrokerLink link(std::move(o));
    self = &link;
    link.start();
    ASSERT_TRUE(waitFor([&] { return link.status().state == "connected"; }));
    liveqx::mqtt::Message ghost;
    ghost.topic = "liveqx/v1/players/" + B + "/presence";
    ghost.payload = "online";
    rig.last()->deliver(ghost);
    ASSERT_TRUE(waitFor([&] {
        for (const auto& m : rig.last()->published)
            if (m.topic == ghost.topic && m.retain && m.payload.empty()) return true;
        return false;
    }));
    const auto t0 = std::chrono::steady_clock::now();
    link.stop();
    EXPECT_LT(std::chrono::steady_clock::now() - t0, 2s);
}

TEST(BrokerLink, ThePageIsToldWhenTheStateChangesAndNotWhileNothingDoes) {
    Rig rig;
    rig.identity_ready = false;
    std::atomic<int> told{0};
    auto o = rig.options();
    o.on_change = [&] { ++told; };
    BrokerLink link(std::move(o));
    link.start();
    ASSERT_TRUE(waitFor([&] { return !link.status().detail.empty(); }));
    const int waiting = told.load();
    EXPECT_GE(waiting, 1) << "waiting, with its reason, is a change from the start";

    // The same reason again and again (the link retries every second) is not news.
    std::this_thread::sleep_for(2500ms);
    EXPECT_EQ(told.load(), waiting);

    { std::lock_guard<std::mutex> lk(rig.mu); rig.identity_ready = true; }
    link.requestSync();
    ASSERT_TRUE(waitFor([&] { return link.status().last_reconcile_ok; }));
    EXPECT_GT(told.load(), waiting) << "connecting, connected and the reconciliation each reach the page";
    link.stop();
}

TEST(BrokerLink, TheResultOfAReconciliationReachesThePageToo) {
    Rig rig;
    rig.setWanted({A});
    std::atomic<int> told{0};
    auto o = rig.options();
    o.on_change = [&] { ++told; };
    BrokerLink link(std::move(o));
    link.start();
    ASSERT_TRUE(waitFor([&] { return link.status().last_reconcile_ok; }));
    ASSERT_TRUE(waitFor([&] { return link.status().state == "connected"; }));
    std::this_thread::sleep_for(300ms);
    const int before = told.load();
    rig.setWanted({A, B});
    link.requestSync();
    ASSERT_TRUE(waitFor([&] { return told.load() > before; })) << "a pass that changed nothing about the state still moves its time";
    link.stop();
}

TEST(BrokerLink, TheStatusIsShownAsTheInterfaceNeedsIt) {
    liveqx::mqtt::BrokerLinkStatus st;
    st.state = "waiting";
    st.detail = "the device CA is not set up yet";
    auto j = liveqx::mqtt::brokerStatusJson(st);
    EXPECT_EQ(j["state"], "waiting");
    EXPECT_EQ(j["detail"], "the device CA is not set up yet");
    EXPECT_EQ(j["last_reconcile_at"], 0);
    EXPECT_FALSE(j.contains("last_report")) << "nothing to report before the first reconciliation";

    st.state = "connected";
    st.last_reconcile_at = 1800000000;
    st.last_reconcile_ok = true;
    st.last_report.issued = 3;
    st.last_report.removed = 1;
    j = liveqx::mqtt::brokerStatusJson(st);
    EXPECT_EQ(j["last_report"]["issued"], 3);
    EXPECT_EQ(j["last_report"]["removed"], 1);
    EXPECT_EQ(j["last_report"]["failed"], 0);
    EXPECT_EQ(j["last_reconcile_ok"], true);
}
