// fix33 — TimeConfigRepo unit tests.
//
// Покрытие:
//   1. Empty load — нет row → load() == nullopt.
//   2. Save+Load roundtrip для всех 3 source.
//   3. updateNtpSyncResult — обновляет только два поля.
//   4. IANA-валидатор: known zones → true; пустая/мусор → false.
//   5. validate(): корректные / некорректные конфиги.
//   6. timeSourceToString / FromString round-trip.

#include <filesystem>

#include <gtest/gtest.h>

#include "auth/AuthDb.h"
#include "auth/TimeConfig.h"
#include "auth/TimeConfigRepo.h"

namespace sa = liveqx::auth;

namespace {

struct Fx {
    std::filesystem::path dbpath;
    sa::AuthDb            db;
    sa::TimeConfigRepo    repo;

    Fx() : dbpath(std::filesystem::temp_directory_path() /
                  ("time_repo_test_" + std::to_string(::getpid()) + "_" +
                   std::to_string(reinterpret_cast<std::uintptr_t>(this)) + ".db")),
           db(dbpath),
           repo(db) {
        std::filesystem::remove(dbpath);
        EXPECT_TRUE(db.open());
    }
    ~Fx() {
        std::error_code ec;
        std::filesystem::remove(dbpath, ec);
        std::filesystem::remove(dbpath.string() + "-wal", ec);
        std::filesystem::remove(dbpath.string() + "-shm", ec);
    }
};

}  // namespace

TEST(TimeConfigRepo, EmptyLoadReturnsNullopt) {
    Fx f;
    EXPECT_FALSE(f.repo.load().has_value());
}

TEST(TimeConfigRepo, SaveAndLoadSystemLocalDefault) {
    Fx f;
    sa::TimeConfig cfg;  // defaults: SystemLocal / UTC.
    ASSERT_TRUE(f.repo.save(cfg));

    auto loaded = f.repo.load();
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->source,          sa::TimeSource::SystemLocal);
    EXPECT_EQ(loaded->server_timezone, "UTC");
    EXPECT_FALSE(loaded->ntp.enabled);
    EXPECT_TRUE(loaded->ntp.servers.empty());
    EXPECT_EQ(loaded->ntp.poll_interval_s, 60);
    EXPECT_FALSE(loaded->ntp.last_offset_ms.has_value());
    EXPECT_FALSE(loaded->ntp.last_sync_at.has_value());
    EXPECT_EQ(loaded->manual.offset_ms, 0);
    EXPECT_FALSE(loaded->manual.set_at.has_value());
}

TEST(TimeConfigRepo, SaveAndLoadNtpRoundTrip) {
    Fx f;
    sa::TimeConfig cfg;
    cfg.source          = sa::TimeSource::Ntp;
    cfg.server_timezone = "Europe/Moscow";
    cfg.ntp.enabled     = true;
    cfg.ntp.servers     = {"pool.ntp.org", "time.cloudflare.com:123"};
    cfg.ntp.poll_interval_s = 300;
    ASSERT_TRUE(f.repo.save(cfg));

    auto loaded = f.repo.load();
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->source,              sa::TimeSource::Ntp);
    EXPECT_EQ(loaded->server_timezone,     "Europe/Moscow");
    EXPECT_TRUE(loaded->ntp.enabled);
    ASSERT_EQ(loaded->ntp.servers.size(),  2u);
    EXPECT_EQ(loaded->ntp.servers[0],      "pool.ntp.org");
    EXPECT_EQ(loaded->ntp.servers[1],      "time.cloudflare.com:123");
    EXPECT_EQ(loaded->ntp.poll_interval_s, 300);
}

TEST(TimeConfigRepo, SaveAndLoadManualRoundTrip) {
    Fx f;
    sa::TimeConfig cfg;
    cfg.source             = sa::TimeSource::Manual;
    cfg.server_timezone    = "America/New_York";
    cfg.manual.offset_ms   = -7200'000;  // -2h
    cfg.manual.set_at      = 1'700'000'000;
    ASSERT_TRUE(f.repo.save(cfg));

    auto loaded = f.repo.load();
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->source,              sa::TimeSource::Manual);
    EXPECT_EQ(loaded->server_timezone,     "America/New_York");
    EXPECT_EQ(loaded->manual.offset_ms,    -7200'000);
    ASSERT_TRUE(loaded->manual.set_at.has_value());
    EXPECT_EQ(*loaded->manual.set_at,      1'700'000'000);
}

TEST(TimeConfigRepo, UpdateNtpSyncResultPreservesOtherFields) {
    Fx f;
    sa::TimeConfig cfg;
    cfg.source          = sa::TimeSource::Ntp;
    cfg.server_timezone = "Europe/Moscow";
    cfg.ntp.enabled     = true;
    cfg.ntp.servers     = {"pool.ntp.org"};
    cfg.ntp.poll_interval_s = 120;
    ASSERT_TRUE(f.repo.save(cfg));

    ASSERT_TRUE(f.repo.updateNtpSyncResult(/*offset_ms=*/-42,
                                           /*sync_at=*/1'700'000'500));

    auto loaded = f.repo.load();
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->source,              sa::TimeSource::Ntp);
    EXPECT_EQ(loaded->server_timezone,     "Europe/Moscow");
    ASSERT_EQ(loaded->ntp.servers.size(),  1u);
    EXPECT_EQ(loaded->ntp.poll_interval_s, 120);
    ASSERT_TRUE(loaded->ntp.last_offset_ms.has_value());
    EXPECT_EQ(*loaded->ntp.last_offset_ms, -42);
    ASSERT_TRUE(loaded->ntp.last_sync_at.has_value());
    EXPECT_EQ(*loaded->ntp.last_sync_at,   1'700'000'500);
}

TEST(TimeConfig, IanaValidatorAcceptsKnownZones) {
    EXPECT_TRUE(sa::isValidIanaTimezone("UTC"));
    EXPECT_TRUE(sa::isValidIanaTimezone("Europe/Moscow"));
    EXPECT_TRUE(sa::isValidIanaTimezone("America/New_York"));
    EXPECT_TRUE(sa::isValidIanaTimezone("Asia/Tokyo"));
}

TEST(TimeConfig, IanaValidatorRejectsGarbage) {
    EXPECT_FALSE(sa::isValidIanaTimezone(""));
    EXPECT_FALSE(sa::isValidIanaTimezone("Garbage/Zone"));
    EXPECT_FALSE(sa::isValidIanaTimezone("Europe/Atlantis"));
    EXPECT_FALSE(sa::isValidIanaTimezone("UTC+3"));  // offset-form не IANA.
}

TEST(TimeConfig, TimeSourceStringRoundTrip) {
    EXPECT_EQ(sa::timeSourceToString(sa::TimeSource::SystemLocal), "system_local");
    EXPECT_EQ(sa::timeSourceToString(sa::TimeSource::Ntp),         "ntp");
    EXPECT_EQ(sa::timeSourceToString(sa::TimeSource::Manual),      "manual");

    EXPECT_EQ(sa::timeSourceFromString("system_local"),
              std::optional{sa::TimeSource::SystemLocal});
    EXPECT_EQ(sa::timeSourceFromString("ntp"),
              std::optional{sa::TimeSource::Ntp});
    EXPECT_EQ(sa::timeSourceFromString("manual"),
              std::optional{sa::TimeSource::Manual});
    EXPECT_FALSE(sa::timeSourceFromString("garbage").has_value());
    EXPECT_FALSE(sa::timeSourceFromString("").has_value());
}

TEST(TimeConfigRepo, ValidateAcceptsDefault) {
    sa::TimeConfig cfg;
    EXPECT_EQ(sa::TimeConfigRepo::validate(cfg), "");
}

TEST(TimeConfigRepo, ValidateRejectsBadTimezone) {
    sa::TimeConfig cfg;
    cfg.server_timezone = "Mars/Olympus";
    EXPECT_NE(sa::TimeConfigRepo::validate(cfg), "");
}

TEST(TimeConfigRepo, ValidateRejectsNtpWithoutServers) {
    sa::TimeConfig cfg;
    cfg.source = sa::TimeSource::Ntp;
    cfg.ntp.servers.clear();
    EXPECT_NE(sa::TimeConfigRepo::validate(cfg), "");
}

TEST(TimeConfigRepo, ValidateRejectsNtpOutOfRangePollInterval) {
    sa::TimeConfig cfg;
    cfg.source = sa::TimeSource::Ntp;
    cfg.ntp.servers = {"pool.ntp.org"};
    cfg.ntp.poll_interval_s = 1;        // <5
    EXPECT_NE(sa::TimeConfigRepo::validate(cfg), "");
    cfg.ntp.poll_interval_s = 99999;    // >3600
    EXPECT_NE(sa::TimeConfigRepo::validate(cfg), "");
}

TEST(TimeConfigRepo, ValidateRejectsAbsurdManualOffset) {
    sa::TimeConfig cfg;
    cfg.source = sa::TimeSource::Manual;
    cfg.manual.offset_ms = std::numeric_limits<std::int64_t>::max();
    EXPECT_NE(sa::TimeConfigRepo::validate(cfg), "");
}
