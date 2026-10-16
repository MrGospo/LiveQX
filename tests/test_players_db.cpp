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
        ASSERT_EQ(sqlite3_exec(raw, "DROP TABLE players_settings; PRAGMA user_version = 1;",
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

