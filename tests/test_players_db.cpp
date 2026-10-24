// Unit tests for players/PlayersDb — state/players.db, the store behind the
// player (LiveQX-Player) integration: players, pairing tokens, issued
// certificates, last known state and state-change events.
//
// What these tests protect:
//   * a pairing token is stored only as a hash, shown once, burns on its
//     first successful use, and "wrong / expired / used / unknown player"
//     are indistinguishable to the caller;
//   * two concurrent attempts with one token yield exactly one winner;
//   * enrollment is atomic (token, player state, certificate, event);
//   * certificate records outlive the player, so a revoked certificate stays
//     in the CRL until it would have expired anyway;
//   * CRL numbers only go up, per CA generation, across restarts;
//   * a database from a newer version is refused, not "fixed".

#include "players/PlayersDb.h"

#include <gtest/gtest.h>

#include <sqlite3.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <set>
#include <thread>

namespace fs = std::filesystem;
using namespace liveqx::players;

namespace {

constexpr std::int64_t kT0 = 1'800'000'000;   // an arbitrary "now"

bool isLowerUuidV4(const std::string& s) {
    if (s.size() != 36) return false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (c != '-') return false; }
        else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return s[14] == '4' && (s[19] == '8' || s[19] == '9' || s[19] == 'a' || s[19] == 'b');
}

std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

class PlayersDbTest : public ::testing::Test {
protected:
    fs::path dir_, path_;
    std::unique_ptr<PlayersDb> db_;

    void SetUp() override {
        static std::atomic<int> seq{0};
        dir_ = fs::temp_directory_path() /
               ("lqx-playersdb-" + std::to_string(::getpid()) + "-" + std::to_string(seq.fetch_add(1)));
        fs::create_directories(dir_);
        path_ = dir_ / "players.db";
        db_ = std::make_unique<PlayersDb>(path_);
        ASSERT_TRUE(db_->open());
    }
    void TearDown() override {
        db_.reset();
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    void reopen() {
        db_.reset();
        db_ = std::make_unique<PlayersDb>(path_);
        ASSERT_TRUE(db_->open());
    }

    CreatedPlayer create(const std::string& name = "Lobby screen", std::int64_t ttl = 7 * 86400,
                         std::int64_t now = kT0) {
        auto c = db_->createPlayer(name, /*created_by=*/1, "strict", ttl, now);
        EXPECT_TRUE(c.has_value());
        return *c;
    }

    static CertRow cert(const std::string& player, const std::string& serial, int gen = 1,
                        std::int64_t nb = kT0 - 86400, std::int64_t na = kT0 + 365 * 86400) {
        CertRow c;
        c.player_id = player; c.serial_hex = serial; c.generation = gen;
        c.issued_at = kT0; c.not_before = nb; c.not_after = na;
        return c;
    }

    // Pairs a player completely; returns its id.
    std::string enrolled(const std::string& serial, int gen = 1, const std::string& name = "P") {
        auto c = create(name);
        auto claim = db_->claimToken(c.player.id, c.token, kT0 + 10);
        EXPECT_TRUE(claim.ok);
        EXPECT_TRUE(db_->finishEnrollment(claim.token_id, cert(c.player.id, serial, gen), kT0 + 11));
        return c.player.id;
    }
};

}  // namespace

// ── schema and opening ──────────────────────────────────────────────────────
TEST_F(PlayersDbTest, OpenCreatesTheSchemaAtTheCurrentVersion) {
    EXPECT_EQ(db_->schemaVersion(), PlayersDb::kSchemaVersion);
    EXPECT_TRUE(fs::exists(path_));
}

TEST_F(PlayersDbTest, DataSurvivesReopening) {
    const auto c = create("Hall");
    reopen();
    auto p = db_->findPlayer(c.player.id);
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(p->name, "Hall");
    EXPECT_EQ(db_->schemaVersion(), PlayersDb::kSchemaVersion);
}

TEST_F(PlayersDbTest, ADatabaseFromANewerVersionIsRefusedAndLeftAlone) {
    db_.reset();
    {
        sqlite3* raw = nullptr;
        ASSERT_EQ(sqlite3_open(path_.c_str(), &raw), SQLITE_OK);
        sqlite3_exec(raw, "PRAGMA user_version = 999;", nullptr, nullptr, nullptr);
        sqlite3_close(raw);
    }
    const auto before = slurp(path_);
    PlayersDb other(path_);
    EXPECT_FALSE(other.open());
    EXPECT_EQ(slurp(path_), before) << "an unknown schema must not be touched";
}

TEST_F(PlayersDbTest, TheDatabaseFileIsPrivate) {
    struct stat st{};
    ASSERT_EQ(::stat(path_.c_str(), &st), 0);
    EXPECT_EQ(st.st_mode & 0777, 0600u) << "it holds token hashes and certificate records";
}

// ── players ─────────────────────────────────────────────────────────────────
TEST_F(PlayersDbTest, NewPlayerIsPendingWithAUuidAndAOneTimeToken) {
    const auto c = create("Lobby screen", 7 * 86400, kT0);
    EXPECT_TRUE(isLowerUuidV4(c.player.id)) << c.player.id;
    EXPECT_EQ(c.player.state, PlayerState::Pending);
    EXPECT_EQ(c.player.name, "Lobby screen");
    EXPECT_EQ(c.player.created_at, kT0);
    EXPECT_EQ(c.player.clock_policy, "strict");
    EXPECT_EQ(c.token.size(), 64u) << "256 bits, hex";
    EXPECT_EQ(c.token_expires_at, kT0 + 7 * 86400);
}

TEST_F(PlayersDbTest, TheTokenItselfIsNeverStored) {
    const auto c = create();
    db_.reset();
    const auto bytes = slurp(path_);
    EXPECT_EQ(bytes.find(c.token), std::string::npos) << "only a hash may be on disk";
    // WAL file too: the plaintext must not appear there either.
    const auto wal = path_.string() + "-wal";
    if (fs::exists(wal)) EXPECT_EQ(slurp(wal).find(c.token), std::string::npos);
}

TEST_F(PlayersDbTest, PlayerIdsAndTokensAreDifferentEachTime) {
    const auto a = create("a"), b = create("b");
    EXPECT_NE(a.player.id, b.player.id);
    EXPECT_NE(a.token, b.token);
}

TEST_F(PlayersDbTest, NamesAreValidated) {
    EXPECT_FALSE(db_->createPlayer("", 1, "strict", 86400, kT0));
    EXPECT_FALSE(db_->createPlayer("   ", 1, "strict", 86400, kT0));
    EXPECT_FALSE(db_->createPlayer(std::string(121, 'x'), 1, "strict", 86400, kT0));
    EXPECT_FALSE(db_->createPlayer("bad\nname", 1, "strict", 86400, kT0));
    EXPECT_TRUE(db_->createPlayer(std::string(120, 'x'), 1, "strict", 86400, kT0));
    auto trimmed = db_->createPlayer("  Hall  ", 1, "strict", 86400, kT0);
    ASSERT_TRUE(trimmed);
    EXPECT_EQ(trimmed->player.name, "Hall");
}

TEST_F(PlayersDbTest, ClockPolicyAndTokenLifetimeAreValidated) {
    EXPECT_FALSE(db_->createPlayer("x", 1, "whenever", 86400, kT0));
    EXPECT_TRUE(db_->createPlayer("x", 1, "relaxed", 86400, kT0));
    EXPECT_FALSE(db_->createPlayer("x", 1, "strict", 0, kT0));
    EXPECT_FALSE(db_->createPlayer("x", 1, "strict", 400LL * 86400, kT0)) << "a token lives at most a year";
}

TEST_F(PlayersDbTest, ListFiltersByStateAndIsOrderedByCreation) {
    const auto a = create("a", 86400, kT0);
    const auto b = create("b", 86400, kT0 + 5);
    const auto c = create("c", 86400, kT0 + 9);
    auto claim = db_->claimToken(b.player.id, b.token, kT0 + 20);
    ASSERT_TRUE(claim.ok);
    ASSERT_TRUE(db_->finishEnrollment(claim.token_id, cert(b.player.id, "AA01"), kT0 + 21));

    auto all = db_->listPlayers({});
    ASSERT_EQ(all.size(), 3u);
    EXPECT_EQ(all[0].id, a.player.id);
    EXPECT_EQ(all[2].id, c.player.id);

    auto active = db_->listPlayers({PlayerState::Active});
    ASSERT_EQ(active.size(), 1u);
    EXPECT_EQ(active[0].id, b.player.id);
    EXPECT_EQ(db_->listPlayers({PlayerState::Pending}).size(), 2u);
    EXPECT_TRUE(db_->listPlayers({PlayerState::Revoked}).empty());
}

TEST_F(PlayersDbTest, DeletingAPlayerRemovesItsTokensAndStatusButKeepsCertificateRecords) {
    const auto id = enrolled("BB01");
    ASSERT_TRUE(db_->applyStatuses({{id, kT0 + 50, true, "1.2.3", "{}"}}));
    ASSERT_TRUE(db_->deletePlayer(id, "removed by admin", kT0 + 100));

    EXPECT_FALSE(db_->findPlayer(id).has_value());
    EXPECT_FALSE(db_->status(id).has_value());
    // The certificate was revoked and must stay listed for the CRL.
    auto revoked = db_->revokedSerials(1, kT0 + 100);
    ASSERT_EQ(revoked.size(), 1u);
    EXPECT_EQ(revoked[0].serial_hex, "BB01");
    EXPECT_EQ(revoked[0].revoked_at, kT0 + 100);
}

TEST_F(PlayersDbTest, DeletingAnUnknownPlayerReportsIt) {
    EXPECT_FALSE(db_->deletePlayer("00000000-0000-4000-8000-000000000000", "x", kT0));
}

// ── pairing tokens ──────────────────────────────────────────────────────────
TEST_F(PlayersDbTest, TheRightTokenIsAcceptedOnceAndThenBurnt) {
    const auto c = create();
    auto first = db_->claimToken(c.player.id, c.token, kT0 + 60);
    EXPECT_TRUE(first.ok);
    auto second = db_->claimToken(c.player.id, c.token, kT0 + 61);
    EXPECT_FALSE(second.ok);
    EXPECT_EQ(second.reason, ClaimFailure::AlreadyUsed);
}

TEST_F(PlayersDbTest, EveryKindOfFailureLooksTheSameToTheCaller) {
    const auto c = create("x", 100, kT0);
    const auto other = create("y");

    auto wrong = db_->claimToken(c.player.id, std::string(64, '0'), kT0 + 1);
    auto expired = db_->claimToken(c.player.id, c.token, kT0 + 101);
    auto unknown = db_->claimToken("00000000-0000-4000-8000-000000000000", c.token, kT0 + 1);
    auto foreign = db_->claimToken(other.player.id, c.token, kT0 + 1);
    auto malformed = db_->claimToken(c.player.id, "short", kT0 + 1);

    for (const auto* r : {&wrong, &expired, &unknown, &foreign, &malformed}) {
        EXPECT_FALSE(r->ok);
        EXPECT_EQ(r->token_id, 0);
    }
    // The audit trail gets the real reason; the HTTP answer never does.
    EXPECT_EQ(wrong.reason, ClaimFailure::NoMatch);
    EXPECT_EQ(expired.reason, ClaimFailure::Expired);
    EXPECT_EQ(unknown.reason, ClaimFailure::UnknownPlayer);
    EXPECT_EQ(foreign.reason, ClaimFailure::NoMatch);
    EXPECT_EQ(malformed.reason, ClaimFailure::NoMatch);
}

TEST_F(PlayersDbTest, AFailedAttemptDoesNotBurnTheToken) {
    const auto c = create();
    EXPECT_FALSE(db_->claimToken(c.player.id, std::string(64, 'f'), kT0 + 1).ok);
    EXPECT_TRUE(db_->claimToken(c.player.id, c.token, kT0 + 2).ok);
}

TEST_F(PlayersDbTest, ATokenOfARevokedPlayerIsRefused) {
    const auto c = create();
    ASSERT_TRUE(db_->revokePlayer(c.player.id, "gone", kT0 + 1));
    auto r = db_->claimToken(c.player.id, c.token, kT0 + 2);
    EXPECT_FALSE(r.ok);
    // Revoking removes the player's unused tokens, so there is nothing to match.
    EXPECT_EQ(r.reason, ClaimFailure::NoMatch);
}

TEST_F(PlayersDbTest, ReleasingAClaimMakesTheTokenUsableAgain) {
    const auto c = create();
    auto claim = db_->claimToken(c.player.id, c.token, kT0 + 1);
    ASSERT_TRUE(claim.ok);
    // The CSR turned out to be unusable: the player may try again.
    ASSERT_TRUE(db_->releaseClaim(claim.token_id));
    EXPECT_TRUE(db_->claimToken(c.player.id, c.token, kT0 + 2).ok);
}

TEST_F(PlayersDbTest, ConcurrentAttemptsWithOneTokenHaveExactlyOneWinner) {
    const auto c = create();
    std::atomic<int> winners{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i)
        threads.emplace_back([&] {
            if (db_->claimToken(c.player.id, c.token, kT0 + 5).ok) ++winners;
        });
    for (auto& t : threads) t.join();
    EXPECT_EQ(winners.load(), 1);
}

TEST_F(PlayersDbTest, ANewTokenReplacesTheOldOneForAPendingPlayer) {
    const auto c = create();
    auto fresh = db_->reissueToken(c.player.id, 3600, kT0 + 10);
    ASSERT_TRUE(fresh.has_value());
    EXPECT_NE(fresh->token, c.token);
    EXPECT_EQ(fresh->token_expires_at, kT0 + 10 + 3600);
    EXPECT_FALSE(db_->claimToken(c.player.id, c.token, kT0 + 11).ok) << "the old token is dead";
    EXPECT_TRUE(db_->claimToken(c.player.id, fresh->token, kT0 + 12).ok);
}

TEST_F(PlayersDbTest, ATokenCannotBeReissuedForAPlayerThatIsAlreadyPaired) {
    const auto id = enrolled("CC01");
    EXPECT_FALSE(db_->reissueToken(id, 3600, kT0 + 100).has_value());
}

// ── binding an already paired screen again ──────────────────────────────────
TEST_F(PlayersDbTest, ReEnrollingAnActiveScreenRevokesItsCertificatesAndReturnsItToPending) {
    const auto id = enrolled("RE01");
    ASSERT_TRUE(db_->recordRenewal(cert(id, "RE02"), kT0 + 200));
    const auto before = db_->findPlayer(id);
    ASSERT_TRUE(before.has_value());

    auto r = db_->reEnrollPlayer(id, "device_replaced", 3600, kT0 + 300);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->previous, PlayerState::Active);
    EXPECT_EQ(r->revoked.size(), 2u) << "both live certificates, so the CRL can be rebuilt";
    for (const auto& c : r->revoked) EXPECT_EQ(c.revoke_reason, "device_replaced");
    EXPECT_EQ(r->token.token_expires_at, kT0 + 300 + 3600);

    const auto p = db_->findPlayer(id);
    ASSERT_TRUE(p);
    EXPECT_EQ(p->state, PlayerState::Pending);
    EXPECT_EQ(p->name, before->name) << "the identity and the name stay";
    EXPECT_FALSE(p->activated_at.has_value());
    EXPECT_FALSE(p->revoked_at.has_value());
    EXPECT_TRUE(p->revoke_reason.empty());
    for (const auto& c : db_->certsOf(id)) EXPECT_TRUE(c.revoked_at.has_value());
    EXPECT_EQ(db_->revokedSerials(1, kT0 + 300).size(), 2u);
}

TEST_F(PlayersDbTest, ReEnrollingARevokedScreenBringsItBackAndKeepsTheFirstRevocationOfItsCertificates) {
    const auto id = enrolled("RE03");
    ASSERT_TRUE(db_->revokePlayer(id, "stolen", kT0 + 100));
    auto r = db_->reEnrollPlayer(id, "bought_back", 3600, kT0 + 300);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->previous, PlayerState::Revoked);
    EXPECT_TRUE(r->revoked.empty()) << "nothing new to revoke";
    const auto certs = db_->certsOf(id);
    ASSERT_EQ(certs.size(), 1u);
    EXPECT_EQ(certs[0].revoke_reason, "stolen") << "the first reason stays on the old certificate";
    EXPECT_EQ(db_->findPlayer(id)->state, PlayerState::Pending);
    EXPECT_TRUE(db_->findPlayer(id)->revoke_reason.empty());
}

TEST_F(PlayersDbTest, ReEnrollingGivesATokenThatEnrolsTheSameIdentityAgain) {
    const auto id = enrolled("RE04");
    auto r = db_->reEnrollPlayer(id, "x", 3600, kT0 + 300);
    ASSERT_TRUE(r.has_value());
    auto claim = db_->claimToken(id, r->token.token, kT0 + 310);
    ASSERT_TRUE(claim.ok);
    ASSERT_TRUE(db_->finishEnrollment(claim.token_id, cert(id, "RE05", 1), kT0 + 311));
    const auto p = db_->findPlayer(id);
    EXPECT_EQ(p->state, PlayerState::Active);
    EXPECT_EQ(p->activated_at, kT0 + 311);
    EXPECT_EQ(db_->certsOf(id).size(), 2u) << "the old one (revoked) and the new one";
}

TEST_F(PlayersDbTest, ReEnrollingKillsEarlierTokensStatusAndWaitingCommands) {
    const auto id = enrolled("RE06");
    ASSERT_TRUE(db_->applyStatuses({{id, kT0 + 20, true, "1.0.0", "{}"}}));
    CommandRow c;
    c.id = PlayersDb::newId(); c.player_id = id; c.type = "reboot_screen";
    c.issued_at = kT0 + 30; c.ttl_seconds = 300; c.deadline_at = kT0 + 1200;
    ASSERT_EQ(db_->createCommand(c), CommandCreate::Ok);

    ASSERT_TRUE(db_->reEnrollPlayer(id, "x", 3600, kT0 + 300).has_value());

    EXPECT_FALSE(db_->status(id).has_value()) << "what the old device said is not about the next one";
    const auto cmd = db_->findCommand(c.id);
    ASSERT_TRUE(cmd);
    EXPECT_EQ(cmd->state, "released");
    EXPECT_EQ(cmd->result, "re_enrolled");
    // The same command can be given to the new device at once: nothing blocks it.
    CommandRow again = c;
    again.id = PlayersDb::newId();
    EXPECT_EQ(db_->createCommand(again), CommandCreate::Ok);
}

TEST_F(PlayersDbTest, ReEnrollingWritesOneEventAndAnOldTokenStaysDead) {
    const auto c0 = create();
    auto claim = db_->claimToken(c0.player.id, c0.token, kT0 + 10);
    ASSERT_TRUE(db_->finishEnrollment(claim.token_id, cert(c0.player.id, "RE07"), kT0 + 11));
    auto r = db_->reEnrollPlayer(c0.player.id, "why", 3600, kT0 + 300);
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(db_->claimToken(c0.player.id, c0.token, kT0 + 310).ok);
    const auto ev = db_->events(c0.player.id, 10);
    ASSERT_FALSE(ev.empty());
    EXPECT_EQ(ev.front().kind, "re_enrolled");
    EXPECT_NE(ev.front().details.find("\"previous_state\":\"active\""), std::string::npos);
    EXPECT_EQ(ev.front().details.find(r->token.token), std::string::npos) << "the token never reaches the journal";
}

TEST_F(PlayersDbTest, ReEnrollingRefusesAPendingOrUnknownScreenAndChangesNothing) {
    const auto c = create();
    EXPECT_FALSE(db_->reEnrollPlayer(c.player.id, "x", 3600, kT0 + 5).has_value()) << "pending: reissueToken is for that";
    EXPECT_TRUE(db_->claimToken(c.player.id, c.token, kT0 + 6).ok) << "its own token still works";
    EXPECT_FALSE(db_->reEnrollPlayer("00000000-0000-4000-8000-000000000000", "x", 3600, kT0).has_value());
    EXPECT_FALSE(db_->reEnrollPlayer(enrolled("RE08"), "x", 0, kT0 + 5).has_value()) << "a bad token lifetime";
    EXPECT_EQ(db_->findPlayer(c.player.id)->state, PlayerState::Pending);
}

// ── enrollment ──────────────────────────────────────────────────────────────
TEST_F(PlayersDbTest, FinishingEnrollmentActivatesTheRecordsTheCertificateAndAnEvent) {
    const auto c = create();
    auto claim = db_->claimToken(c.player.id, c.token, kT0 + 10);
    ASSERT_TRUE(claim.ok);
    ASSERT_TRUE(db_->finishEnrollment(claim.token_id, cert(c.player.id, "DD01"), kT0 + 11));

    auto p = db_->findPlayer(c.player.id);
    ASSERT_TRUE(p);
    EXPECT_EQ(p->state, PlayerState::Active);
    EXPECT_EQ(p->activated_at, kT0 + 11);

    auto certs = db_->certsOf(c.player.id);
    ASSERT_EQ(certs.size(), 1u);
    EXPECT_EQ(certs[0].serial_hex, "DD01");
    EXPECT_FALSE(certs[0].revoked_at.has_value());

    auto ev = db_->events(c.player.id, 10);
    ASSERT_FALSE(ev.empty());
    EXPECT_EQ(ev.front().kind, "enrolled");
}

TEST_F(PlayersDbTest, EnrollmentFailsWithoutTraceIfThePlayerWasRevokedMeanwhile) {
    const auto c = create();
    auto claim = db_->claimToken(c.player.id, c.token, kT0 + 10);
    ASSERT_TRUE(claim.ok);
    ASSERT_TRUE(db_->revokePlayer(c.player.id, "revoked while the CSR was being signed", kT0 + 11));

    EXPECT_FALSE(db_->finishEnrollment(claim.token_id, cert(c.player.id, "DD02"), kT0 + 12));
    EXPECT_TRUE(db_->certsOf(c.player.id).empty()) << "no certificate may be recorded";
    EXPECT_EQ(db_->findPlayer(c.player.id)->state, PlayerState::Revoked);
}

TEST_F(PlayersDbTest, ATokenClaimCannotCompleteAnotherPlayersEnrollment) {
    const auto a = create("a"), b = create("b");
    auto claim = db_->claimToken(a.player.id, a.token, kT0 + 1);
    ASSERT_TRUE(claim.ok);
    EXPECT_FALSE(db_->finishEnrollment(claim.token_id, cert(b.player.id, "DD03"), kT0 + 2));
    EXPECT_EQ(db_->findPlayer(b.player.id)->state, PlayerState::Pending);
}

TEST_F(PlayersDbTest, RenewalAddsACertificateOnlyForAnActivePlayer) {
    const auto id = enrolled("EE01");
    ASSERT_TRUE(db_->recordRenewal(cert(id, "EE02"), kT0 + 200));
    EXPECT_EQ(db_->certsOf(id).size(), 2u);
    auto ev = db_->events(id, 10);
    bool renewed = false;
    for (const auto& e : ev) renewed |= (e.kind == "renewed");
    EXPECT_TRUE(renewed);

    const auto pending = create("p");
    EXPECT_FALSE(db_->recordRenewal(cert(pending.player.id, "EE03"), kT0 + 201));
    EXPECT_TRUE(db_->certsOf(pending.player.id).empty());
}

// ── certificates and the CRL ────────────────────────────────────────────────
TEST_F(PlayersDbTest, RevokingAPlayerRevokesItsCertificatesAndListsThemForTheCrl) {
    const auto id = enrolled("FF01");
    ASSERT_TRUE(db_->recordRenewal(cert(id, "FF02"), kT0 + 200));
    auto revoked = db_->revokePlayer(id, "stolen", kT0 + 300);
    ASSERT_TRUE(revoked.has_value());
    EXPECT_EQ(revoked->size(), 2u) << "both certificates, so the CRL can be rebuilt";

    EXPECT_EQ(db_->findPlayer(id)->state, PlayerState::Revoked);
    EXPECT_EQ(db_->findPlayer(id)->revoke_reason, "stolen");
    auto list = db_->revokedSerials(1, kT0 + 300);
    EXPECT_EQ(list.size(), 2u);
    for (const auto& r : list) EXPECT_EQ(r.revoke_reason, "stolen");
}

TEST_F(PlayersDbTest, RevokingTwiceIsHarmlessAndKeepsTheFirstReason) {
    const auto id = enrolled("FF03");
    ASSERT_TRUE(db_->revokePlayer(id, "first", kT0 + 300));
    auto again = db_->revokePlayer(id, "second", kT0 + 400);
    ASSERT_TRUE(again.has_value());
    EXPECT_TRUE(again->empty());
    EXPECT_EQ(db_->findPlayer(id)->revoke_reason, "first");
    EXPECT_EQ(db_->findPlayer(id)->revoked_at, kT0 + 300);
}

TEST_F(PlayersDbTest, RevokingAnUnknownPlayerReportsIt) {
    EXPECT_FALSE(db_->revokePlayer("00000000-0000-4000-8000-000000000000", "x", kT0).has_value());
}

TEST_F(PlayersDbTest, ASingleCertificateCanBeRevoked) {
    const auto id = enrolled("FF04");
    ASSERT_TRUE(db_->recordRenewal(cert(id, "FF05"), kT0 + 200));
    ASSERT_TRUE(db_->revokeCert(1, "FF04", "superseded", kT0 + 250));
    auto list = db_->revokedSerials(1, kT0 + 250);
    ASSERT_EQ(list.size(), 1u);
    EXPECT_EQ(list[0].serial_hex, "FF04");
    EXPECT_EQ(db_->findPlayer(id)->state, PlayerState::Active) << "the player itself is untouched";
}

TEST_F(PlayersDbTest, ExpiredCertificatesDropOutOfTheCrl) {
    const auto id = enrolled("AB01");
    ASSERT_TRUE(db_->revokePlayer(id, "x", kT0 + 300));
    EXPECT_EQ(db_->revokedSerials(1, kT0 + 300).size(), 1u);
    EXPECT_TRUE(db_->revokedSerials(1, kT0 + 366 * 86400).empty())
        << "a certificate past its notAfter needs no CRL entry";
}

TEST_F(PlayersDbTest, CrlListsAreSeparatePerGeneration) {
    const auto a = enrolled("AC01", /*gen=*/1, "a");
    const auto b = enrolled("AC01", /*gen=*/2, "b");   // same serial, other CA
    ASSERT_TRUE(db_->revokePlayer(a, "x", kT0 + 300));
    EXPECT_EQ(db_->revokedSerials(1, kT0 + 300).size(), 1u);
    EXPECT_TRUE(db_->revokedSerials(2, kT0 + 300).empty());
    (void)b;
}

TEST_F(PlayersDbTest, ASerialIsUniqueWithinAGenerationButNotAcrossGenerations) {
    const auto a = create("a"), b = create("b");
    auto ca = db_->claimToken(a.player.id, a.token, kT0 + 1);
    auto cb = db_->claimToken(b.player.id, b.token, kT0 + 1);
    ASSERT_TRUE(ca.ok && cb.ok);
    ASSERT_TRUE(db_->finishEnrollment(ca.token_id, cert(a.player.id, "AD01", 1), kT0 + 2));
    EXPECT_FALSE(db_->finishEnrollment(cb.token_id, cert(b.player.id, "AD01", 1), kT0 + 3));
    EXPECT_EQ(db_->findPlayer(b.player.id)->state, PlayerState::Pending) << "rolled back";
}

TEST_F(PlayersDbTest, CrlNumbersOnlyGoUpPerGenerationAndSurviveARestart) {
    EXPECT_EQ(db_->nextCrlNumber(1), 1);
    EXPECT_EQ(db_->nextCrlNumber(1), 2);
    EXPECT_EQ(db_->nextCrlNumber(2), 1) << "each generation counts on its own";
    reopen();
    EXPECT_EQ(db_->nextCrlNumber(1), 3);
}

TEST_F(PlayersDbTest, PurgingDeadTokensRemovesUsedAndRunOutOnesAndNeverOneThatCanStillBeClaimed) {
    const std::int64_t kDay = 86400;
    // Used long ago: the token of a screen that is enrolled.
    const auto used = create("used");
    auto cu = db_->claimToken(used.player.id, used.token, kT0 + 1);
    ASSERT_TRUE(cu.ok);
    ASSERT_TRUE(db_->finishEnrollment(cu.token_id, cert(used.player.id, "AF01", 1), kT0 + 2));
    // Never used, ran out long ago: a package that was never taken to a screen.
    const auto lapsed = db_->createPlayer("lapsed", std::nullopt, "strict", 3600, kT0, std::nullopt);
    ASSERT_TRUE(lapsed.has_value());
    // Never used, still good a hundred days on (a token lives a year at most).
    const auto fresh = db_->createPlayer("fresh", std::nullopt, "strict", 300 * kDay, kT0, std::nullopt);
    ASSERT_TRUE(fresh.has_value());

    const std::int64_t cutoff = kT0 + 100 * kDay;
    EXPECT_EQ(db_->purgeDeadTokens(cutoff), 2u) << "the used one and the one that ran out";
    EXPECT_EQ(db_->purgeDeadTokens(cutoff), 0u) << "nothing more to remove";

    // What is left still behaves: the live token can be claimed, the removed ones are simply unknown.
    EXPECT_TRUE(db_->claimToken(fresh->player.id, fresh->token, cutoff).ok);
    EXPECT_EQ(db_->claimToken(lapsed->player.id, lapsed->token, cutoff).reason, ClaimFailure::NoMatch);
}

TEST_F(PlayersDbTest, ATokenIsKeptUntilTheMarginHasPassedSoALateAttemptIsStillClassified) {
    const std::int64_t kDay = 86400;
    const auto p = db_->createPlayer("p", std::nullopt, "strict", 3600, kT0, std::nullopt);
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(db_->purgeDeadTokens(kT0 + 3600 - 1), 0u) << "not run out yet";
    EXPECT_EQ(db_->purgeDeadTokens(kT0 + 3600 - kDay), 0u);
    EXPECT_EQ(db_->claimToken(p->player.id, p->token, kT0 + 3600 + 10).reason, ClaimFailure::Expired);
}

TEST_F(PlayersDbTest, PurgingExpiredCertificatesKeepsTheLiveOnes) {
    const auto id = enrolled("AE01");
    ASSERT_TRUE(db_->recordRenewal(cert(id, "AE02", 1, kT0, kT0 + 1000), kT0 + 5));
    EXPECT_EQ(db_->purgeExpiredCerts(kT0 + 2000), 1u);
    auto left = db_->certsOf(id);
    ASSERT_EQ(left.size(), 1u);
    EXPECT_EQ(left[0].serial_hex, "AE01");
}

// ── last known state and events ─────────────────────────────────────────────
TEST_F(PlayersDbTest, StatusesAreWrittenInABatchAndUpdatedInPlace) {
    const auto a = enrolled("AF01", 1, "a"), b = enrolled("AF02", 1, "b");
    ASSERT_TRUE(db_->applyStatuses({{a, kT0 + 10, true, "1.0.0", R"({"cpu":3})"},
                                    {b, kT0 + 10, true, "1.0.0", "{}"}}));
    ASSERT_TRUE(db_->applyStatuses({{a, kT0 + 40, false, "1.1.0", R"({"cpu":9})"}}));

    auto sa = db_->status(a);
    ASSERT_TRUE(sa);
    EXPECT_EQ(sa->updated_at, kT0 + 40);
    EXPECT_FALSE(sa->online);
    EXPECT_EQ(sa->version, "1.1.0");
    EXPECT_EQ(sa->payload_json, R"({"cpu":9})");
    EXPECT_EQ(db_->allStatuses().size(), 2u);
}

TEST_F(PlayersDbTest, AnOversizedStatusPayloadIsRefusedNotTruncated) {
    const auto a = enrolled("AF03");
    EXPECT_FALSE(db_->applyStatuses({{a, kT0, true, "1", std::string(70'000, 'x')}}));
    EXPECT_FALSE(db_->status(a).has_value());
}

TEST_F(PlayersDbTest, AStatusForAnUnknownPlayerIsIgnoredWithoutFailingTheBatch) {
    const auto a = enrolled("AF04");
    ASSERT_TRUE(db_->applyStatuses({{"00000000-0000-4000-8000-000000000000", kT0, true, "1", "{}"},
                                    {a, kT0, true, "1", "{}"}}));
    EXPECT_TRUE(db_->status(a).has_value());
}

TEST_F(PlayersDbTest, EventsAreListedNewestFirstAndPurgedByAge) {
    const auto id = enrolled("AG01");
    ASSERT_TRUE(db_->addEvent(id, "online", "", kT0 + 100));
    ASSERT_TRUE(db_->addEvent(id, "offline", "heartbeat lost", kT0 + 200));
    auto ev = db_->events(id, 2);
    ASSERT_EQ(ev.size(), 2u);
    EXPECT_EQ(ev[0].kind, "offline");
    EXPECT_EQ(ev[0].details, "heartbeat lost");
    EXPECT_EQ(ev[1].kind, "online");

    EXPECT_EQ(db_->purgeEventsOlderThan(kT0 + 150), 2u) << "'enrolled' (t0+11) and 'online' (t0+100)";
    EXPECT_EQ(db_->events(id, 10).size(), 1u);
}

TEST_F(PlayersDbTest, EventsOutliveTheirPlayerUntilRetentionRemovesThem) {
    const auto id = enrolled("AG02");
    ASSERT_TRUE(db_->deletePlayer(id, "gone", kT0 + 500));
    EXPECT_FALSE(db_->events(id, 10).empty()) << "history stays for the audit";
    db_->purgeEventsOlderThan(kT0 + 10'000);
    EXPECT_TRUE(db_->events(id, 10).empty());
}

// ── settings and the version 1 -> 2 migration ───────────────────────────────
TEST_F(PlayersDbTest, SettingsStartEmpty) {
    EXPECT_TRUE(db_->settings().empty());
}

TEST_F(PlayersDbTest, SettingsAreSavedTogetherAndSurviveReopening) {
    ASSERT_TRUE(db_->setSettings({{"server_url", "https://liveqx.example:8443"},
                                  {"mqtt_broker_url", "mqtts://liveqx.example:8883"}}));
    reopen();
    auto s = db_->settings();
    ASSERT_EQ(s.size(), 2u);
    EXPECT_EQ(s["server_url"], "https://liveqx.example:8443");
    EXPECT_EQ(s["mqtt_broker_url"], "mqtts://liveqx.example:8883");
}

TEST_F(PlayersDbTest, SavingSettingsReplacesTheValuesButKeepsTheOthers) {
    ASSERT_TRUE(db_->setSettings({{"a", "1"}, {"b", "2"}}));
    ASSERT_TRUE(db_->setSettings({{"b", "20"}, {"c", "3"}}));
    auto s = db_->settings();
    EXPECT_EQ(s["a"], "1");
    EXPECT_EQ(s["b"], "20");
    EXPECT_EQ(s["c"], "3");
}

TEST_F(PlayersDbTest, AnEmptyValueRemovesTheSetting) {
    ASSERT_TRUE(db_->setSettings({{"server_url", "https://x.example"}}));
    ASSERT_TRUE(db_->setSettings({{"server_url", ""}}));
    EXPECT_EQ(db_->settings().count("server_url"), 0u);
}

TEST_F(PlayersDbTest, AnOversizedSettingIsRefusedAndNothingIsWritten) {
    EXPECT_FALSE(db_->setSettings({{"ok", "1"}, {"big", std::string(40'000, 'x')}}));
    EXPECT_TRUE(db_->settings().empty()) << "all or nothing";
}

TEST_F(PlayersDbTest, ADatabaseOfVersionOneIsMigratedKeepingItsData) {
    // Build what a version-1 installation looks like: real data, no settings
    // table, user_version 1.
    const auto id = enrolled("MIG01");
    ASSERT_TRUE(db_->nextCrlNumber(1) > 0);
    db_.reset();
    {
        sqlite3* raw = nullptr;
        ASSERT_EQ(sqlite3_open(path_.c_str(), &raw), SQLITE_OK);
        ASSERT_EQ(sqlite3_exec(raw, "DROP INDEX idx_players_group; ALTER TABLE players DROP COLUMN group_id;"
                               "DROP TABLE command_job_items; DROP TABLE command_jobs; DROP TABLE player_commands; DROP TABLE player_groups; DROP TABLE player_objects;"
                               "DROP TABLE players_settings; PRAGMA user_version = 1;",
                               nullptr, nullptr, nullptr), SQLITE_OK);
        sqlite3_close(raw);
    }

    PlayersDb upgraded(path_);
    ASSERT_TRUE(upgraded.open());
    EXPECT_EQ(upgraded.schemaVersion(), PlayersDb::kSchemaVersion);
    ASSERT_TRUE(upgraded.findPlayer(id).has_value()) << "players survive the migration";
    EXPECT_EQ(upgraded.certsOf(id).size(), 1u);
    EXPECT_EQ(upgraded.nextCrlNumber(1), 2) << "CRL numbers must not restart";
    EXPECT_TRUE(upgraded.setSettings({{"server_url", "https://x.example"}}))
        << "the new table must exist after the migration";
}

TEST_F(PlayersDbTest, MigrationIsIdempotent) {
    reopen();
    reopen();
    EXPECT_EQ(db_->schemaVersion(), PlayersDb::kSchemaVersion);
}

// ── all revoked certificates, whatever the CA generation ────────────────────
TEST_F(PlayersDbTest, RevokedCertsListsEveryGenerationButNotLiveOnesOrExpiredOnes) {
    const auto a = enrolled("A100", /*gen=*/1, "A");
    const auto b = enrolled("B200", /*gen=*/2, "B");
    const auto live = enrolled("C300", /*gen=*/1, "C");
    ASSERT_TRUE(db_->revokePlayer(a, "x", kT0 + 20));
    ASSERT_TRUE(db_->revokePlayer(b, "y", kT0 + 21));

    auto all = db_->revokedCerts(kT0 + 30);
    ASSERT_EQ(all.size(), 2u);
    std::set<std::string> serials;
    for (const auto& c : all) serials.insert(c.serial_hex);
    EXPECT_EQ(serials, (std::set<std::string>{"A100", "B200"}));
    EXPECT_FALSE(serials.count("C300")) << "a live certificate is not revoked";
    (void)live;

    // Past its notAfter a revoked certificate no longer matters (it is expired anyway).
    EXPECT_TRUE(db_->revokedCerts(kT0 + 400LL * 86400).empty());
}

// ── objects and groups (schema version 3) ───────────────────────────────────
// The hierarchy is all -> object -> group; a player sits in exactly one group.
TEST_F(PlayersDbTest, ObjectsAndGroupsAreCreatedListedAndFound) {
    GroupResult err = GroupResult::Ok;
    auto lobby = db_->createObject("Mall", &err, kT0);
    ASSERT_TRUE(lobby.has_value());
    EXPECT_EQ(err, GroupResult::Ok);
    EXPECT_GT(lobby->id, 0);
    EXPECT_EQ(lobby->name, "Mall");

    auto hall = db_->createGroup(lobby->id, "Entrance hall", &err, kT0);
    ASSERT_TRUE(hall.has_value());
    EXPECT_EQ(hall->object_id, lobby->id);

    EXPECT_EQ(db_->findObject(lobby->id)->name, "Mall");
    EXPECT_EQ(db_->findGroup(hall->id)->name, "Entrance hall");
    ASSERT_EQ(db_->listObjects().size(), 1u);
    ASSERT_EQ(db_->listGroups(lobby->id).size(), 1u);
    EXPECT_EQ(db_->listGroups(std::nullopt).size(), 1u) << "no filter = every group";
    EXPECT_FALSE(db_->findObject(9999).has_value());
    EXPECT_FALSE(db_->findGroup(9999).has_value());
}

TEST_F(PlayersDbTest, NamesAreCleanedValidatedAndUniqueWhereTheyMust) {
    GroupResult err;
    ASSERT_TRUE(db_->createObject("  Mall  ", &err));
    EXPECT_EQ(db_->listObjects()[0].name, "Mall") << "surrounding spaces are trimmed";

    EXPECT_FALSE(db_->createObject("Mall", &err));
    EXPECT_EQ(err, GroupResult::NameTaken) << "an object name is unique everywhere";
    for (const std::string& bad : {std::string(""), std::string("   "), std::string(121, 'x'),
                                   std::string("a\nb"), std::string("a\x01" "b")}) {
        EXPECT_FALSE(db_->createObject(bad, &err)) << bad.size();
        EXPECT_EQ(err, GroupResult::InvalidName);
    }

    auto a = db_->createObject("A", &err);
    auto b = db_->createObject("B", &err);
    ASSERT_TRUE(a && b);
    ASSERT_TRUE(db_->createGroup(a->id, "Hall", &err));
    EXPECT_FALSE(db_->createGroup(a->id, "Hall", &err));
    EXPECT_EQ(err, GroupResult::NameTaken) << "a group name is unique within its object";
    EXPECT_TRUE(db_->createGroup(b->id, "Hall", &err)) << "...but may repeat in another object";
    EXPECT_FALSE(db_->createGroup(9999, "Hall", &err));
    EXPECT_EQ(err, GroupResult::NotFound);
}

TEST_F(PlayersDbTest, RenamingKeepsTheIdAndRespectsUniqueness) {
    GroupResult err;
    auto a = db_->createObject("A", &err);
    auto b = db_->createObject("B", &err);
    auto g1 = db_->createGroup(a->id, "G1", &err);
    auto g2 = db_->createGroup(a->id, "G2", &err);
    ASSERT_TRUE(a && b && g1 && g2);

    EXPECT_EQ(db_->renameObject(a->id, "A2"), GroupResult::Ok);
    EXPECT_EQ(db_->findObject(a->id)->name, "A2");
    EXPECT_EQ(db_->renameObject(a->id, "B"), GroupResult::NameTaken);
    EXPECT_EQ(db_->renameObject(a->id, "A2"), GroupResult::Ok) << "renaming to itself is fine";
    EXPECT_EQ(db_->renameObject(9999, "X"), GroupResult::NotFound);
    EXPECT_EQ(db_->renameObject(a->id, ""), GroupResult::InvalidName);

    EXPECT_EQ(db_->renameGroup(g1->id, "G1b"), GroupResult::Ok);
    EXPECT_EQ(db_->renameGroup(g1->id, "G2"), GroupResult::NameTaken);
    EXPECT_EQ(db_->renameGroup(9999, "X"), GroupResult::NotFound);
}

TEST_F(PlayersDbTest, ThingsThatStillHoldSomethingCannotBeDeleted) {
    GroupResult err;
    auto obj = db_->createObject("Mall", &err);
    auto grp = db_->createGroup(obj->id, "Hall", &err);
    ASSERT_TRUE(obj && grp);
    auto p = db_->createPlayer("Screen", 1, "strict", 86400, kT0, grp->id);
    ASSERT_TRUE(p);

    EXPECT_EQ(db_->deleteObject(obj->id), GroupResult::NotEmpty) << "it still has a group";
    EXPECT_EQ(db_->deleteGroup(grp->id), GroupResult::NotEmpty) << "it still has a player";
    EXPECT_TRUE(db_->findGroup(grp->id).has_value());

    auto other = db_->createGroup(obj->id, "Cafe", &err);
    ASSERT_TRUE(other);
    ASSERT_EQ(db_->movePlayer(p->player.id, other->id, kT0 + 1), GroupResult::Ok);
    EXPECT_EQ(db_->deleteGroup(grp->id), GroupResult::Ok) << "empty now";
    EXPECT_EQ(db_->deleteGroup(grp->id), GroupResult::NotFound);
    EXPECT_EQ(db_->deleteObject(obj->id), GroupResult::NotEmpty) << "one group is left";
    ASSERT_TRUE(db_->deletePlayer(p->player.id, "x", kT0 + 2));
    EXPECT_EQ(db_->deleteGroup(other->id), GroupResult::Ok);
    EXPECT_EQ(db_->deleteObject(obj->id), GroupResult::Ok);
    EXPECT_EQ(db_->deleteObject(obj->id), GroupResult::NotFound);
}

TEST_F(PlayersDbTest, IdsOfDeletedGroupsAreNeverReused) {
    // Access grants refer to ids. If a deleted group's id came back for a new
    // group, a stale grant would silently apply to it.
    GroupResult err;
    auto obj = db_->createObject("Mall", &err);
    auto g1 = db_->createGroup(obj->id, "G1", &err);
    ASSERT_TRUE(g1);
    ASSERT_EQ(db_->deleteGroup(g1->id), GroupResult::Ok);
    auto g2 = db_->createGroup(obj->id, "G2", &err);
    ASSERT_TRUE(g2);
    EXPECT_NE(g2->id, g1->id);

    ASSERT_EQ(db_->deleteGroup(g2->id), GroupResult::Ok);
    ASSERT_EQ(db_->deleteObject(obj->id), GroupResult::Ok);
    auto obj2 = db_->createObject("Mall", &err);
    ASSERT_TRUE(obj2);
    EXPECT_NE(obj2->id, obj->id);
}

TEST_F(PlayersDbTest, APlayerCanBeCreatedIntoAGroupAndOnlyIntoAnExistingOne) {
    GroupResult err;
    auto obj = db_->createObject("Mall", &err);
    auto grp = db_->createGroup(obj->id, "Hall", &err);
    ASSERT_TRUE(obj && grp);

    auto p = db_->createPlayer("Screen", 1, "strict", 86400, kT0, grp->id);
    ASSERT_TRUE(p);
    ASSERT_TRUE(p->player.group_id.has_value());
    EXPECT_EQ(*p->player.group_id, grp->id);
    EXPECT_EQ(db_->findPlayer(p->player.id)->group_id, std::optional<std::int64_t>(grp->id));

    EXPECT_FALSE(db_->createPlayer("Ghost", 1, "strict", 86400, kT0, 9999).has_value())
        << "a group that does not exist is refused";
    EXPECT_EQ(db_->listPlayers({}).size(), 1u) << "and leaves nothing behind";
}

TEST_F(PlayersDbTest, PlayersAreListedByGroupAndByObject) {
    GroupResult err;
    auto o1 = db_->createObject("O1", &err);
    auto o2 = db_->createObject("O2", &err);
    auto g1 = db_->createGroup(o1->id, "G1", &err);
    auto g2 = db_->createGroup(o1->id, "G2", &err);
    auto g3 = db_->createGroup(o2->id, "G3", &err);
    ASSERT_TRUE(g1 && g2 && g3);
    ASSERT_TRUE(db_->createPlayer("a", 1, "strict", 86400, kT0, g1->id));
    ASSERT_TRUE(db_->createPlayer("b", 1, "strict", 86400, kT0 + 1, g1->id));
    ASSERT_TRUE(db_->createPlayer("c", 1, "strict", 86400, kT0 + 2, g2->id));
    ASSERT_TRUE(db_->createPlayer("d", 1, "strict", 86400, kT0 + 3, g3->id));

    ListFilter f;
    f.group_id = g1->id;
    EXPECT_EQ(db_->listPlayers(f).size(), 2u);
    f.group_id = std::nullopt;
    f.object_id = o1->id;
    EXPECT_EQ(db_->listPlayers(f).size(), 3u) << "an object includes all its groups";
    f.object_id = o2->id;
    EXPECT_EQ(db_->listPlayers(f).size(), 1u);
    f.object_id = std::nullopt;
    f.state = PlayerState::Active;
    EXPECT_TRUE(db_->listPlayers(f).empty()) << "filters combine";

    auto counts = db_->playerCountByGroup();
    EXPECT_EQ(counts[g1->id], 2);
    EXPECT_EQ(counts[g2->id], 1);
    EXPECT_EQ(counts[g3->id], 1);
}

TEST_F(PlayersDbTest, MovingAPlayerChangesItsGroupAndLeavesATrace) {
    GroupResult err;
    auto obj = db_->createObject("Mall", &err);
    auto g1 = db_->createGroup(obj->id, "G1", &err);
    auto g2 = db_->createGroup(obj->id, "G2", &err);
    auto p = db_->createPlayer("Screen", 1, "strict", 86400, kT0, g1->id);
    ASSERT_TRUE(g1 && g2 && p);

    EXPECT_EQ(db_->movePlayer(p->player.id, g2->id, kT0 + 5), GroupResult::Ok);
    EXPECT_EQ(db_->findPlayer(p->player.id)->group_id, std::optional<std::int64_t>(g2->id));
    int moved = 0;
    std::string details;
    for (const auto& e : db_->events(p->player.id, 20))
        if (e.kind == "moved") { ++moved; details = e.details; }
    EXPECT_EQ(moved, 1);
    EXPECT_NE(details.find("\"from\":" + std::to_string(g1->id)), std::string::npos) << details;
    EXPECT_NE(details.find("\"to\":" + std::to_string(g2->id)), std::string::npos) << details;

    EXPECT_EQ(db_->movePlayer(p->player.id, g2->id, kT0 + 6), GroupResult::Ok) << "already there: nothing to do";
    moved = 0;
    for (const auto& e : db_->events(p->player.id, 20)) moved += (e.kind == "moved");
    EXPECT_EQ(moved, 1) << "no second trace for a no-op";

    EXPECT_EQ(db_->movePlayer(p->player.id, 9999, kT0 + 7), GroupResult::NotFound);
    EXPECT_EQ(db_->movePlayer("00000000-0000-4000-8000-000000000000", g1->id, kT0 + 7), GroupResult::NotFound);
    EXPECT_EQ(db_->findPlayer(p->player.id)->group_id, std::optional<std::int64_t>(g2->id)) << "failed moves change nothing";
}

TEST_F(PlayersDbTest, TheHierarchySurvivesReopening) {
    GroupResult err;
    auto obj = db_->createObject("Mall", &err);
    auto grp = db_->createGroup(obj->id, "Hall", &err);
    auto p = db_->createPlayer("Screen", 1, "strict", 86400, kT0, grp->id);
    ASSERT_TRUE(obj && grp && p);
    reopen();
    EXPECT_EQ(db_->findGroup(grp->id)->object_id, obj->id);
    EXPECT_EQ(db_->findPlayer(p->player.id)->group_id, std::optional<std::int64_t>(grp->id));
}

TEST_F(PlayersDbTest, ADatabaseOfVersionTwoIsMigratedAndItsPlayersGetAPlace) {
    // What an installation from before groups looks like: players, no
    // objects or groups, no group_id column, user_version 2.
    auto legacy = enrolled("MIG02");
    auto legacy_cert_count = db_->certsOf(legacy).size();
    db_.reset();
    {
        sqlite3* raw = nullptr;
        ASSERT_EQ(sqlite3_open(path_.c_str(), &raw), SQLITE_OK);
        ASSERT_EQ(sqlite3_exec(raw,
            "DROP INDEX idx_players_group; ALTER TABLE players DROP COLUMN group_id;"
            "DROP TABLE command_job_items; DROP TABLE command_jobs; DROP TABLE player_commands; DROP TABLE player_groups; DROP TABLE player_objects; PRAGMA user_version = 2;",
            nullptr, nullptr, nullptr), SQLITE_OK);
        sqlite3_close(raw);
    }

    PlayersDb upgraded(path_);
    ASSERT_TRUE(upgraded.open());
    EXPECT_EQ(upgraded.schemaVersion(), PlayersDb::kSchemaVersion);
    auto p = upgraded.findPlayer(legacy);
    ASSERT_TRUE(p.has_value()) << "players survive";
    EXPECT_EQ(upgraded.certsOf(legacy).size(), legacy_cert_count);
    ASSERT_TRUE(p->group_id.has_value()) << "every player ends up in a group";
    auto g = upgraded.findGroup(*p->group_id);
    ASSERT_TRUE(g.has_value());
    EXPECT_EQ(g->name, "Unassigned");
    EXPECT_EQ(upgraded.findObject(g->object_id)->name, "Unassigned");
}

TEST_F(PlayersDbTest, AFreshOrEmptyDatabaseGetsNoInventedObjects) {
    EXPECT_TRUE(db_->listObjects().empty());
    EXPECT_TRUE(db_->listGroups(std::nullopt).empty());
}


// ── commands (schema v4) ────────────────────────────────────────────────────
TEST_F(PlayersDbTest, ADatabaseOfVersionThreeGainsTheCommandLogAndKeepsItsData) {
    const auto id = enrolled("MIG03");
    db_.reset();
    {
        sqlite3* raw = nullptr;
        ASSERT_EQ(sqlite3_open(path_.c_str(), &raw), SQLITE_OK);
        ASSERT_EQ(sqlite3_exec(raw, "DROP TABLE command_job_items; DROP TABLE command_jobs; DROP TABLE player_commands; PRAGMA user_version = 3;",
                               nullptr, nullptr, nullptr), SQLITE_OK);
        sqlite3_close(raw);
    }
    PlayersDb upgraded(path_);
    ASSERT_TRUE(upgraded.open());
    EXPECT_EQ(upgraded.schemaVersion(), PlayersDb::kSchemaVersion);
    EXPECT_TRUE(upgraded.findPlayer(id).has_value());
    CommandRow c;
    c.id = PlayersDb::newId(); c.player_id = id; c.type = "restart_player";
    c.issued_at = kT0; c.ttl_seconds = 300; c.deadline_at = kT0 + 300;
    EXPECT_EQ(upgraded.createCommand(c), CommandCreate::Ok);
}

TEST_F(PlayersDbTest, OnlyOneUnsettledCommandPerScreenAndTypeCanExist) {
    const auto id = enrolled("CMD01");
    CommandRow c;
    c.id = PlayersDb::newId(); c.player_id = id; c.type = "reboot_screen";
    c.issued_at = kT0; c.ttl_seconds = 300; c.deadline_at = kT0 + 1200;
    ASSERT_EQ(db_->createCommand(c), CommandCreate::Ok);

    CommandRow second = c;
    second.id = PlayersDb::newId();
    std::string busy;
    EXPECT_EQ(db_->createCommand(second, &busy), CommandCreate::Busy);
    EXPECT_EQ(busy, c.id);

    CommandRow other = second;
    other.type = "restart_player";
    EXPECT_EQ(db_->createCommand(other), CommandCreate::Ok) << "another type is another matter";

    ASSERT_TRUE(db_->settleCommand(c.id, "executed", "executed", kT0 + 10).has_value());
    EXPECT_FALSE(db_->settleCommand(c.id, "failed", "late", kT0 + 20).has_value()) << "settled once";
    EXPECT_EQ(db_->findCommand(c.id)->state, "executed");
    second.id = PlayersDb::newId();
    EXPECT_EQ(db_->createCommand(second), CommandCreate::Ok) << "settling frees the screen";
}

TEST_F(PlayersDbTest, TimeoutsTakeOnlyPendingCommandsPastTheirDeadline) {
    const auto id = enrolled("CMD02");
    CommandRow early, late, done;
    early.id = PlayersDb::newId(); early.player_id = id; early.type = "restart_player";
    early.issued_at = kT0; early.ttl_seconds = 300; early.deadline_at = kT0 + 100;
    late = early; late.id = PlayersDb::newId(); late.type = "reboot_screen"; late.deadline_at = kT0 + 900;
    done = early; done.id = PlayersDb::newId(); done.type = "reload_playlist"; done.deadline_at = kT0 + 50;
    ASSERT_EQ(db_->createCommand(early), CommandCreate::Ok);
    ASSERT_EQ(db_->createCommand(late), CommandCreate::Ok);
    ASSERT_EQ(db_->createCommand(done), CommandCreate::Ok);
    db_->settleCommand(done.id, "executed", "executed", kT0 + 10);

    auto t = db_->timeoutCommands(kT0 + 500);
    ASSERT_EQ(t.size(), 1u);
    EXPECT_EQ(t[0].id, early.id);
    EXPECT_EQ(db_->findCommand(early.id)->state, "timeout");
    EXPECT_EQ(db_->findCommand(late.id)->state, "pending");
    EXPECT_EQ(db_->findCommand(done.id)->state, "executed");
    EXPECT_TRUE(db_->timeoutCommands(kT0 + 500).empty()) << "nothing is timed out twice";
}

// ── bulk command jobs (schema v5) ───────────────────────────────────────────
TEST_F(PlayersDbTest, ADatabaseOfVersionFourGainsTheJobTablesAndKeepsItsCommands) {
    const auto id = enrolled("MIG04");
    CommandRow c;
    c.id = PlayersDb::newId(); c.player_id = id; c.type = "restart_player";
    c.issued_at = kT0; c.ttl_seconds = 300; c.deadline_at = kT0 + 300;
    ASSERT_EQ(db_->createCommand(c), CommandCreate::Ok);
    db_.reset();
    {
        sqlite3* raw = nullptr;
        ASSERT_EQ(sqlite3_open(path_.c_str(), &raw), SQLITE_OK);
        ASSERT_EQ(sqlite3_exec(raw, "DROP TABLE command_job_items; DROP TABLE command_jobs; PRAGMA user_version = 4;",
                               nullptr, nullptr, nullptr), SQLITE_OK);
        sqlite3_close(raw);
    }
    PlayersDb upgraded(path_);
    ASSERT_TRUE(upgraded.open());
    EXPECT_EQ(upgraded.schemaVersion(), PlayersDb::kSchemaVersion);
    EXPECT_TRUE(upgraded.findCommand(c.id).has_value()) << "earlier data survives";
    JobRow j;
    j.id = PlayersDb::newId(); j.type = "restart_player"; j.target_kind = "all"; j.target_name = "all";
    j.created_at = kT0;
    EXPECT_TRUE(upgraded.createJob(j, {}));
}

TEST_F(PlayersDbTest, AJobKeepsItsScreensInBatchesAndSurvivesAReopen) {
    JobRow j;
    j.id = PlayersDb::newId(); j.type = "reboot_screen"; j.target_kind = "group"; j.target_id = 3;
    j.target_name = "Main / Hall"; j.batch_size = 2; j.stop_percent = 30; j.created_at = kT0;
    j.created_by = 5; j.created_by_name = "boss"; j.created_by_role = "admin";
    std::vector<JobItem> items = {{"", "p1", 0, "queued", "", ""}, {"", "p2", 0, "queued", "", ""},
                                  {"", "p3", 1, "queued", "", ""}, {"", "p4", -1, "skipped_offline", "", ""}};
    ASSERT_TRUE(db_->createJob(j, items));
    ASSERT_TRUE(db_->updateJobItem(j.id, "p1", "sent", "cmd-1", ""));
    ASSERT_TRUE(db_->setNextBatch(j.id, 1));
    db_.reset();

    PlayersDb again(path_);
    ASSERT_TRUE(again.open());
    auto running = again.runningJobs();
    ASSERT_EQ(running.size(), 1u) << "a job in progress is still there after a restart";
    EXPECT_EQ(running[0].next_batch, 1);
    EXPECT_EQ(running[0].created_by_name, "boss");
    EXPECT_EQ(running[0].target_name, "Main / Hall");
    auto got = again.jobItems(j.id);
    ASSERT_EQ(got.size(), 4u);
    EXPECT_EQ(got[0].batch_no, -1) << "the screens that were absent come first";
    int sent = 0, queued = 0;
    for (const auto& it : got) { sent += it.state == "sent"; queued += it.state == "queued"; }
    EXPECT_EQ(sent, 1);
    EXPECT_EQ(queued, 2);
}

TEST_F(PlayersDbTest, FinishingAJobCancelsWhatWasQueuedAndHappensOnlyOnce) {
    JobRow j;
    j.id = PlayersDb::newId(); j.type = "restart_player"; j.target_kind = "all"; j.target_name = "all";
    j.batch_size = 1; j.stop_percent = 30; j.created_at = kT0;
    ASSERT_TRUE(db_->createJob(j, {{"", "p1", 0, "sent", "c1", ""}, {"", "p2", 1, "queued", "", ""}}));
    EXPECT_TRUE(db_->finishJob(j.id, "stopped", "too_many_unconfirmed", kT0 + 50));
    EXPECT_FALSE(db_->finishJob(j.id, "done", "", kT0 + 60)) << "already over";
    auto job = db_->findJob(j.id);
    EXPECT_EQ(job->state, "stopped");
    EXPECT_EQ(job->stop_reason, "too_many_unconfirmed");
    EXPECT_EQ(*job->finished_at, kT0 + 50);
    for (const auto& it : db_->jobItems(j.id)) {
        if (it.player_id == "p2") EXPECT_EQ(it.state, "cancelled");
        if (it.player_id == "p1") EXPECT_EQ(it.state, "sent") << "what already went out stays";
    }
    EXPECT_TRUE(db_->runningJobs().empty());
    EXPECT_FALSE(db_->setNextBatch(j.id, 9)) << "a finished job is not advanced";
}

TEST_F(PlayersDbTest, JobsAreListedNewestFirstAndCanBeLimitedToOnePerson) {
    for (int i = 0; i < 3; ++i) {
        JobRow j;
        j.id = PlayersDb::newId(); j.type = "restart_player"; j.target_kind = "all"; j.target_name = "all";
        j.batch_size = 10; j.stop_percent = 30; j.created_at = kT0 + i; j.created_by = (i == 1) ? 8 : 7;
        ASSERT_TRUE(db_->createJob(j, {}));
    }
    EXPECT_EQ(db_->listJobs(std::nullopt, 10).size(), 3u);
    auto mine = db_->listJobs(7, 10);
    ASSERT_EQ(mine.size(), 2u);
    EXPECT_GT(mine[0].created_at, mine[1].created_at);
    EXPECT_EQ(db_->listJobs(std::nullopt, 1).size(), 1u);
}
