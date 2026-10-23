// players/BrokerSettings — changing how the server reaches the MQTT broker, from
// the interface.
//
// What these tests protect:
//   * a change that points the server somewhere new is tried with a throwaway
//     connection first, and applied only if the broker answers — a wrong address
//     must not cut the server off from its broker;
//   * the trial uses its own client id, so it never knocks the running
//     connection off the broker;
//   * an applied change is stored (it survives a restart, wins over the file) and
//     the running link reconnects to the new place;
//   * a change that cannot cut anything off (the reconciliation period) needs no
//     trial; "check only" changes nothing; "skip check" is explicit;
//   * bad input is refused with a reason, and nothing is half-applied.

#include "fake_mqtt_client.h"

#include "mqtt/BrokerLink.h"
#include "players/BrokerSettings.h"

#include <gtest/gtest.h>

#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <thread>

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using liveqx::mqtt::BrokerConnection;
using liveqx::mqtt::BrokerIdentityPem;
using liveqx::mqtt::BrokerLink;
using liveqx::mqtt::BrokerLinkOptions;
using liveqx::mqtt::ClientOptions;
using liveqx::mqtt::IMqttClient;
using liveqx::testing::FakeMqttClient;
using nlohmann::json;
using namespace liveqx::players;

namespace {

bool waitFor(const std::function<bool()>& pred, std::chrono::milliseconds limit = 8000ms) {
    const auto end = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < end) {
        if (pred()) return true;
        std::this_thread::sleep_for(10ms);
    }
    return pred();
}

class BrokerSettingsTest : public ::testing::Test {
protected:
    fs::path dir_;
    std::unique_ptr<PlayersDb> db_;
    std::unique_ptr<PlayersService> players_;
    std::unique_ptr<BrokerLink> link_;

    std::mutex mu_;
    bool identity_ready_{true};
    std::atomic<bool> reachable_{true};            // do connections to new addresses come up?
    std::vector<ClientOptions> made_;

    void SetUp() override {
        static std::atomic<int> seq{0};
        dir_ = fs::temp_directory_path() /
               ("lqx-brokerset-" + std::to_string(::getpid()) + "-" + std::to_string(seq.fetch_add(1)));
        fs::create_directories(dir_);
        db_ = std::make_unique<PlayersDb>(dir_ / "players.db");
        ASSERT_TRUE(db_->open());
        PlayersServiceOptions po;
        players_ = std::make_unique<PlayersService>(*db_, std::move(po));
    }
    void TearDown() override {
        link_.reset();
        players_.reset();
        db_.reset();
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    void startLink() {
        BrokerLinkOptions o;
        o.host = "127.0.0.1";
        o.port = 8883;
        o.retry_s = 1;
        o.reconcile_interval_s = 30;
        o.probe_timeout_s = 1;
        o.admin.reply_timeout = 500ms;
        o.identity = [this](BrokerIdentityPem& id, std::string& why) {
            std::lock_guard<std::mutex> lk(mu_);
            if (!identity_ready_) { why = "the device CA is not set up yet"; return false; }
            id.cert_chain_pem = "CERT"; id.key_pem = "KEY"; id.serial = "s1";
            return true;
        };
        o.trust_pem = [] { return std::string("ROOT"); };
        o.wanted = [] { return std::set<std::string>{}; };
        o.make_client = [this](const ClientOptions& co) -> std::unique_ptr<IMqttClient> {
            auto f = std::make_unique<FakeMqttClient>();
            f->dynsec = true;
            f->clients.insert("liveqx-server");
            f->up = reachable_.load() || co.host == "127.0.0.1";        // the first address always works
            std::lock_guard<std::mutex> lk(mu_);
            if (co.client_id == "liveqx-server-setup") return f;       // the short connection for the server's rights is not what is counted here
            made_.push_back(co);
            return f;
        };
        link_ = std::make_unique<BrokerLink>(std::move(o));
        link_->start();
        ASSERT_TRUE(waitFor([&] { return link_->status().state == "connected"; }));
    }
    std::size_t madeCount() { std::lock_guard<std::mutex> lk(mu_); return made_.size(); }
    BrokerUpdateResult put(const json& body) { return updateBrokerConnection(body, *players_, link_.get()); }
};

}  // namespace

TEST_F(BrokerSettingsTest, WithoutALinkThereIsNothingToChange) {
    auto r = updateBrokerConnection(json{{"port", 8884}}, *players_, nullptr);
    EXPECT_EQ(r.status, 409);
    EXPECT_EQ(r.body["error"], "broker_disabled");
}

TEST_F(BrokerSettingsTest, ANewAddressThatAnswersIsAppliedStoredAndTheLinkMovesThere) {
    startLink();
    ASSERT_EQ(madeCount(), 1u);
    auto r = put({{"host", "mqtt.example.lan"}, {"port", 8884}});
    ASSERT_EQ(r.status, 200) << r.body.dump();
    EXPECT_EQ(r.body["applied"], true);
    EXPECT_EQ(r.body["checked"], true);
    EXPECT_EQ(r.body["connection"]["host"], "mqtt.example.lan");
    EXPECT_EQ(r.body["connection"]["port"], 8884);
    EXPECT_EQ(link_->connection().host, "mqtt.example.lan");

    ASSERT_TRUE(waitFor([&] {
        std::lock_guard<std::mutex> lk(mu_);
        return !made_.empty() && made_.back().host == "mqtt.example.lan" && made_.back().client_id == "liveqx-server";
    })) << "the real connection followed";

    auto stored = players_->brokerConnection(BrokerConnection{});
    EXPECT_EQ(stored.host, "mqtt.example.lan") << "stored: it wins over the file after a restart";
    EXPECT_EQ(stored.port, 8884);
    EXPECT_EQ(r.audit["changed"], (json::array({"host", "port"})));
}

TEST_F(BrokerSettingsTest, TheTrialUsesItsOwnClientIdSoTheRunningConnectionIsNotKnockedOff) {
    startLink();
    auto r = put({{"check_only", true}, {"port", 8884}});
    ASSERT_EQ(r.status, 200);
    std::lock_guard<std::mutex> lk(mu_);
    ASSERT_GE(made_.size(), 2u);
    EXPECT_EQ(made_[0].client_id, "liveqx-server");
    EXPECT_EQ(made_[1].client_id, "liveqx-server-probe");
    EXPECT_EQ(made_[1].port, 8884);
}

TEST_F(BrokerSettingsTest, AnAddressThatDoesNotAnswerIsRefusedAndNothingChanges) {
    startLink();
    reachable_ = false;
    auto r = put({{"host", "wrong.example.lan"}});
    EXPECT_EQ(r.status, 502);
    EXPECT_EQ(r.body["error"], "probe_failed");
    EXPECT_EQ(r.body["connection"]["host"], "127.0.0.1") << "the old, working address is still the one in force";
    EXPECT_EQ(link_->connection().host, "127.0.0.1");
    EXPECT_EQ(players_->brokerConnection(BrokerConnection{}).host, "127.0.0.1") << "nothing stored";
    std::this_thread::sleep_for(300ms);
    EXPECT_EQ(link_->status().state, "connected") << "the running link was never disturbed";
}

TEST_F(BrokerSettingsTest, CheckOnlyReportsAndChangesNothing) {
    startLink();
    reachable_ = false;
    auto bad = put({{"host", "wrong.example.lan"}, {"check_only", true}});
    EXPECT_EQ(bad.status, 200);
    EXPECT_EQ(bad.body["checked"], true);
    EXPECT_EQ(bad.body["ok"], false);
    EXPECT_FALSE(bad.body["detail"].get<std::string>().empty());
    reachable_ = true;
    auto good = put({{"host", "right.example.lan"}, {"check_only", true}});
    EXPECT_EQ(good.body["ok"], true);
    EXPECT_EQ(link_->connection().host, "127.0.0.1");
    EXPECT_EQ(players_->brokerConnection(BrokerConnection{}).host, "127.0.0.1");
}

TEST_F(BrokerSettingsTest, SkippingTheCheckIsExplicitAndAppliesAnUnreachableAddress) {
    startLink();
    reachable_ = false;
    auto r = put({{"host", "not-up-yet.example.lan"}, {"skip_check", true}});
    ASSERT_EQ(r.status, 200) << r.body.dump();
    EXPECT_EQ(r.body["applied"], true);
    EXPECT_EQ(r.body["checked"], false);
    EXPECT_EQ(link_->connection().host, "not-up-yet.example.lan");
    EXPECT_EQ(r.audit["skip_check"], true) << "the audit trail shows it was not tried";
}

TEST_F(BrokerSettingsTest, ThePeriodAloneNeedsNoTrialBecauseItCannotCutAnythingOff) {
    startLink();
    reachable_ = false;                                   // would fail a trial; none is made
    auto r = put({{"reconcile_interval_s", 120}});
    ASSERT_EQ(r.status, 200) << r.body.dump();
    EXPECT_EQ(r.body["applied"], true);
    EXPECT_EQ(r.body["checked"], false);
    EXPECT_EQ(link_->connection().reconcile_interval_s, 120);
    EXPECT_EQ(players_->brokerConnection(BrokerConnection{}).reconcile_interval_s, 120);
}

TEST_F(BrokerSettingsTest, TurningOffNameCheckingWorksButIsFlagged) {
    startLink();
    auto r = put({{"verify_hostname", false}});
    ASSERT_EQ(r.status, 200);
    EXPECT_EQ(r.body["warning"], "verify_hostname_disabled");
    EXPECT_EQ(r.audit["verify_hostname"], false);
    EXPECT_FALSE(link_->connection().verify_hostname);
}

TEST_F(BrokerSettingsTest, NothingToChangeIsSaidPlainly) {
    startLink();
    auto r = put({{"host", "127.0.0.1"}, {"port", 8883}});
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body["applied"], false);
    EXPECT_EQ(r.body["unchanged"], true);
    EXPECT_EQ(madeCount(), 1u) << "no trial, no reconnect";
}

TEST_F(BrokerSettingsTest, BadInputIsRefusedWithAReasonAndTouchesNothing) {
    startLink();
    struct Case { json body; const char* error; };
    const Case cases[] = {
        {{{"host", ""}}, "invalid_host"},
        {{{"host", "bad host"}}, "invalid_host"},
        {{{"host", "-leading"}}, "invalid_host"},
        {{{"host", 5}}, "invalid_host"},
        {{{"port", 0}}, "invalid_port"},
        {{{"port", 70000}}, "invalid_port"},
        {{{"port", "8883"}}, "invalid_port"},
        {{{"reconcile_interval_s", 5}}, "invalid_reconcile_interval_s"},
        {{{"reconcile_interval_s", 99999999}}, "invalid_reconcile_interval_s"},
        {{{"verify_hostname", "no"}}, "invalid_verify_hostname"},
        {{{"check_only", true}, {"skip_check", true}}, "check_only_and_skip_check"},
        {{{"enabled", true}}, "unknown_field"},
        {json::array(), "invalid_body"},
    };
    for (const auto& c : cases) {
        auto r = put(c.body);
        EXPECT_EQ(r.status, 400) << c.body.dump();
        EXPECT_EQ(r.body["error"], c.error) << c.body.dump();
    }
    EXPECT_EQ(madeCount(), 1u);
    EXPECT_EQ(link_->connection().host, "127.0.0.1");
}

TEST_F(BrokerSettingsTest, ATrialNeedsTheServersIdentityAndSaysSoWhenItIsNotThereYet) {
    startLink();
    { std::lock_guard<std::mutex> lk(mu_); identity_ready_ = false; }
    auto r = put({{"host", "other.example.lan"}});
    EXPECT_EQ(r.status, 502);
    EXPECT_NE(r.body["detail"].get<std::string>().find("not_ready"), std::string::npos) << r.body.dump();
}

TEST_F(BrokerSettingsTest, StoredValuesWinOverTheFileAndBrokenStoredValuesAreIgnored) {
    BrokerConnection file;
    file.host = "from-file.lan";
    file.port = 8883;
    EXPECT_EQ(players_->brokerConnection(file).host, "from-file.lan");

    BrokerConnection chosen = file;
    chosen.host = "chosen.lan";
    chosen.port = 9001;
    chosen.verify_hostname = false;
    chosen.reconcile_interval_s = 60;
    ASSERT_EQ(players_->storeBrokerConnection(chosen), "");
    EXPECT_EQ(players_->brokerConnection(file), chosen);

    EXPECT_EQ(players_->storeBrokerConnection(BrokerConnection{"bad host", 1, true, 60}), "invalid_host");
    ASSERT_TRUE(db_->setSettings({{"broker_port", "99999"}}));
    EXPECT_EQ(players_->brokerConnection(file), file) << "a stored value that does not validate is not used";
}
