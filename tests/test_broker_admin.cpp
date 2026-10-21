// mqtt/BrokerAdmin — the server administering the broker through Dynamic
// Security, tested against an in-memory stand-in (the live plugin is covered in
// test_mosquitto_client_broker.cpp).
//
// What these tests protect:
//   * the command lists give a player exactly its own topics plus the shared
//     role — nothing wider, never a wildcard;
//   * issuing and removing are idempotent: doing either twice is fine, a real
//     refusal is reported, and a missing shared role is NOT swallowed as
//     "already exists";
//   * a broker that is down or silent is an answer ("not connected", "no
//     reply"), not a hang and not a crash;
//   * reconciliation issues what is missing, removes player entries nobody
//     owns, and never touches the server's own client or the shared roles.

#include "fake_mqtt_client.h"

#include "mqtt/BrokerAdmin.h"

#include <gtest/gtest.h>

#include <memory>

using liveqx::mqtt::BrokerAdmin;
using liveqx::testing::FakeMqttClient;
using nlohmann::json;

namespace {

const std::string A = "0b8f5c1e-3a2d-4c47-9a6e-1f2d3c4b5a69";
const std::string B = "7c1d2e3f-4a5b-4c6d-8e7f-90a1b2c3d4e5";
const std::string C = "11111111-2222-4333-8444-555555555555";

struct Stand {
    FakeMqttClient fake;
    std::unique_ptr<BrokerAdmin> admin;
    explicit Stand(std::chrono::milliseconds timeout = std::chrono::milliseconds(500)) {
        fake.dynsec = true;
        fake.clients.insert(BrokerAdmin::kServerClient);        // made by `dynsec init`
        fake.roles.insert("admin");
        BrokerAdmin::Options o;
        o.reply_timeout = timeout;
        admin = std::make_unique<BrokerAdmin>(fake, o);
        fake.setMessageHandler([this](const liveqx::mqtt::Message& m) { admin->handleMessage(m); });
    }
};

}  // namespace

TEST(BrokerAdmin, ItSubscribesToRepliesWhenCreated) {
    FakeMqttClient fake;
    BrokerAdmin admin(fake);
    ASSERT_EQ(fake.subscriptions.size(), 1u);
    EXPECT_EQ(fake.subscriptions[0], "$CONTROL/dynamic-security/v1/response");
}

TEST(BrokerAdmin, APlayerGetsExactlyItsOwnTopicsAndTheSharedRole) {
    const json cmds = BrokerAdmin::playerCommands(A);
    int acls = 0;
    std::set<std::string> topics;
    for (const auto& c : cmds) {
        if (c["command"] == "addRoleACL") {
            ++acls;
            topics.insert(c["topic"].get<std::string>());
            EXPECT_EQ(c["rolename"], "player-" + A);
            EXPECT_TRUE(c["allow"].get<bool>());
        }
        EXPECT_NE(c["command"], "addClientRole") << "bindings are sent separately, only when missing";
    }
    EXPECT_EQ(acls, 3);
    EXPECT_EQ(topics, (std::set<std::string>{"liveqx/v1/players/" + A + "/commands",
                                             "liveqx/v1/players/" + A + "/presence"}));
    for (const auto& t : topics) {
        EXPECT_EQ(t.find('+'), std::string::npos) << "never a wildcard in a player's rights";
        EXPECT_EQ(t.find('#'), std::string::npos);
    }
    const auto bindings = BrokerAdmin::playerBindings(A);
    ASSERT_EQ(bindings.size(), 2u);
    EXPECT_EQ(bindings[0].first, "player-common");
    EXPECT_EQ(bindings[1].first, "player-" + A);
}

TEST(BrokerAdmin, TheBaselineDeniesByDefaultAndGivesTheServerItsWorkingRights) {
    const json cmds = BrokerAdmin::baselineCommands();
    const auto& first = cmds[0];
    ASSERT_EQ(first["command"], "setDefaultACLAccess");
    ASSERT_EQ(first["acls"].size(), 4u);
    for (const auto& a : first["acls"]) EXPECT_FALSE(a["allow"].get<bool>());
    const auto bindings = BrokerAdmin::serverBindings();
    ASSERT_EQ(bindings.size(), 1u);
    EXPECT_EQ(bindings[0].first, "server-ops");
}

TEST(BrokerAdmin, IssuingAPlayerCreatesItsClientAndRole) {
    Stand s;
    ASSERT_TRUE(s.admin->ensureBaseline().ok);
    auto r = s.admin->provisionPlayer(A);
    EXPECT_TRUE(r.ok) << r.error;
    EXPECT_TRUE(s.fake.clients.count(A));
    EXPECT_TRUE(s.fake.roles.count("player-" + A));
    EXPECT_EQ(s.fake.bindings[A], (std::set<std::string>{"player-common", "player-" + A}));
    EXPECT_EQ(s.fake.bindings["liveqx-server"], (std::set<std::string>{"server-ops"}));
}

TEST(BrokerAdmin, IssuingTwiceIsTheSameAsOnce) {
    Stand s;
    ASSERT_TRUE(s.admin->ensureBaseline().ok);
    ASSERT_TRUE(s.admin->provisionPlayer(A).ok);
    auto again = s.admin->provisionPlayer(A);
    EXPECT_TRUE(again.ok) << again.error << " — 'already exists' is not a failure";
    EXPECT_TRUE(s.admin->ensureBaseline().ok) << "the baseline too";
    EXPECT_EQ(s.fake.countCommand("addClientRole"), 3u) << "each binding sent once, never repeated";
}

TEST(BrokerAdmin, ARealRefusalIsReportedNotSwallowed) {
    Stand s;
    // Without the baseline the shared role does not exist: binding the player to
    // it is a real error and must not be mistaken for "already there".
    auto r = s.admin->provisionPlayer(A);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("addClientRole"), std::string::npos) << r.error;
    EXPECT_NE(r.error.find("not found"), std::string::npos) << r.error;
}

TEST(BrokerAdmin, RemovingAPlayerThatIsNotThereIsFine) {
    Stand s;
    ASSERT_TRUE(s.admin->ensureBaseline().ok);
    ASSERT_TRUE(s.admin->provisionPlayer(A).ok);
    EXPECT_TRUE(s.admin->removePlayer(A).ok);
    EXPECT_FALSE(s.fake.clients.count(A));
    EXPECT_FALSE(s.fake.roles.count("player-" + A));
    EXPECT_TRUE(s.admin->removePlayer(A).ok) << "a second removal finds nothing and says so quietly";
}

TEST(BrokerAdmin, ADownBrokerIsAnAnswerNotAHang) {
    Stand s;
    s.fake.up = false;
    const auto t0 = std::chrono::steady_clock::now();
    auto r = s.admin->provisionPlayer(A);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error, "broker_not_connected");
    EXPECT_LT(std::chrono::steady_clock::now() - t0, std::chrono::milliseconds(200));
}

TEST(BrokerAdmin, ASilentBrokerTimesOut) {
    Stand s(std::chrono::milliseconds(150));
    s.fake.silent = true;
    auto r = s.admin->provisionPlayer(A);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error, "broker_no_reply");
    s.fake.silent = false;
    ASSERT_TRUE(s.admin->ensureBaseline().ok) << "a timed-out call does not poison the next one";
}

TEST(BrokerAdmin, OnlyTheReplyToTheCallThatWaitsIsKept) {
    Stand s;
    // A stray reply (no one waiting) and one with no correlation are both absorbed.
    liveqx::mqtt::Message stray;
    stray.topic   = "$CONTROL/dynamic-security/v1/response";
    stray.payload = R"({"responses":[{"command":"createClient","correlationData":"lqx-999"}]})";
    EXPECT_TRUE(s.admin->handleMessage(stray));
    stray.payload = "not json at all";
    EXPECT_TRUE(s.admin->handleMessage(stray));
    liveqx::mqtt::Message other;
    other.topic = "liveqx/v1/players/x/presence";
    EXPECT_FALSE(s.admin->handleMessage(other)) << "not ours: left for the next handler";
    EXPECT_TRUE(s.admin->ensureBaseline().ok);
}

TEST(BrokerAdmin, ReconcileIssuesTheMissingAndRemovesWhatNobodyOwns) {
    Stand s;
    ASSERT_TRUE(s.admin->ensureBaseline().ok);
    ASSERT_TRUE(s.admin->provisionPlayer(B).ok);         // B is on the broker but no longer wanted
    ASSERT_TRUE(s.admin->provisionPlayer(A).ok);         // A is fine
    s.fake.roles.erase("player-" + A);                   // ...except its role is gone (half-issued)
    s.fake.bindings[A].erase("player-" + A);

    auto rep = s.admin->reconcile({A, C});
    EXPECT_TRUE(rep.ok) << rep.error;
    EXPECT_EQ(rep.issued, 2u) << "A (repaired) and C (new)";
    EXPECT_EQ(rep.removed, 1u) << "B";
    EXPECT_TRUE(s.fake.clients.count(A));
    EXPECT_TRUE(s.fake.clients.count(C));
    EXPECT_FALSE(s.fake.clients.count(B));
    EXPECT_FALSE(s.fake.roles.count("player-" + B));
    EXPECT_TRUE(s.fake.roles.count("player-" + A));
}

TEST(BrokerAdmin, ReconcileNeverTouchesTheServerOrTheSharedRoles) {
    Stand s;
    auto rep = s.admin->reconcile({});
    EXPECT_TRUE(rep.ok) << rep.error;
    EXPECT_TRUE(s.fake.clients.count("liveqx-server"));
    EXPECT_TRUE(s.fake.roles.count("admin"));
    EXPECT_TRUE(s.fake.roles.count("player-common"));
    EXPECT_TRUE(s.fake.roles.count("server-ops"));
    EXPECT_EQ(rep.removed, 0u);
}

TEST(BrokerAdmin, ReconcileOnADownBrokerReportsItAndChangesNothing) {
    Stand s;
    s.fake.up = false;
    auto rep = s.admin->reconcile({A});
    EXPECT_FALSE(rep.ok);
    EXPECT_EQ(rep.error, "broker_not_connected");
    EXPECT_EQ(rep.issued, 0u);
}

TEST(BrokerAdmin, ReconcileIsQuietWhenNothingDiffers) {
    Stand s;
    ASSERT_TRUE(s.admin->reconcile({A, B}).ok);
    const auto before = s.fake.countCommand("createClient");
    auto rep = s.admin->reconcile({A, B});
    EXPECT_TRUE(rep.ok);
    EXPECT_EQ(rep.issued, 0u);
    EXPECT_EQ(rep.removed, 0u);
    EXPECT_EQ(s.fake.countCommand("createClient"), before) << "no needless writes to the broker";
}

TEST(BrokerAdmin, ThePlayerIdFilterKeepsOtherNamesSafe) {
    EXPECT_TRUE(BrokerAdmin::looksLikePlayerId(A));
    EXPECT_FALSE(BrokerAdmin::looksLikePlayerId("liveqx-server"));
    EXPECT_FALSE(BrokerAdmin::looksLikePlayerId("admin"));
    EXPECT_FALSE(BrokerAdmin::looksLikePlayerId(""));
    std::string upper = A;
    upper[0] = 'F';
    EXPECT_FALSE(BrokerAdmin::looksLikePlayerId(upper)) << "ids are lowercase";
}
