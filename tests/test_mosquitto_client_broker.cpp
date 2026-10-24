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
#include "mqtt/BrokerReload.h"
#include "players/BrokerTls.h"
#include "players/CrlPublisher.h"
#include "players/PlayersDb.h"
#include "mqtt/MosquittoClient.h"

#include <gtest/gtest.h>

#include <ctime>
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

TEST_F(MosquittoBrokerTest, AfterTheBrokerIsKilledOutrightAndStartedAgainTheClientComesBack) {
    mqtt::MosquittoClient server(broker_->serverOptions());
    ASSERT_TRUE(server.start());
    ASSERT_TRUE(waitFor([&] { return server.connected(); })) << broker_->log();

    broker_->crash();
    EXPECT_TRUE(waitFor([&] { return !server.connected(); }));
    std::this_thread::sleep_for(std::chrono::seconds(3));        // a few attempts fail while it is down
    ASSERT_TRUE(broker_->start());
    EXPECT_TRUE(waitFor([&] { return server.connected(); }, 40000)) << server.lastError() << "\n" << broker_->log();
    server.stop();
}

TEST_F(MosquittoBrokerTest, RereadingItsSettingsWhileASessionWaitsForItsClientDoesNotKillTheBrokerWhenTheSessionExpires) {
    // Mosquitto before 2.0.23 "disconnects" every offline persistent session again at a reload, which puts
    // it in its expiry list twice and, once the session expires, kills the broker (use after free). The
    // server rereads the broker's settings after every revocation list it writes, and screens that are
    // switched off keep their sessions on purpose, so this must be survived.
    if (!liveqx::testing::brokerIsOurBuild())
        GTEST_SKIP() << "needs a broker of 2.0.23 or newer: packaging/mosquitto/build.sh";
    auto player = broker_->enrollPlayer();
    auto opts = broker_->playerOptions(player);
    opts.session_expiry_s = 3;                           // short, so the expiry happens inside the test
    mqtt::MosquittoClient dev(opts);
    dev.subscribe(commandsOf(player), 1);
    ASSERT_TRUE(dev.start());
    ASSERT_TRUE(waitFor([&] { return dev.connected(); })) << broker_->log();
    dev.stop();                                          // the screen is switched off; its session stays for now

    for (int i = 0; i < 3; ++i) {
        broker_->reload();
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
    // The session runs out (3 s, plus the broker's own check interval). Then the list is walked.
    std::this_thread::sleep_for(std::chrono::seconds(8));

    mqtt::MosquittoClient server(broker_->serverOptions());
    ASSERT_TRUE(server.start());
    EXPECT_TRUE(waitFor([&] { return server.connected(); })) << "the broker is not answering after the session expired\n" << broker_->log();
    server.stop();
}

TEST(MosquittoBrokerCertificate, ARenewedCertificateOfTheBrokerIsServedAfterAReloadWithoutARestart) {
    // The server renews the broker's certificate 30 days before its end and then tells the broker to
    // reread its settings (SIGHUP). That only works while the broker is set to check a revocation list
    // (as it always is in production): then a reload also rebuilds the listener's TLS context.
    if (auto why = TestBroker::unavailable()) GTEST_SKIP() << *why;
    TestBroker broker({"localhost"}, {"127.0.0.1"}, /*with_crl=*/true);
    ASSERT_TRUE(broker.ok()) << broker.problem();
    const std::string before = liveqx::testing::servedCertSerial(broker.port());
    ASSERT_FALSE(before.empty()) << broker.log();

    liveqx::tls::SanList sans;
    sans.dns_names = {"localhost"};
    sans.ip_v4 = {"127.0.0.1"};
    ASSERT_TRUE(liveqx::tls::issueServerCert(broker.brokerCertDir(), "localhost", sans).ok());
    EXPECT_EQ(liveqx::testing::servedCertSerial(broker.port()), before) << "the file alone changes nothing";

    broker.reload();
    EXPECT_TRUE(waitFor([&] {
        const std::string now = liveqx::testing::servedCertSerial(broker.port());
        return !now.empty() && now != before;
    })) << "the broker still shows the old certificate after the reload\n" << broker.log();
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

TEST_F(MosquittoBrokerTest, AFreshBrokerIsSetUpWithoutCuttingTheServersOwnConnection) {
    // A fresh broker knows the server only as its administrator. Giving the server its working role
    // makes the broker drop the connections of that user, so it must happen before the standing
    // connection exists, not on it.
    TestBroker bare({"localhost"}, {"127.0.0.1"}, /*with_crl=*/false, /*bare=*/true);
    ASSERT_TRUE(bare.ok()) << bare.problem();
    const auto server = bare.serverOptions();
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
    o.wanted = [] { return std::set<std::string>{}; };
    o.make_client = [](const mqtt::ClientOptions& co) -> std::unique_ptr<mqtt::IMqttClient> {
        return std::make_unique<mqtt::MosquittoClient>(co);
    };
    mqtt::BrokerLink link(std::move(o));
    link.start();
    ASSERT_TRUE(waitFor([&] { return link.status().last_reconcile_ok; }, 20000))
        << link.status().detail << "\n" << bare.log();
    std::this_thread::sleep_for(std::chrono::seconds(1));          // a late drop would show by now
    link.stop();

    const std::string log = bare.log();
    auto count = [&](const std::string& what) {
        std::size_t n = 0;
        for (auto at = log.find(what); at != std::string::npos; at = log.find(what, at + 1)) ++n;
        return n;
    };
    EXPECT_EQ(count("Client liveqx-server been disconnected by administrative action"), 0u) << log;
    EXPECT_EQ(count("as liveqx-server (p5, c0"), 1u) << "the standing connection was opened once and never had to come back\n" << log;
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

TEST_F(MosquittoBrokerTest, RebuildingAPlayersRecordEndsItsOpenConnectionAndLeavesAWorkingRecordForTheNextDevice) {
    // A screen bound again keeps its place on the broker, so a revoked certificate alone would
    // leave the old device's open connection alive (the broker checks the list at the handshake).
    // Dropping and making the record again is what ends it.
    AdminRig rig(broker_->serverOptions());
    ASSERT_TRUE(rig.up());
    ASSERT_TRUE(rig.admin.ensureBaseline().ok);
    auto player = broker_->enrollPlayer(/*provision=*/false);
    ASSERT_TRUE(rig.admin.provisionPlayer(player.id).ok);

    Inbox at_player;
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

    const auto rep = rig.admin.reconcile({player.id}, {player.id});
    ASSERT_TRUE(rep.ok) << rep.error;
    EXPECT_EQ(rep.issued, 1u);

    EXPECT_TRUE(waitFor([&] { return drops.load() >= 1; })) << "the old connection is dropped, not left to idle";

    // The record is there again: a device that connects now (the new one, with a valid
    // certificate) gets commands as any other.
    cmd.payload = "after";
    EXPECT_TRUE(waitFor([&] {
        rig.client.publish(cmd);
        return at_player.has(cmd.topic, "after");
    }, 15000)) << "the rebuilt record gives the next device its rights\n" << broker_->log();
}

// ── the revocation list, against a real broker ──────────────────────────────
namespace {

// Players that exist in a database AND have certificates the broker knows, as after pairing.
struct RevocationRig {
    std::filesystem::path dir;
    std::unique_ptr<liveqx::players::PlayersDb> db;
    std::unique_ptr<TestBroker> broker;

    explicit RevocationRig() {
        static std::atomic<int> seq{0};
        dir = std::filesystem::temp_directory_path() /
              ("lqx-revrig-" + std::to_string(::getpid()) + "-" + std::to_string(seq.fetch_add(1)));
        std::filesystem::create_directories(dir);
        db = std::make_unique<liveqx::players::PlayersDb>(dir / "players.db");
        db->open();
        broker = std::make_unique<TestBroker>(std::vector<std::string>{"localhost"}, std::vector<std::string>{"127.0.0.1"},
                                              /*with_crl=*/true);
    }
    ~RevocationRig() {
        broker.reset();
        db.reset();
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    TestBroker::Player pair(const std::string& name) {
        const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
        auto created = db->createPlayer(name, std::nullopt, "strict", 86400, now);
        auto player = broker->enrollPlayerAs(created->player.id, /*provision=*/true);
        auto claim = db->claimToken(player.id, created->token, now);
        liveqx::players::CertRow row;
        row.serial_hex = player.serial_hex; row.generation = player.generation; row.player_id = player.id;
        row.issued_at = now; row.not_before = player.not_before; row.not_after = player.not_after;
        db->finishEnrollment(claim.token_id, row, now);
        return player;
    }

    liveqx::players::CrlPublisherOptions publisher(std::function<std::int64_t()> now = nullptr, int days = 30) {
        liveqx::players::CrlPublisherOptions o;
        o.path = broker->crlFile();
        o.next_update_days = days;
        o.now = std::move(now);
        auto ca_dir = broker->caDir();
        auto* key = &broker->masterKey();
        o.generations = [ca_dir] { return liveqx::tls::DeviceCa::generations(ca_dir); };
        o.open_generation = [ca_dir, key](int g, std::string& err) { return liveqx::tls::DeviceCa::openGeneration(ca_dir, *key, g, err); };
        auto pid = broker->pidFile();
        o.reload = [pid] { return mqtt::reloadBrokerFromPidFile(pid); };
        return o;
    }
};

bool connectsWithin(const mqtt::ClientOptions& o, int ms) {
    mqtt::MosquittoClient c(o);
    if (!c.start()) return false;
    const bool up = waitFor([&] { return c.connected(); }, ms);
    c.stop();
    return up;
}

}  // namespace

TEST(MosquittoRevocation, ARevokedCertificateIsTurnedAwayAtTheHandshakeAndOthersAreNotAfterTheBrokerIsToldToReread) {
    if (auto why = TestBroker::unavailable()) GTEST_SKIP() << *why;
    RevocationRig rig;
    ASSERT_TRUE(rig.broker->ok()) << rig.broker->problem();
    auto a = rig.pair("screen-a");
    auto b = rig.pair("screen-b");

    EXPECT_TRUE(connectsWithin(rig.broker->playerOptions(a), 8000)) << "before: both come in";
    EXPECT_TRUE(connectsWithin(rig.broker->playerOptions(b), 8000));

    ASSERT_TRUE(rig.db->revokePlayer(a.id, "stolen").has_value());
    liveqx::players::CrlPublisher pub(*rig.db, rig.publisher());
    auto st = pub.publishNow();
    ASSERT_TRUE(st.last_ok) << st.error;
    EXPECT_EQ(st.revoked, 1u);
    ASSERT_TRUE(st.reload_error.empty()) << "the broker was told: " << st.reload_error;
    std::this_thread::sleep_for(std::chrono::milliseconds(800));          // the broker handles the hangup

    EXPECT_FALSE(connectsWithin(rig.broker->playerOptions(a), 3000)) << "the revoked certificate does not get through";
    EXPECT_TRUE(connectsWithin(rig.broker->playerOptions(b), 8000)) << "the others still do";
    EXPECT_TRUE(connectsWithin(rig.broker->serverOptions(), 8000)) << "and so does the server itself";
}

TEST(MosquittoRevocation, AListThatHasRunOutShutsEveryoneOutAndAFreshOneRepairsItWhichIsWhyItIsRefreshed) {
    if (auto why = TestBroker::unavailable()) GTEST_SKIP() << *why;
    RevocationRig rig;
    ASSERT_TRUE(rig.broker->ok()) << rig.broker->problem();
    auto b = rig.pair("screen-b");
    ASSERT_TRUE(connectsWithin(rig.broker->playerOptions(b), 8000));

    // A list published three days ago that was good for two.
    const std::int64_t real_now = static_cast<std::int64_t>(std::time(nullptr));
    {
        liveqx::players::CrlPublisher stale(*rig.db, rig.publisher([=] { return real_now - 3 * 86400; }, 2));
        ASSERT_TRUE(stale.publishNow().last_ok);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    EXPECT_FALSE(connectsWithin(rig.broker->playerOptions(b), 3000)) << "an expired list: nobody gets in";

    liveqx::players::CrlPublisher fresh(*rig.db, rig.publisher());
    ASSERT_TRUE(fresh.publishNow().last_ok);
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    EXPECT_TRUE(connectsWithin(rig.broker->playerOptions(b), 8000)) << "a new list and the hangup bring everyone back";
}

// ── The configuration that the installer ships, with the files the server writes ──

TEST(MosquittoShippedConfig, TheBrokerStartsOnTheInstallersTemplateAndOnlyWhatTheServerWrites) {
    if (auto why = TestBroker::unavailable()) GTEST_SKIP() << *why;
    namespace fs = std::filesystem;
    static std::atomic<int> seq{0};
    const fs::path dir = fs::temp_directory_path() /
                         ("lqx-shipped-" + std::to_string(::getpid()) + "-" + std::to_string(seq.fetch_add(1)));
    fs::create_directories(dir);
    struct Cleanup {
        fs::path d; pid_t pid{-1};
        ~Cleanup() {
            if (pid > 0) { ::kill(pid, SIGKILL); int st = 0; ::waitpid(pid, &st, 0); }
            std::error_code ec; fs::remove_all(d, ec);
        }
    } cleanup{dir};

    // What the server has: a master key, a device CA, the server's TLS CA, the players' database.
    liveqx::auth::MasterKey mk((dir / "master.key").string());
    ASSERT_TRUE(mk.load());
    std::string err;
    auto device_ca = liveqx::tls::DeviceCa::open(dir / "device_ca", mk, err, /*allow_create=*/true);
    ASSERT_TRUE(device_ca) << err;
    ASSERT_TRUE(liveqx::tls::ensureCa(dir / "tls").ok());
    liveqx::players::PlayersDb db(dir / "players.db");
    ASSERT_TRUE(db.open());

    // The server writes the broker's files: the list, the CA file, the certificate, then `ready`.
    const fs::path broker_dir = dir / "state" / "broker";
    liveqx::players::BrokerTlsOptions bt;
    bt.dir = broker_dir;
    bt.generations = [&] { return liveqx::tls::DeviceCa::generations(dir / "device_ca"); };
    bt.open_generation = [&](int g, std::string& e) { return liveqx::tls::DeviceCa::openGeneration(dir / "device_ca", mk, g, e); };
    bt.server_ca_dir = dir / "tls";
    bt.trust_root_pem = [&] { return liveqx::testing::readFileText(dir / "tls" / "ca.crt"); };
    bt.host = [] { return std::string("127.0.0.1"); };
    liveqx::players::BrokerTls broker_tls(std::move(bt));
    liveqx::players::CrlPublisherOptions co;
    co.path = broker_dir / "crl.pem";
    co.ready_path = broker_dir / "ready";
    co.generations = [&] { return liveqx::tls::DeviceCa::generations(dir / "device_ca"); };
    co.open_generation = [&](int g, std::string& e) { return liveqx::tls::DeviceCa::openGeneration(dir / "device_ca", mk, g, e); };
    co.prepare = [&](bool& changed) { return broker_tls.ensure(changed); };
    liveqx::players::CrlPublisher crl(db, std::move(co));
    const auto published = crl.publishNow();
    ASSERT_TRUE(published.last_ok) << published.error;
    ASSERT_EQ(published.prepare_error, "");
    ASSERT_TRUE(fs::exists(broker_dir / "ready"));

    // The installer: the template with its places filled in, and `dynsec init` for the administrator.
    const fs::path tmpl = fs::path(__FILE__).parent_path().parent_path() / "packaging" / "mosquitto" / "mosquitto.conf.in";
    std::string conf = liveqx::testing::readFileText(tmpl);
    ASSERT_FALSE(conf.empty()) << tmpl;
    const int port = liveqx::testing::freeLoopbackPort();
    const fs::path run_dir = dir / "run";
    fs::create_directories(run_dir);
    fs::create_directories(broker_dir / "data");
    auto fill = [&](const std::string& what, const std::string& with) {
        for (auto at = conf.find(what); at != std::string::npos; at = conf.find(what, at + with.size()))
            conf.replace(at, what.size(), with);
    };
    fill("@BROKER_DIR@", broker_dir.string());
    fill("@RUN_DIR@", run_dir.string());
    fill("@PORT@", std::to_string(port));
    fill("@PLUGIN@", liveqx::testing::findDynsecPlugin()->string());
    EXPECT_EQ(conf.find('@'), std::string::npos) << "every place in the template was filled in";
    liveqx::testing::writeFileText(dir / "mosquitto.conf", conf, 0644);
    const auto init = liveqx::testing::runProcess({liveqx::testing::ctrlBinary()->string(), "dynsec", "init",
                                                   (broker_dir / "dynamic-security.json").string(),
                                                   "liveqx-server", "unused-password"}, dir);
    ASSERT_EQ(init.code, 0) << init.output;

    // Start the broker the way the service does, and see it come up.
    std::vector<std::string> argv = {liveqx::testing::brokerBinary()->string(), "-c", (dir / "mosquitto.conf").string()};
    std::vector<char*> args;
    for (auto& a : argv) args.push_back(a.data());
    args.push_back(nullptr);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, (dir / "broker.out").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    posix_spawn_file_actions_adddup2(&fa, 1, 2);
    ASSERT_EQ(posix_spawn(&cleanup.pid, argv[0].c_str(), &fa, nullptr, args.data(), environ), 0);
    posix_spawn_file_actions_destroy(&fa);
    ASSERT_TRUE(waitFor([&] { return liveqx::testing::portAccepts(port); }, 8000)) << liveqx::testing::readFileText(dir / "broker.out");

    // The server's standing connection (the real link) checks the broker's name against the certificate
    // the server wrote, and gives the player its place on the broker.
    liveqx::tls::BrokerIdentity identity;
    ASSERT_TRUE(liveqx::tls::DeviceCa::ensureBrokerIdentity(dir / "device_ca", mk, *device_ca, identity, err)) << err;
    const std::string server_root = liveqx::testing::readFileText(dir / "tls" / "ca.crt");
    const std::string player_id = liveqx::testing::randomUuidV4();
    mqtt::BrokerLinkOptions lo;
    lo.host = "127.0.0.1";
    lo.port = port;
    lo.retry_s = 1;
    lo.reconcile_interval_s = 30;
    lo.identity = [&](mqtt::BrokerIdentityPem& id, std::string&) {
        id.cert_chain_pem = identity.cert_chain_pem;
        id.key_pem = identity.key_pem;
        id.serial = identity.serial_hex;
        return true;
    };
    lo.trust_pem = [&] { return server_root; };
    lo.wanted = [&] { return std::set<std::string>{player_id}; };
    lo.make_client = [](const mqtt::ClientOptions& co) -> std::unique_ptr<mqtt::IMqttClient> {
        return std::make_unique<mqtt::MosquittoClient>(co);
    };
    mqtt::BrokerLink link(std::move(lo));
    link.start();
    ASSERT_TRUE(waitFor([&] { return link.status().last_reconcile_ok; }, 20000))
        << link.status().detail << "\n" << liveqx::testing::readFileText(dir / "broker.out");
    EXPECT_EQ(link.status().last_report.issued, 1u);

    // A screen as the player makes it: the certificate its CSR got, the session it asks for
    // (persistent, never expiring on its own: the broker's `persistent_client_expiration` rules).
    std::string player_key, cert_err;
    const std::string csr = liveqx::testing::makeCsrAndKey(player_key);
    liveqx::tls::IssuedClientCert player_cert;
    ASSERT_TRUE(device_ca->signClientCsr(csr, player_id, 365, player_cert, cert_err)) << cert_err;
    mqtt::ClientOptions po;
    po.host = "127.0.0.1";
    po.port = port;
    po.client_id = player_id;
    po.ca_pem = server_root;
    po.cert_chain_pem = player_cert.cert_pem + player_cert.chain_pem;
    po.key_pem = player_key;
    po.clean_start = false;
    po.session_expiry_s = 0xFFFFFFFFu;
    const std::string topic = "liveqx/v1/players/" + player_id + "/commands";
    {
        mqtt::MosquittoClient screen(po);
        screen.subscribe(topic, 1);
        ASSERT_TRUE(screen.start());
        ASSERT_TRUE(waitFor([&] { return screen.connected(); }, 10000)) << screen.lastError();
        screen.stop();                                              // the screen is switched off
    }

    // The server reloads the broker's files, as it does after every new list, while the screen is away.
    EXPECT_EQ(mqtt::reloadBrokerFromPidFile(run_dir / "mosquitto.pid"), "");
    std::this_thread::sleep_for(std::chrono::seconds(1));
    EXPECT_EQ(link.status().state, "connected") << "a reload does not cut the open connections";

    // A command sent meanwhile waits for the screen and is there when it comes back.
    mqtt::Message cmd;
    cmd.topic = topic;
    cmd.payload = "restart_player";
    cmd.expiry_s = 300;
    ASSERT_EQ(link.publish(cmd), mqtt::PublishResult::Ok);
    std::mutex got_mu;
    std::vector<std::string> got;
    mqtt::MosquittoClient back(po);
    back.setMessageHandler([&](const mqtt::Message& m) { std::lock_guard<std::mutex> lk(got_mu); got.push_back(m.payload); });
    back.subscribe(topic, 1);
    ASSERT_TRUE(back.start());
    EXPECT_TRUE(waitFor([&] { std::lock_guard<std::mutex> lk(got_mu); return !got.empty(); }, 8000))
        << "the command queued for the switched-off screen arrived when it returned";
    back.stop();
    link.stop();
}
