// players/RevocationList — the in-process list of revoked certificates (layer 1
// of the three-layer revocation, A9): it takes effect the moment a revoke
// call returns, with no file to write and no broker to wake.
//
// The database stays the source of truth: the list is rebuilt from it at
// startup, so a revocation is never lost with the process.
//
// What these tests protect:
//   * a certificate added by the revoke hook is refused at once;
//   * a restart (a new list built from the database) remembers every
//     revocation of every CA generation, and forgets expired ones;
//   * serial numbers compare the way the CA writes them (upper case hex, no
//     separators, no leading-zero games);
//   * readers and a writer can run together.

#include "players/RevocationList.h"

#include <gtest/gtest.h>

#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <thread>

using namespace liveqx::players;

namespace {

CertRow cert(const std::string& serial, int gen = 1) {
    CertRow c;
    c.serial_hex = serial;
    c.generation = gen;
    c.player_id  = "11111111-2222-4333-8444-555555555555";
    return c;
}

}  // namespace

TEST(RevocationList, StartsEmptyAndRevokesNothingUnlessAdded) {
    RevocationList l;
    EXPECT_EQ(l.size(), 0u);
    EXPECT_FALSE(l.isRevoked("AB01"));
}

TEST(RevocationList, AnAddedCertificateIsRevokedAtOnce) {
    RevocationList l;
    l.add({cert("AB01"), cert("CD02")});
    EXPECT_TRUE(l.isRevoked("AB01"));
    EXPECT_TRUE(l.isRevoked("CD02"));
    EXPECT_FALSE(l.isRevoked("EF03"));
    EXPECT_EQ(l.size(), 2u);
}

TEST(RevocationList, SerialsAreComparedInTheFormatTheCaWrites) {
    RevocationList l;
    l.add({cert("0AB01")});
    EXPECT_TRUE(l.isRevoked("0AB01"));
    EXPECT_TRUE(l.isRevoked("0ab01")) << "case does not matter";
    EXPECT_TRUE(l.isRevoked("AB01")) << "a leading zero does not make another certificate";
    EXPECT_FALSE(l.isRevoked("")) << "an empty serial matches nothing";
    EXPECT_FALSE(l.isRevoked("AB0"));
}

TEST(RevocationList, AddingTwiceIsHarmless) {
    RevocationList l;
    l.add({cert("AB01")});
    l.add({cert("AB01")});
    EXPECT_EQ(l.size(), 1u);
}

TEST(RevocationList, ReplaceAllSwapsTheWholeContent) {
    RevocationList l;
    l.add({cert("AB01")});
    l.replaceAll({cert("CD02", 2)});
    EXPECT_FALSE(l.isRevoked("AB01"));
    EXPECT_TRUE(l.isRevoked("CD02"));
}

TEST(RevocationList, CanBeRebuiltFromTheDatabaseAfterARestart) {
    // The same call the server makes at startup.
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / ("lqx-revlist-" + std::to_string(::getpid()));
    fs::create_directories(dir);
    {
        PlayersDb db(dir / "players.db");
        ASSERT_TRUE(db.open());
        constexpr std::int64_t kNow = 1'800'000'000;
        auto c = db.createPlayer("P", std::nullopt, "strict", 86400, kNow);
        ASSERT_TRUE(c);
        auto claim = db.claimToken(c->player.id, c->token, kNow + 1);
        ASSERT_TRUE(claim.ok);
        CertRow row = cert("ABCDEF", 3);
        row.player_id = c->player.id;
        row.issued_at = kNow; row.not_before = kNow - 86400; row.not_after = kNow + 365 * 86400;
        ASSERT_TRUE(db.finishEnrollment(claim.token_id, row, kNow + 2));
        ASSERT_TRUE(db.revokePlayer(c->player.id, "x", kNow + 3));

        RevocationList fresh;                        // "the process restarted"
        EXPECT_EQ(fresh.rebuildFrom(db, kNow + 10), 1u);
        EXPECT_TRUE(fresh.isRevoked("ABCDEF"));

        RevocationList later;
        EXPECT_EQ(later.rebuildFrom(db, kNow + 400LL * 86400), 0u) << "expired entries are dropped";
        EXPECT_FALSE(later.isRevoked("ABCDEF"));
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST(RevocationList, ReadersAndAWriterRunTogether) {
    RevocationList l;
    std::atomic<bool> stop{false};
    std::atomic<long> reads{0};
    std::vector<std::thread> readers;
    for (int i = 0; i < 4; ++i)
        readers.emplace_back([&] {
            while (!stop.load()) { l.isRevoked("AB01"); reads.fetch_add(1); }
        });
    for (int i = 0; i < 2000; ++i) l.add({cert("5" + std::to_string(i))});
    l.add({cert("AB01")});
    stop = true;
    for (auto& t : readers) t.join();
    EXPECT_TRUE(l.isRevoked("AB01"));
    EXPECT_EQ(l.size(), 2001u);
    EXPECT_GT(reads.load(), 0);
}
