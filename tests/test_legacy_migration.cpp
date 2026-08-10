#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "utils/LegacyMigration.h"

using nlohmann::json;
namespace fs = std::filesystem;

namespace {

class LegacyMigrationTest : public ::testing::Test {
protected:
    fs::path tmp_;
    fs::path main_cfg_;
    fs::path channels_root_;

    void SetUp() override {
        tmp_ = fs::temp_directory_path() /
               ("legacy_mig_" + std::to_string(::getpid()) + "_" +
                std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::remove_all(tmp_);
        fs::create_directories(tmp_);
        main_cfg_      = tmp_ / "config.json";
        channels_root_ = tmp_ / "channels";
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(tmp_, ec);
    }

    void writeMainCfg(const json& j) {
        std::ofstream f(main_cfg_);
        f << j.dump(2);
    }
    json readMainCfg() const {
        std::ifstream f(main_cfg_);
        return json::parse(f);
    }
    json readJson(const fs::path& p) const {
        std::ifstream f(p);
        return json::parse(f);
    }

    static json makeChannel(int id, const std::string& name) {
        return json{
            {"id", id}, {"name", name},
            {"resolution", "320x240"}, {"fps", 25},
            {"bitrate", 1'000'000}, {"preset", "ultrafast"},
            {"output", {{"port", 9000 + id}, {"latency_ms", 200}}},
        };
    }
};

}  // namespace

TEST_F(LegacyMigrationTest, NoOpWhenChannelsArrayMissing) {
    writeMainCfg(json{{"log_level", "info"}});
    EXPECT_EQ(LegacyMigration::run(main_cfg_, channels_root_), 0u);
    EXPECT_FALSE(fs::is_directory(channels_root_));
}

TEST_F(LegacyMigrationTest, MigratesTwoChannelsAndStripsArray) {
    writeMainCfg(json{
        {"log_level", "info"},
        {"channels", {
            makeChannel(7, "Sport"),
            makeChannel(12, "News"),
        }},
    });

    EXPECT_EQ(LegacyMigration::run(main_cfg_, channels_root_), 2u);

    const auto sport_cfg = channels_root_ / "ch7-Sport"   / "config.json";
    const auto news_cfg  = channels_root_ / "ch12-News"   / "config.json";
    ASSERT_TRUE(fs::exists(sport_cfg));
    ASSERT_TRUE(fs::exists(news_cfg));

    EXPECT_TRUE(fs::is_directory(channels_root_ / "ch7-Sport"  / "logs"));
    EXPECT_TRUE(fs::is_directory(channels_root_ / "ch7-Sport"  / "cache"));

    EXPECT_EQ(readJson(sport_cfg)["name"], "Sport");
    EXPECT_EQ(readJson(news_cfg)["name"],  "News");

    // Main config rewritten without "channels".
    EXPECT_FALSE(readMainCfg().contains("channels"));
    EXPECT_EQ(readMainCfg().value("log_level", std::string{}), "info");
}

TEST_F(LegacyMigrationTest, IdempotentWhenChannelsRootHasFolders) {
    fs::create_directories(channels_root_ / "ch1-Existing");
    writeMainCfg(json{
        {"channels", {makeChannel(1, "Existing")}},
    });
    EXPECT_EQ(LegacyMigration::run(main_cfg_, channels_root_), 0u);
    // Main config still untouched (we did NOT migrate).
    EXPECT_TRUE(readMainCfg().contains("channels"));
}

TEST_F(LegacyMigrationTest, EntryWithoutIdSkippedAndArrayKept) {
    writeMainCfg(json{
        {"channels", {
            makeChannel(7, "Sport"),
            json{{"name", "Broken"}},   // missing id
        }},
    });
    EXPECT_EQ(LegacyMigration::run(main_cfg_, channels_root_), 1u);
    // Sport migrated, array kept because not all entries succeeded.
    EXPECT_TRUE(fs::exists(channels_root_ / "ch7-Sport" / "config.json"));
    EXPECT_TRUE(readMainCfg().contains("channels"));
}

TEST_F(LegacyMigrationTest, EmptyNameUsesIdOnlyFolder) {
    writeMainCfg(json{
        {"channels", {makeChannel(99, "")}},
    });
    EXPECT_EQ(LegacyMigration::run(main_cfg_, channels_root_), 1u);
    EXPECT_TRUE(fs::is_directory(channels_root_ / "ch99"));
}

TEST_F(LegacyMigrationTest, Utf8NamePreservedInFolderName) {
    writeMainCfg(json{
        {"channels", {makeChannel(5, "Канал-1")}},
    });
    EXPECT_EQ(LegacyMigration::run(main_cfg_, channels_root_), 1u);
    EXPECT_TRUE(fs::is_directory(channels_root_ / "ch5-Канал-1"));
}

TEST_F(LegacyMigrationTest, LegacyLogsMovedIntoChannelLogsDir) {
    fs::create_directories(tmp_ / "logs");
    {
        std::ofstream f(tmp_ / "logs" / "ch7-Sport.log");
        f << "ancient log line\n";
    }
    writeMainCfg(json{{"channels", {makeChannel(7, "Sport")}}});

    // Run migration with the working directory set to tmp_ so the
    // hard-coded "logs" lookup inside migrateOne resolves correctly.
    const auto saved_cwd = fs::current_path();
    fs::current_path(tmp_);
    EXPECT_EQ(LegacyMigration::run(main_cfg_, channels_root_), 1u);
    fs::current_path(saved_cwd);

    const auto moved = channels_root_ / "ch7-Sport" / "logs" / "ch7-Sport.log";
    ASSERT_TRUE(fs::exists(moved));
    std::ifstream f(moved);
    std::stringstream ss; ss << f.rdbuf();
    EXPECT_NE(ss.str().find("ancient log line"), std::string::npos);
    // Original location emptied.
    EXPECT_FALSE(fs::exists(tmp_ / "logs" / "ch7-Sport.log"));
}
