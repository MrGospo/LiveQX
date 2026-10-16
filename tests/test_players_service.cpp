// Unit tests for players/PlayersService — the domain layer between the REST
// handlers and state/players.db.
//
// What these tests protect:
//   * the pairing package has exactly the A3 fields and nothing secret besides
//     the one-time token (no key, no certificate);
//   * nothing is issued while the module is not configured (no server URL ->
//     no package, and no half-created player left behind);
//   * settings are validated (https / mqtts, no credentials in URLs, token
//     lifetime and clock policy ranges) and a bad patch changes nothing;
//   * ca_cert_pem comes from the server's TLS root, or from an explicit
//     override; without either the module refuses to issue packages;
//   * ntp_hint appears only when the server has NTP configured;
//   * revoking reports the certificates it revoked (the CRL rebuild hook).

#include "events/EventBus.h"
#include "players/PlayersService.h"
#include "utils/Tls.h"

#include <gtest/gtest.h>

#include <unistd.h>

#include <atomic>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <set>

namespace fs = std::filesystem;
using namespace liveqx::players;
using nlohmann::json;

namespace {

constexpr std::int64_t kT0 = 1'800'000'000;

std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

class PlayersServiceTest : public ::testing::Test {
protected:
    fs::path dir_;
    fs::path tls_dir_;
    std::unique_ptr<PlayersDb> db_;
    liveqx::events::EventBus bus_;
    std::vector<std::string> ntp_;
    std::vector<CertRow> revoked_by_hook_;
    std::unique_ptr<PlayersService> svc_;

    void SetUp() override {
        static std::atomic<int> seq{0};
        dir_ = fs::temp_directory_path() /
               ("lqx-playerssvc-" + std::to_string(::getpid()) + "-" + std::to_string(seq.fetch_add(1)));
        tls_dir_ = dir_ / "tls";
        fs::create_directories(tls_dir_);
        db_ = std::make_unique<PlayersDb>(dir_ / "players.db");
        ASSERT_TRUE(db_->open());
        ASSERT_TRUE(liveqx::tls::ensureCa(tls_dir_).ok());
        makeService();
    }
    void TearDown() override {
        svc_.reset();
        db_.reset();
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    void makeService() {
        PlayersServiceOptions o;
        o.ca_cert_path = tls_dir_ / "ca.crt";
        o.events       = &bus_;
        o.ntp_hint     = [this] { return ntp_; };
        o.on_certs_revoked = [this](const std::vector<CertRow>& c) {
            revoked_by_hook_.insert(revoked_by_hook_.end(), c.begin(), c.end());
        };
        svc_ = std::make_unique<PlayersService>(*db_, std::move(o));
    }

    void configure() {
        ASSERT_EQ(svc_->updateSettings({
            {"server_url", "https://liveqx.example:8443"},
            {"device_api_url", "https://liveqx.example:8444"},
            {"mqtt_broker_url", "mqtts://liveqx.example:8883"}}), "");
    }

    CertRow certFor(const std::string& player, const std::string& serial) {
        CertRow c;
        c.serial_hex = serial; c.generation = 1; c.player_id = player;
        c.issued_at = kT0; c.not_before = kT0 - 86400; c.not_after = kT0 + 365 * 86400;
        return c;
    }
};

}  // namespace

TEST_F(PlayersServiceTest, SettingsHaveSafeDefaults) {
    auto s = svc_->settings();
    EXPECT_TRUE(s.server_url.empty());
    EXPECT_EQ(s.token_ttl_days, 7);
    EXPECT_EQ(s.clock_policy, "strict");
}

TEST_F(PlayersServiceTest, ThePackageIsRefusedUntilTheModuleIsConfigured) {
    auto r = svc_->create("Lobby", 1, std::nullopt, kT0);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error, "not_configured");
    EXPECT_TRUE(db_->listPlayers({}).empty()) << "no half-created player";
}

TEST_F(PlayersServiceTest, ThePackageCarriesExactlyTheA3Fields) {
    configure();
    auto r = svc_->create("Lobby", 7, std::nullopt, kT0);
    ASSERT_TRUE(r.ok) << r.error;
    const json& p = r.package;
    std::set<std::string> keys;
    for (auto it = p.begin(); it != p.end(); ++it) keys.insert(it.key());
    EXPECT_EQ(keys, (std::set<std::string>{"schema", "player_id", "server_url", "ca_cert_pem",
                                           "device_api_url", "mqtt_broker_url", "bootstrap_token",
                                           "token_expires_at", "clock_policy"}))
        << "ntp_hint must be absent when the server has no NTP";
    EXPECT_EQ(p["schema"], 1);
    EXPECT_EQ(p["player_id"], r.player.id);
    EXPECT_EQ(p["server_url"], "https://liveqx.example:8443");
    EXPECT_EQ(p["device_api_url"], "https://liveqx.example:8444");
    EXPECT_EQ(p["mqtt_broker_url"], "mqtts://liveqx.example:8883");
    EXPECT_EQ(p["clock_policy"], "strict");
    EXPECT_FALSE(p["bootstrap_token"].get<std::string>().empty());
    EXPECT_EQ(p["ca_cert_pem"], slurp(tls_dir_ / "ca.crt"));
    // The package never holds private material.
    EXPECT_EQ(p.dump().find("PRIVATE KEY"), std::string::npos);
}

TEST_F(PlayersServiceTest, TokenExpiryIsRfc3339UtcAndFollowsTheConfiguredLifetime) {
    configure();
    ASSERT_EQ(svc_->updateSettings({{"token_ttl_days", "1"}}), "");
    auto r = svc_->create("Lobby", 1, std::nullopt, kT0);
    ASSERT_TRUE(r.ok);
    std::time_t t = kT0 + 86400;
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    EXPECT_EQ(r.package["token_expires_at"], std::string(buf));
}

TEST_F(PlayersServiceTest, TheCreatedPlayerIsPendingAndAnEventIsPublished) {
    configure();
    auto sub = bus_.subscribe();
    auto r = svc_->create("Lobby", 7, std::nullopt, kT0);
    ASSERT_TRUE(r.ok);
    auto stored = db_->findPlayer(r.player.id);
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(stored->state, PlayerState::Pending);
    EXPECT_EQ(stored->created_by, std::optional<std::int64_t>(7));
    auto evs = sub->drain(std::chrono::milliseconds(50));
    ASSERT_EQ(evs.size(), 1u);
    EXPECT_EQ(evs[0].type, liveqx::events::EventType::PlayerRegistered);
    EXPECT_EQ(evs[0].payload["player_id"], r.player.id);
    EXPECT_EQ(evs[0].payload["name"], "Lobby");
    EXPECT_EQ(evs[0].payload.dump().find(r.package["bootstrap_token"].get<std::string>()),
              std::string::npos) << "the token must never travel over SSE";
}

TEST_F(PlayersServiceTest, ThePolicyOfTheRequestWinsOverTheDefault) {
    configure();
    ASSERT_EQ(svc_->updateSettings({{"clock_policy", "relaxed"}}), "");
    EXPECT_EQ(svc_->create("A", 1, std::nullopt, kT0).package["clock_policy"], "relaxed");
    EXPECT_EQ(svc_->create("B", 1, std::string("strict"), kT0).package["clock_policy"], "strict");
}

TEST_F(PlayersServiceTest, AnInvalidNameOrPolicyIsRejectedWithoutSideEffects) {
    configure();
    auto a = svc_->create("   ", 1, std::nullopt, kT0);
    EXPECT_FALSE(a.ok);
    EXPECT_EQ(a.error, "invalid_name");
    auto b = svc_->create("Lobby", 1, std::string("whatever"), kT0);
    EXPECT_FALSE(b.ok);
    EXPECT_EQ(b.error, "invalid_clock_policy");
    EXPECT_TRUE(db_->listPlayers({}).empty());
}

TEST_F(PlayersServiceTest, NtpHintAppearsOnlyWhenTheServerHasNtp) {
    configure();
    ntp_ = {"pool.ntp.org:123", "time.example"};
    auto r = svc_->create("Lobby", 1, std::nullopt, kT0);
    ASSERT_TRUE(r.ok);
    ASSERT_TRUE(r.package.contains("ntp_hint"));
    EXPECT_EQ(r.package["ntp_hint"], (json{"pool.ntp.org:123", "time.example"}));
}

TEST_F(PlayersServiceTest, WithoutACaNoPackageIsIssued) {
    configure();
    fs::remove(tls_dir_ / "ca.crt");
    auto r = svc_->create("Lobby", 1, std::nullopt, kT0);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error, "ca_unavailable");
    EXPECT_TRUE(db_->listPlayers({}).empty());
}

TEST_F(PlayersServiceTest, AnExplicitCaOverrideIsUsedInsteadOfTheTlsDirectory) {
    configure();
    const fs::path other = dir_ / "other";
    fs::create_directories(other);
    ASSERT_TRUE(liveqx::tls::ensureCa(other, "Other CA").ok());
    const std::string pem = slurp(other / "ca.crt");
    ASSERT_EQ(svc_->updateSettings({{"ca_cert_pem", pem}}), "");
    auto r = svc_->create("Lobby", 1, std::nullopt, kT0);
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.package["ca_cert_pem"], pem);
}

TEST_F(PlayersServiceTest, AnOverrideThatIsNotACertificateIsRefused) {
    EXPECT_EQ(svc_->updateSettings({{"ca_cert_pem", "not a certificate"}}), "invalid_ca_cert_pem");
    EXPECT_TRUE(svc_->settings().ca_cert_pem.empty());
}

TEST_F(PlayersServiceTest, UrlsAreValidated) {
    EXPECT_EQ(svc_->updateSettings({{"server_url", "http://liveqx.example"}}), "invalid_server_url")
        << "plain http is not acceptable for pairing";
    EXPECT_EQ(svc_->updateSettings({{"server_url", "https://user:pw@liveqx.example"}}), "invalid_server_url")
        << "no credentials inside a URL";
    EXPECT_EQ(svc_->updateSettings({{"server_url", "https://liveqx.example/?a=1"}}), "invalid_server_url");
    EXPECT_EQ(svc_->updateSettings({{"server_url", "https://liveqx.example:99999"}}), "invalid_server_url");
    EXPECT_EQ(svc_->updateSettings({{"server_url", "https://live qx.example"}}), "invalid_server_url");
    EXPECT_EQ(svc_->updateSettings({{"server_url", "https://"}}), "invalid_server_url");
    EXPECT_EQ(svc_->updateSettings({{"device_api_url", "ftp://x.example"}}), "invalid_device_api_url");
    EXPECT_EQ(svc_->updateSettings({{"mqtt_broker_url", "mqtt://x.example:1883"}}), "invalid_mqtt_broker_url")
        << "the broker connection must be TLS";
    EXPECT_EQ(svc_->updateSettings({{"mqtt_broker_url", "mqtts://x.example:8883"}}), "");
    EXPECT_EQ(svc_->updateSettings({{"server_url", "https://[fd00::1]:8443"}}), "") << "IPv6 literal";
}

TEST_F(PlayersServiceTest, TokenLifetimeAndPolicyAreRangeChecked) {
    EXPECT_EQ(svc_->updateSettings({{"token_ttl_days", "0"}}), "invalid_token_ttl_days");
    EXPECT_EQ(svc_->updateSettings({{"token_ttl_days", "366"}}), "invalid_token_ttl_days");
    EXPECT_EQ(svc_->updateSettings({{"token_ttl_days", "abc"}}), "invalid_token_ttl_days");
    EXPECT_EQ(svc_->updateSettings({{"token_ttl_days", "30"}}), "");
    EXPECT_EQ(svc_->settings().token_ttl_days, 30);
    EXPECT_EQ(svc_->updateSettings({{"clock_policy", "lax"}}), "invalid_clock_policy");
}

TEST_F(PlayersServiceTest, AnUnknownSettingIsRefused) {
    EXPECT_EQ(svc_->updateSettings({{"colour", "blue"}}), "unknown_setting");
}

TEST_F(PlayersServiceTest, ABadPatchChangesNothingEvenForItsValidKeys) {
    configure();
    EXPECT_EQ(svc_->updateSettings({{"server_url", "https://new.example"},
                                    {"mqtt_broker_url", "mqtt://bad"}}), "invalid_mqtt_broker_url");
    EXPECT_EQ(svc_->settings().server_url, "https://liveqx.example:8443");
}

TEST_F(PlayersServiceTest, SettingsSurviveARestart) {
    configure();
    svc_.reset();
    makeService();
    EXPECT_EQ(svc_->settings().mqtt_broker_url, "mqtts://liveqx.example:8883");
}

TEST_F(PlayersServiceTest, AnEmptyValueClearsASetting) {
    configure();
    ASSERT_EQ(svc_->updateSettings({{"server_url", ""}}), "");
    EXPECT_TRUE(svc_->settings().server_url.empty());
    EXPECT_EQ(svc_->create("Lobby", 1, std::nullopt, kT0).error, "not_configured");
}

TEST_F(PlayersServiceTest, ReissuingGivesANewPackageForAPendingPlayerOnly) {
    configure();
    auto r = svc_->create("Lobby", 1, std::nullopt, kT0);
    ASSERT_TRUE(r.ok);
    auto again = svc_->reissueToken(r.player.id, kT0 + 10);
    ASSERT_TRUE(again.ok) << again.error;
    EXPECT_NE(again.package["bootstrap_token"], r.package["bootstrap_token"]);
    EXPECT_EQ(again.package["player_id"], r.player.id);
    EXPECT_FALSE(db_->claimToken(r.player.id, r.package["bootstrap_token"], kT0 + 11).ok)
        << "the old token stops working";

    EXPECT_EQ(svc_->reissueToken("00000000-0000-4000-8000-000000000000", kT0).error, "not_found");

    auto claim = db_->claimToken(r.player.id, again.package["bootstrap_token"], kT0 + 12);
    ASSERT_TRUE(claim.ok);
    ASSERT_TRUE(db_->finishEnrollment(claim.token_id, certFor(r.player.id, "AA01"), kT0 + 13));
    EXPECT_EQ(svc_->reissueToken(r.player.id, kT0 + 20).error, "not_pending")
        << "an active player is re-paired by revoking it, not by a new token";
}

TEST_F(PlayersServiceTest, RevokingReportsTheCertificatesToTheCrlHook) {
    configure();
    auto r = svc_->create("Lobby", 1, std::nullopt, kT0);
    auto claim = db_->claimToken(r.player.id, r.package["bootstrap_token"], kT0 + 1);
    ASSERT_TRUE(db_->finishEnrollment(claim.token_id, certFor(r.player.id, "BB02"), kT0 + 2));

    auto out = svc_->revoke(r.player.id, "stolen", kT0 + 3);
    ASSERT_TRUE(out.found);
    EXPECT_EQ(out.revoked_certs, 1u);
    ASSERT_EQ(revoked_by_hook_.size(), 1u);
    EXPECT_EQ(revoked_by_hook_[0].serial_hex, "BB02");
    EXPECT_EQ(db_->findPlayer(r.player.id)->state, PlayerState::Revoked);

    // Revoking twice must not report the same certificate twice.
    auto again = svc_->revoke(r.player.id, "stolen", kT0 + 4);
    EXPECT_TRUE(again.found);
    EXPECT_EQ(again.revoked_certs, 0u);
    EXPECT_EQ(revoked_by_hook_.size(), 1u);
}

TEST_F(PlayersServiceTest, RevokingAnUnknownPlayerIsNotFound) {
    EXPECT_FALSE(svc_->revoke("00000000-0000-4000-8000-000000000000", "x", kT0).found);
}

TEST_F(PlayersServiceTest, DeletingRevokesTheCertificatesFirstAndKeepsTheirRecords) {
    configure();
    auto r = svc_->create("Lobby", 1, std::nullopt, kT0);
    auto claim = db_->claimToken(r.player.id, r.package["bootstrap_token"], kT0 + 1);
    ASSERT_TRUE(db_->finishEnrollment(claim.token_id, certFor(r.player.id, "CC03"), kT0 + 2));

    auto out = svc_->remove(r.player.id, "decommissioned", kT0 + 3);
    ASSERT_TRUE(out.found);
    EXPECT_EQ(out.revoked_certs, 1u);
    ASSERT_EQ(revoked_by_hook_.size(), 1u) << "the CRL has to learn about it";
    EXPECT_FALSE(db_->findPlayer(r.player.id).has_value());
    EXPECT_EQ(db_->revokedSerials(1, kT0 + 4).size(), 1u) << "stays in the CRL";
}

TEST_F(PlayersServiceTest, DeletingAPendingPlayerBurnsItsToken) {
    configure();
    auto r = svc_->create("Lobby", 1, std::nullopt, kT0);
    ASSERT_TRUE(svc_->remove(r.player.id, "mistake", kT0 + 1).found);
    EXPECT_FALSE(db_->claimToken(r.player.id, r.package["bootstrap_token"], kT0 + 2).ok);
    EXPECT_TRUE(revoked_by_hook_.empty());
}
