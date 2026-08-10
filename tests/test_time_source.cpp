// fix33 A2 — unit tests для ITimeSource / TimeSourceManager.

#include <chrono>
#include <thread>

#include <gtest/gtest.h>

#include "auth/TimeConfig.h"
#include "auth/TimeSource.h"

namespace sa = liveqx::auth;

namespace {

std::int64_t systemNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}  // namespace

TEST(TimeSource, SystemLocalMatchesSystemClock) {
    sa::SystemLocalSource src;
    EXPECT_EQ(src.offsetMs(), 0);
    EXPECT_EQ(src.sourceName(), "system_local");

    const auto before = systemNowMs();
    const auto got    = src.effectiveUnixMs();
    const auto after  = systemNowMs();

    // SystemLocal — passthrough; effective должен лежать в окне [before, after].
    EXPECT_GE(got, before);
    EXPECT_LE(got, after);
}

TEST(TimeSource, ManualAppliesPositiveOffset) {
    constexpr std::int64_t kOffset = 12'345;
    sa::ManualOffsetSource src(kOffset);

    EXPECT_EQ(src.offsetMs(), kOffset);
    EXPECT_EQ(src.sourceName(), "manual");

    const auto sys_now = systemNowMs();
    const auto got     = src.effectiveUnixMs();
    // Допустимый дрифт ±50 мс на разницу между двумя замерами.
    EXPECT_NEAR(got - sys_now, kOffset, 50);
}

TEST(TimeSource, ManualAppliesNegativeOffset) {
    constexpr std::int64_t kOffset = -7'200'000;  // -2h
    sa::ManualOffsetSource src(kOffset);

    const auto sys_now = systemNowMs();
    const auto got     = src.effectiveUnixMs();
    EXPECT_NEAR(got - sys_now, kOffset, 50);
}

TEST(TimeSource, ManualSetOffsetMsUpdatesNow) {
    sa::ManualOffsetSource src(0);
    EXPECT_EQ(src.offsetMs(), 0);

    src.setOffsetMs(500);
    EXPECT_EQ(src.offsetMs(), 500);

    const auto sys_now = systemNowMs();
    const auto got     = src.effectiveUnixMs();
    EXPECT_NEAR(got - sys_now, 500, 50);
}

TEST(TimeSourceManager, DefaultIsSystemLocal) {
    sa::TimeSourceManager mgr;
    EXPECT_EQ(mgr.sourceName(), "system_local");
    EXPECT_EQ(mgr.offsetMs(),   0);

    const auto sys_now = systemNowMs();
    const auto got     = std::chrono::duration_cast<std::chrono::milliseconds>(
                             mgr.now().time_since_epoch())
                             .count();
    EXPECT_NEAR(got, sys_now, 50);
}

TEST(TimeSourceManager, ReconfigureToManualAppliesOffset) {
    sa::TimeSourceManager mgr;

    sa::TimeConfig cfg;
    cfg.source           = sa::TimeSource::Manual;
    cfg.manual.offset_ms = 60'000;  // +1m
    mgr.reconfigure(cfg);

    EXPECT_EQ(mgr.sourceName(), "manual");
    EXPECT_EQ(mgr.offsetMs(),   60'000);

    const auto sys_now = systemNowMs();
    const auto got     = std::chrono::duration_cast<std::chrono::milliseconds>(
                             mgr.now().time_since_epoch())
                             .count();
    EXPECT_NEAR(got - sys_now, 60'000, 50);
}

TEST(TimeSourceManager, ReconfigureToNtpUsesLastOffsetAsStub) {
    sa::TimeSourceManager mgr;

    sa::TimeConfig cfg;
    cfg.source             = sa::TimeSource::Ntp;
    cfg.ntp.enabled        = true;
    cfg.ntp.servers        = {"pool.ntp.org"};
    cfg.ntp.last_offset_ms = 1'234;
    mgr.reconfigure(cfg);

    // A2 stub: NtpSource ещё не реализован → ManualOffsetSource(last_offset).
    // sourceName на этом этапе — "manual" (это видно консьюмерам).
    EXPECT_EQ(mgr.sourceName(), "manual");
    EXPECT_EQ(mgr.offsetMs(),   1'234);
}

TEST(TimeSourceManager, ReconfigureBackToSystemLocalResetsOffset) {
    sa::TimeSourceManager mgr;

    sa::TimeConfig cfg;
    cfg.source           = sa::TimeSource::Manual;
    cfg.manual.offset_ms = 9'999;
    mgr.reconfigure(cfg);
    EXPECT_EQ(mgr.offsetMs(), 9'999);

    cfg.source           = sa::TimeSource::SystemLocal;
    cfg.manual.offset_ms = 0;
    mgr.reconfigure(cfg);
    EXPECT_EQ(mgr.sourceName(), "system_local");
    EXPECT_EQ(mgr.offsetMs(),   0);
}
