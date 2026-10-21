// mqtt/MosquittoClient against a real Mosquitto broker (mutual TLS, identity from
// the certificate, Dynamic Security rights — see test_broker.h).
//
// What these tests protect:
//   * the server and a player connect with certificates held in memory only;
//   * the broker's name is checked unless the administrator turns that off;
//   * messages flow both ways under the rights we issue, and a player sees
//     nothing meant for another;
//   * after the broker restarts, the client reconnects and renews its
//     subscriptions on its own;
//   * a persistent session keeps a message for an offline client, and an
//     expired message is not delivered late;
//   * the server issues and removes players through Dynamic Security itself,
//     idempotently, and reconciles the broker with a wanted list;
//   * a client the broker has no record of may connect but can neither receive
//     nor send anything.
//
// All tests are skipped when mosquitto, mosquitto_ctrl or the Dynamic Security
// plugin are not installed.

#include "test_broker.h"

#include "mqtt/BrokerAdmin.h"
#include "mqtt/BrokerLink.h"
#include "mqtt/MosquittoClient.h"

#include <gtest/gtest.h>

#include <mutex>

namespace mqtt = liveqx::mqtt;
using liveqx::testing::TestBroker;
using liveqx::testing::waitFor;

namespace {

struct Inbox {
    std::mutex mu;
    std::vector<mqtt::Message> got;
    void add(const mqtt::Message& m) { std::lock_guard<std::mutex> lk(mu); got.push_back(m); }
    bool has(const std::string& topic, const std::string& payload) {
        std::lock_guard<std::mutex> lk(mu);
        for (const auto& m : got) if (m.topic == topic && m.payload == payload) return true;
        return false;
    }
};

class MosquittoBrokerTest : public ::testing::Test {
protected:
    std::unique_ptr<TestBroker> broker_;

    void SetUp() override {
        if (auto why = TestBroker::unavailable()) GTEST_SKIP() << *why;
        broker_ = std::make_unique<TestBroker>();
        ASSERT_TRUE(broker_->ok()) << broker_->problem();
    }

    static std::string commandsOf(const TestBroker::Player& p) { return "liveqx/v1/players/" + p.id + "/commands"; }
    static std::string presenceOf(const TestBroker::Player& p) { return "liveqx/v1/players/" + p.id + "/presence"; }
};

}  // namespace

TEST_F(MosquittoBrokerTest, TheServerAndAPlayerConnectWithCertificatesHeldInMemory) {
    auto player = broker_->enrollPlayer();
    mqtt::MosquittoClient server(broker_->serverOptions());
    mqtt::MosquittoClient dev(broker_->playerOptions(player));
    ASSERT_TRUE(server.start()) << server.lastError();
    ASSERT_TRUE(dev.start()) << dev.lastError();
    EXPECT_TRUE(waitFor([&] { return server.connected(); })) << server.lastError() << "\n" << broker_->log();
    EXPECT_TRUE(waitFor([&] { return dev.connected(); })) << dev.lastError() << "\n" << broker_->log();
    server.stop();
    dev.stop();
}

TEST_F(MosquittoBrokerTest, ACommandReachesItsPlayerAndPresenceReachesTheServer) {
    auto player = broker_->enrollPlayer();
    Inbox at_server, at_player;
    mqtt::MosquittoClient server(broker_->serverOptions());
    mqtt::MosquittoClient dev(broker_->playerOptions(player));
    server.setMessageHandler([&](const mqtt::Message& m) { at_server.add(m); });
    dev.setMessageHandler([&](const mqtt::Message& m) { at_player.add(m); });
    server.subscribe("liveqx/v1/players/+/presence", 1);
    dev.subscribe(commandsOf(player), 1);
    ASSERT_TRUE(server.start());
    ASSERT_TRUE(dev.start());
    ASSERT_TRUE(waitFor([&] { return server.connected() && dev.connected(); })) << broker_->log();

    mqtt::Message cmd;
    cmd.topic = commandsOf(player);
    cmd.payload = R"({"cmd":"reboot"})";
    ASSERT_TRUE(waitFor([&] {                                   // the subscription may still be settling
        server.publish(cmd);
        return at_player.has(cmd.topic, cmd.payload);
    }));

    mqtt::Message pres;
    pres.topic = presenceOf(player);
    pres.payload = R"({"online":true})";
    pres.retain = true;
    ASSERT_EQ(dev.publish(pres), mqtt::PublishResult::Ok);
    EXPECT_TRUE(waitFor([&] { return at_server.has(pres.topic, pres.payload); }));
    server.stop();
    dev.stop();
}

TEST_F(MosquittoBrokerTest, APlayerDoesNotSeeAnotherPlayersCommandsAndCannotSendThemItself) {
    auto a = broker_->enrollPlayer();
    auto b = broker_->enrollPlayer();
    Inbox at_a;
    mqtt::MosquittoClient server(broker_->serverOptions());
    mqtt::MosquittoClient da(broker_->playerOptions(a));
    mqtt::MosquittoClient db(broker_->playerOptions(b));
    da.setMessageHandler([&](const mqtt::Message& m) { at_a.add(m); });
    da.subscribe(commandsOf(a), 1);
    da.subscribe(commandsOf(b), 1);                      // not allowed by the broker
    ASSERT_TRUE(server.start());
    ASSERT_TRUE(da.start());
    ASSERT_TRUE(db.start());
    ASSERT_TRUE(waitFor([&] { return server.connected() && da.connected() && db.connected(); })) << broker_->log();

    mqtt::Message to_b;
    to_b.topic = commandsOf(b);
    to_b.payload = "for-b";
    mqtt::Message to_a;
    to_a.topic = commandsOf(a);
    to_a.payload = "for-a";
    ASSERT_TRUE(waitFor([&] { server.publish(to_b); server.publish(to_a); return at_a.has(to_a.topic, "for-a"); }));
    EXPECT_FALSE(at_a.has(to_b.topic, "for-b")) << "the broker keeps another player's commands away";

    // A player publishing a command (to someone else) is dropped by the broker.
    mqtt::Message forged;
    forged.topic = commandsOf(a);
    forged.payload = "forged";
    db.publish(forged);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    EXPECT_FALSE(at_a.has(forged.topic, "forged"));
    server.stop();
    da.stop();
    db.stop();
}

TEST_F(MosquittoBrokerTest, TheBrokersNameIsCheckedUnlessTheAdministratorTurnsThatOff) {
    // A certificate that is valid for "localhost" only, reached by the address 127.0.0.1.
    TestBroker only_name({"localhost"}, {});
    ASSERT_TRUE(only_name.ok()) << only_name.problem();

    auto strict = only_name.serverOptions();
    strict.verify_hostname = true;
    mqtt::MosquittoClient refused(strict);
    ASSERT_TRUE(refused.start());
    EXPECT_FALSE(waitFor([&] { return refused.connected(); }, 3000)) << "the name does not match the certificate";
    EXPECT_FALSE(refused.lastError().empty());
    refused.stop();

    auto lax = only_name.serverOptions();
    lax.verify_hostname = false;
    mqtt::MosquittoClient accepted(lax);
    ASSERT_TRUE(accepted.start());
    EXPECT_TRUE(waitFor([&] { return accepted.connected(); })) << accepted.lastError();
    accepted.stop();
}

TEST_F(MosquittoBrokerTest, AfterTheBrokerRestartsTheClientComesBackAndSubscribesAgain) {
    auto player = broker_->enrollPlayer();
    Inbox at_player;
    mqtt::MosquittoClient server(broker_->serverOptions());
    mqtt::MosquittoClient dev(broker_->playerOptions(player));
    dev.setMessageHandler([&](const mqtt::Message& m) { at_player.add(m); });
    dev.subscribe(commandsOf(player), 1);
    ASSERT_TRUE(server.start());
    ASSERT_TRUE(dev.start());
    ASSERT_TRUE(waitFor([&] { return server.connected() && dev.connected(); })) << broker_->log();

    broker_->stop();
    EXPECT_TRUE(waitFor([&] { return !dev.connected() && !server.connected(); }));
    mqtt::Message cmd;
    cmd.topic = commandsOf(player);
    cmd.payload = "after-restart";
    EXPECT_EQ(server.publish(cmd), mqtt::PublishResult::NotConnected) << "with no broker, publishing says so";

    ASSERT_TRUE(broker_->start());
    ASSERT_TRUE(waitFor([&] { return server.connected() && dev.connected(); }, 15000)) << broker_->log();
    ASSERT_TRUE(waitFor([&] { server.publish(cmd); return at_player.has(cmd.topic, cmd.payload); }, 8000))
        << "the subscription was renewed without anyone asking";
    server.stop();
    dev.stop();
}

TEST_F(MosquittoBrokerTest, APersistentSessionKeepsAMessageForAnOfflineClientAndDropsAnExpiredOne) {
    auto player = broker_->enrollPlayer();
    mqtt::MosquittoClient server(broker_->serverOptions());
    ASSERT_TRUE(server.start());
    ASSERT_TRUE(waitFor([&] { return server.connected(); }));

    auto opts = broker_->playerOptions(player);
    opts.clean_start = false;
    opts.session_expiry_s = 300;
    {
        // First visit: subscribe so that the broker keeps a session for this player, then leave.
        mqtt::MosquittoClient dev(opts);
        dev.subscribe(commandsOf(player), 1);
        ASSERT_TRUE(dev.start());
        ASSERT_TRUE(waitFor([&] { return dev.connected(); })) << broker_->log();
        std::this_thread::sleep_for(std::chrono::milliseconds(500));   // let the subscription land
        dev.stop();
    }

    mqtt::Message lasting;
    lasting.topic = commandsOf(player);
    lasting.payload = "long-lived";
    lasting.expiry_s = 300;
    mqtt::Message brief = lasting;
    brief.payload = "short-lived";
    brief.expiry_s = 1;
    ASSERT_EQ(server.publish(lasting), mqtt::PublishResult::Ok);
    ASSERT_EQ(server.publish(brief), mqtt::PublishResult::Ok);
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));      // the short one runs out

    Inbox at_player;
    mqtt::MosquittoClient back(opts);
    back.setMessageHandler([&](const mqtt::Message& m) { at_player.add(m); });
    back.subscribe(commandsOf(player), 1);
    ASSERT_TRUE(back.start());
    EXPECT_TRUE(waitFor([&] { return at_player.has(lasting.topic, "long-lived"); }))
        << "the message waited for the player to return";
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_FALSE(at_player.has(lasting.topic, "short-lived")) << "an expired command is not delivered late";
    back.stop();
    server.stop();
}

TEST_F(MosquittoBrokerTest, APlayerTheBrokerDoesNotKnowMayConnectButHasNoRights) {
    // Dynamic Security checks rights, not existence: a valid certificate gets a
    // connection, and with no client record behind it nothing is allowed.
    auto stranger = broker_->enrollPlayer(/*provision=*/false);
    Inbox at_server, at_stranger;
    mqtt::MosquittoClient server(broker_->serverOptions());
    mqtt::MosquittoClient dev(broker_->playerOptions(stranger));
    server.setMessageHandler([&](const mqtt::Message& m) { at_server.add(m); });
    dev.setMessageHandler([&](const mqtt::Message& m) { at_stranger.add(m); });
    server.subscribe("liveqx/v1/players/+/presence", 1);
    dev.subscribe(commandsOf(stranger), 1);
    dev.subscribe("liveqx/v1/broadcast/commands", 1);
    ASSERT_TRUE(server.start());
    ASSERT_TRUE(dev.start());
    ASSERT_TRUE(waitFor([&] { return server.connected() && dev.connected(); })) << broker_->log();

    mqtt::Message cmd;
    cmd.topic = commandsOf(stranger);
    cmd.payload = "to-stranger";
    mqtt::Message all;
    all.topic = "liveqx/v1/broadcast/commands";
    all.payload = "to-everyone";
    mqtt::Message pres;
    pres.topic = presenceOf(stranger);
    pres.payload = "i-am-here";
    for (int i = 0; i < 5; ++i) {
        server.publish(cmd);
        server.publish(all);
        dev.publish(pres);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    EXPECT_FALSE(at_stranger.has(cmd.topic, cmd.payload)) << "no personal commands without a record";
    EXPECT_FALSE(at_stranger.has(all.topic, all.payload)) << "no broadcast without a record";
    EXPECT_FALSE(at_server.has(pres.topic, pres.payload)) << "no presence without a record";
    server.stop();
    dev.stop();
}

namespace {

// The server's connection with the administrator wired in, as main will do it.
struct AdminRig {
    mqtt::MosquittoClient client;
    mqtt::BrokerAdmin     admin;
    AdminRig(const mqtt::ClientOptions& o) : client(o), admin(client) {
        client.setMessageHandler([this](const mqtt::Message& m) { admin.handleMessage(m); });
    }
    bool up() {
        if (!client.start()) return false;
        return waitFor([&] { return client.connected(); });
    }
};

}  // namespace

TEST_F(MosquittoBrokerTest, TheServerIssuesAPlayerThroughDynamicSecurityAndThePlayerWorks) {
    AdminRig rig(broker_->serverOptions());
    ASSERT_TRUE(rig.up()) << broker_->log();
    auto base = rig.admin.ensureBaseline();
    ASSERT_TRUE(base.ok) << base.error << " — the real plugin's wording for 'already there' is accepted";

    auto player = broker_->enrollPlayer(/*provision=*/false);
    auto issued = rig.admin.provisionPlayer(player.id);
    ASSERT_TRUE(issued.ok) << issued.error;
    EXPECT_TRUE(rig.admin.provisionPlayer(player.id).ok) << "twice is the same as once";

    Inbox at_player;
    mqtt::MosquittoClient dev(broker_->playerOptions(player));
    dev.setMessageHandler([&](const mqtt::Message& m) { at_player.add(m); });
    dev.subscribe(commandsOf(player), 1);
    ASSERT_TRUE(dev.start());
    ASSERT_TRUE(waitFor([&] { return dev.connected(); })) << broker_->log();
    mqtt::Message cmd;
    cmd.topic = commandsOf(player);
    cmd.payload = "issued-by-server";
    EXPECT_TRUE(waitFor([&] { rig.client.publish(cmd); return at_player.has(cmd.topic, cmd.payload); }));
    dev.stop();
}

TEST_F(MosquittoBrokerTest, RemovingAPlayerClosesItsRightsAtOnce) {
    AdminRig rig(broker_->serverOptions());
    ASSERT_TRUE(rig.up());
    auto player = broker_->enrollPlayer(/*provision=*/false);
    ASSERT_TRUE(rig.admin.provisionPlayer(player.id).ok);

    Inbox at_player;
    mqtt::MosquittoClient dev(broker_->playerOptions(player));
    dev.setMessageHandler([&](const mqtt::Message& m) { at_player.add(m); });
    dev.subscribe(commandsOf(player), 1);
    ASSERT_TRUE(dev.start());
    ASSERT_TRUE(waitFor([&] { return dev.connected(); }));
    mqtt::Message before;
    before.topic = commandsOf(player);
    before.payload = "before";
    ASSERT_TRUE(waitFor([&] { rig.client.publish(before); return at_player.has(before.topic, "before"); }));

    ASSERT_TRUE(rig.admin.removePlayer(player.id).ok);
    EXPECT_TRUE(rig.admin.removePlayer(player.id).ok) << "removing again is fine";
    mqtt::Message after = before;
    after.payload = "after";
    for (int i = 0; i < 5; ++i) {
        rig.client.publish(after);
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
    EXPECT_FALSE(at_player.has(after.topic, "after")) << "nothing reaches a removed player";
    dev.stop();
}

TEST_F(MosquittoBrokerTest, ReconcileBringsTheRealBrokerInLineWithTheWantedList) {
    AdminRig rig(broker_->serverOptions());
    ASSERT_TRUE(rig.up());
    auto keep = broker_->enrollPlayer(/*provision=*/true);     // issued by hand, as if earlier
    auto drop = broker_->enrollPlayer(/*provision=*/true);     // no longer wanted
    auto fresh = broker_->enrollPlayer(/*provision=*/false);   // wanted, never issued

    auto rep = rig.admin.reconcile({keep.id, fresh.id});
    EXPECT_TRUE(rep.ok) << rep.error;
    EXPECT_EQ(rep.issued, 1u);
    EXPECT_EQ(rep.removed, 1u);

    std::set<std::string> clients, roles;
    ASSERT_TRUE(rig.admin.listClients(clients).ok);
    ASSERT_TRUE(rig.admin.listRoles(roles).ok);
    EXPECT_TRUE(clients.count(keep.id));
    EXPECT_TRUE(clients.count(fresh.id));
    EXPECT_FALSE(clients.count(drop.id));
    EXPECT_FALSE(roles.count("player-" + drop.id));
    EXPECT_TRUE(clients.count("liveqx-server")) << "the server's own client is never touched";
    EXPECT_TRUE(roles.count("player-common"));

    auto again = rig.admin.reconcile({keep.id, fresh.id});
    EXPECT_TRUE(again.ok);
    EXPECT_EQ(again.issued + again.removed, 0u) << "a second pass finds nothing to do";
}

TEST_F(MosquittoBrokerTest, TheLinkIssuesAPlayerAndHearsItsPresenceEndToEnd) {
    auto player = broker_->enrollPlayer(/*provision=*/false);
    const auto server = broker_->serverOptions();

    std::mutex mu;
    std::vector<std::pair<std::string, std::string>> heard;
    mqtt::BrokerLinkOptions o;
    o.host = server.host;
    o.port = server.port;
    o.retry_s = 1;
    o.reconcile_interval_s = 30;
    o.identity = [&](mqtt::BrokerIdentityPem& id, std::string&) {
        id.cert_chain_pem = server.cert_chain_pem;
        id.key_pem = server.key_pem;
        id.serial = "s1";
        return true;
    };
    o.trust_pem = [&] { return server.ca_pem; };
    o.wanted = [&] { return std::set<std::string>{player.id}; };
    o.make_client = [](const mqtt::ClientOptions& co) -> std::unique_ptr<mqtt::IMqttClient> {
        return std::make_unique<mqtt::MosquittoClient>(co);
    };
    o.subscriptions = {"liveqx/v1/players/+/presence"};
    o.on_message = [&](const mqtt::Message& m) {
        std::lock_guard<std::mutex> lk(mu);
        heard.push_back({m.topic, m.payload});
    };
    mqtt::BrokerLink link(std::move(o));
    link.start();
    ASSERT_TRUE(waitFor([&] { return link.status().last_reconcile_ok; }, 20000))
        << link.status().detail << "\n" << broker_->log();
    EXPECT_EQ(link.status().last_report.issued, 1u) << "the link gave the new player its place on the broker";

    // The player connects and announces itself, as the real one does.
    mqtt::MosquittoClient dev(broker_->playerOptions(player));
    ASSERT_TRUE(dev.start());
    ASSERT_TRUE(waitFor([&] { return dev.connected(); })) << broker_->log();
    mqtt::Message hello;
    hello.topic = presenceOf(player);
    hello.payload = "online";
    hello.retain = true;
    ASSERT_EQ(dev.publish(hello), mqtt::PublishResult::Ok);
    EXPECT_TRUE(waitFor([&] {
        std::lock_guard<std::mutex> lk(mu);
        for (const auto& h : heard) if (h.first == hello.topic && h.second == "online") return true;
        return false;
    })) << "the server hears it";
    dev.stop();
    link.stop();
}

TEST_F(MosquittoBrokerTest, RemovingAPlayerLeavesNoRetainedPresenceBehindAndStaleOnesAreSwept) {
    AdminRig rig(broker_->serverOptions());
    ASSERT_TRUE(rig.up());
    ASSERT_TRUE(rig.admin.ensureBaseline().ok);
    auto gone = broker_->enrollPlayer(/*provision=*/true);
    auto stays = broker_->enrollPlayer(/*provision=*/true);
    for (const auto* p : {&gone, &stays}) {
        mqtt::MosquittoClient dev(broker_->playerOptions(*p));
        ASSERT_TRUE(dev.start());
        ASSERT_TRUE(waitFor([&] { return dev.connected(); })) << broker_->log();
        mqtt::Message hello;
        hello.topic = presenceOf(*p);
        hello.payload = "online";
        hello.retain = true;
        ASSERT_EQ(dev.publish(hello), mqtt::PublishResult::Ok);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        dev.stop();
    }

    // What a late subscriber is told: only the retained messages that still exist.
    auto retainedSeenBy = [&](const std::string& who) {
        auto o = broker_->serverOptions();
        o.client_id = who;
        mqtt::MosquittoClient probe(o);
        Inbox seen;
        probe.setMessageHandler([&](const mqtt::Message& m) { seen.add(m); });
        probe.subscribe("liveqx/v1/players/+/presence", 1);
        EXPECT_TRUE(probe.start());
        EXPECT_TRUE(waitFor([&] { return probe.connected(); }));
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
        probe.stop();
        std::lock_guard<std::mutex> lk(seen.mu);
        std::set<std::string> topics;
        for (const auto& m : seen.got) if (!m.payload.empty()) topics.insert(m.topic);
        return topics;
    };
    auto before = retainedSeenBy("probe-1");
    ASSERT_TRUE(before.count(presenceOf(gone)));
    ASSERT_TRUE(before.count(presenceOf(stays)));

    ASSERT_TRUE(rig.admin.removePlayer(gone.id).ok);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    auto after = retainedSeenBy("probe-2");
    EXPECT_FALSE(after.count(presenceOf(gone))) << "its retained presence is gone with it";
    EXPECT_TRUE(after.count(presenceOf(stays)));
}

TEST_F(MosquittoBrokerTest, ARevokedPlayerIsCutOffAtOnceAndWhateverItDoesNextReachesNothing) {
    // The revoked player's record is removed on the broker (what the server does on a
    // revocation). This pins down what that alone achieves, without any CRL.
    AdminRig rig(broker_->serverOptions());
    ASSERT_TRUE(rig.up());
    ASSERT_TRUE(rig.admin.ensureBaseline().ok);
    auto player = broker_->enrollPlayer(/*provision=*/false);
    ASSERT_TRUE(rig.admin.provisionPlayer(player.id).ok);

    Inbox at_player, at_server;
    std::atomic<int> drops{0};
    mqtt::MosquittoClient dev(broker_->playerOptions(player));
    dev.setMessageHandler([&](const mqtt::Message& m) { at_player.add(m); });
    dev.setStateHandler([&](bool up, const std::string&) { if (!up) ++drops; });
    dev.subscribe(commandsOf(player), 1);
    ASSERT_TRUE(dev.start());
    ASSERT_TRUE(waitFor([&] { return dev.connected(); })) << broker_->log();
    mqtt::Message cmd;
    cmd.topic = commandsOf(player);
    cmd.payload = "before";
    ASSERT_TRUE(waitFor([&] { rig.client.publish(cmd); return at_player.has(cmd.topic, "before"); }));

    ASSERT_TRUE(rig.admin.removePlayer(player.id).ok);

    EXPECT_TRUE(waitFor([&] { return drops.load() >= 1; })) << "the open connection is dropped, not left to idle";

    // It will try again (a player does); the broker lets it in (the certificate is
    // still valid) but it has no record, so nothing reaches it and nothing it sends is heard.
    rig.client.setMessageHandler([&](const mqtt::Message& m) { at_server.add(m); rig.admin.handleMessage(m); });
    cmd.payload = "after";
    mqtt::Message hello;
    hello.topic = presenceOf(player);
    hello.payload = "online";
    for (int i = 0; i < 8; ++i) {
        rig.client.publish(cmd);
        dev.publish(hello);
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    EXPECT_FALSE(at_player.has(cmd.topic, "after")) << "no commands for a revoked player";
}
