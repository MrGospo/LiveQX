#include <gtest/gtest.h>
#include <spdlog/spdlog.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

#include "utils/Log.h"
#include "utils/PathUtils.h"

namespace fs = std::filesystem;

namespace {

std::string slurp(const fs::path& p) {
    std::ifstream f(p);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

class LogRoutingTest : public ::testing::Test {
protected:
    fs::path tmp_;
    void SetUp() override {
        tmp_ = fs::temp_directory_path() /
               ("log_routing_test_" + std::to_string(::getpid()) +
                "_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::create_directories(tmp_);
        Log::init(tmp_.string(), "trace");
    }
    void TearDown() override {
        spdlog::shutdown();
        std::error_code ec;
        fs::remove_all(tmp_, ec);
    }
};

}  // namespace

TEST(PathUtils, SanitizeReplacesUnsafe) {
    EXPECT_EQ(PathUtils::sanitizeForPath("hello/world"),  "hello_world");
    EXPECT_EQ(PathUtils::sanitizeForPath("a:b*c?d|e"),    "a_b_c_d_e");
    EXPECT_EQ(PathUtils::sanitizeForPath("  spaced  "),   "spaced");
    EXPECT_EQ(PathUtils::sanitizeForPath("..hidden"),     "hidden");
    EXPECT_EQ(PathUtils::sanitizeForPath(""),             "");
}

TEST(PathUtils, SanitizePreservesUtf8Cyrillic) {
    const std::string in  = "Канал-1";
    const std::string out = PathUtils::sanitizeForPath(in);
    EXPECT_EQ(out, in);
}

TEST_F(LogRoutingTest, CreateChannelLoggerProducesNamedFile) {
    auto lg = Log::createChannelLogger(7, "MyChannel");
    ASSERT_NE(lg, nullptr);
    lg->info("hello-from-channel");
    lg->flush();

    const auto expected = tmp_ / "ch7-MyChannel.log";
    ASSERT_TRUE(fs::exists(expected)) << "missing " << expected;
    EXPECT_NE(slurp(expected).find("hello-from-channel"), std::string::npos);
}

TEST_F(LogRoutingTest, ChannelLoggerWritesIndependentlyFromMain) {
    auto lg = Log::createChannelLogger(3, "Alpha");
    spdlog::default_logger()->info("global-line");
    lg->info("channel-line");
    spdlog::default_logger()->flush();
    lg->flush();

    const auto main_log = tmp_ / "liveqx.log";
    const auto ch_log   = tmp_ / "ch3-Alpha.log";
    ASSERT_TRUE(fs::exists(main_log));
    ASSERT_TRUE(fs::exists(ch_log));

    const auto main_body = slurp(main_log);
    const auto ch_body   = slurp(ch_log);

    EXPECT_NE(main_body.find("global-line"), std::string::npos);
    EXPECT_EQ(main_body.find("channel-line"), std::string::npos);
    EXPECT_NE(ch_body.find("channel-line"), std::string::npos);
    EXPECT_EQ(ch_body.find("global-line"), std::string::npos);
}

TEST_F(LogRoutingTest, EmptyNameFallsBackToIdOnly) {
    auto lg = Log::createChannelLogger(42, "");
    lg->info("noname");
    lg->flush();
    EXPECT_TRUE(fs::exists(tmp_ / "ch42.log"));
}

TEST_F(LogRoutingTest, SetLevelGloballyAffectsExistingLoggers) {
    auto lg = Log::createChannelLogger(1, "A");
    Log::setLevelGlobally("warn");
    EXPECT_FALSE(lg->should_log(spdlog::level::info));
    EXPECT_TRUE (lg->should_log(spdlog::level::warn));
    EXPECT_FALSE(spdlog::default_logger()->should_log(spdlog::level::info));

    Log::setLevelGlobally("debug");
    EXPECT_TRUE(lg->should_log(spdlog::level::debug));
}

TEST_F(LogRoutingTest, SetThreadNameDoesNotCrash) {
    std::thread t([] {
        Log::setThreadName("test-thread");
        // A long name (>15) must be silently truncated, not crash.
        Log::setThreadName("0123456789abcdefXYZ");
    });
    t.join();
    SUCCEED();
}
