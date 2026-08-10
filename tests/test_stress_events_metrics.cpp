// fix26 c12 — StressService EventBus integration + Prometheus rendering.
//
// We want to verify:
//   1. Starting a run (channels=0, duration_sec=1) publishes a
//      StressRunStarted event.
//   2. Completing the run publishes a StressRunFinished event with
//      pass=true, a non-empty verdict, and report_id from the on-disk store.
//   3. metricsSnapshot() reflects the run after completion.
//   4. MetricsCollector.renderPrometheus() emits all stress_* families
//      when constructed with a StressService pointer, and omits them
//      when given nullptr.
//
// Tests stay <2s by using channels=0 (no real channel work) and
// duration_sec=1 (the runner sleeps in 100ms chunks so it returns quickly).
//
// We construct a real ChannelManager, EventBus, ProcessMetrics and
// MetricsCollector — no REST layer needed.

#include <chrono>
#include <filesystem>
#include <thread>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "api/ChannelManager.h"
#include "api/MetricsCollector.h"
#include "events/EventBus.h"
#include "metrics/ProcessMetrics.h"
#include "stress/StressConfig.h"
#include "stress/StressService.h"

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using nlohmann::json;
using liveqx::stress::StressConfig;
using liveqx::stress::StressService;
using liveqx::events::EventBus;
using liveqx::events::EventType;

namespace {

fs::path uniqueDir(const std::string& tag) {
    auto p = fs::temp_directory_path() /
             ("stress_em_" + tag + "_" + std::to_string(::getpid()));
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p);
    return p;
}

StressConfig makeCfg(const fs::path& dir) {
    StressConfig c;
    c.enabled             = true;
    c.schedule_cron       = "";
    c.duration_sec        = 1;
    c.channels            = 0;
    c.outputs_per_channel = 0;
    c.report_dir          = dir.string();
    return c;
}

}  // namespace

TEST(StressEvents, StartAndFinishPublishToBus) {
    auto dir = uniqueDir("publish");
    ChannelManager  mgr(nullptr);
    EventBus        bus;
    auto cfg = makeCfg(dir);
    StressService   svc(mgr, cfg, {}, &bus);

    auto sub = bus.subscribe();

    std::string err;
    ASSERT_TRUE(svc.startNow(&err)) << err;

    // Wait up to 5s for both events. duration_sec=1 ⇒ should land well under.
    bool got_started = false, got_finished = false;
    json finished_payload;
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline &&
           !(got_started && got_finished)) {
        for (const auto& e : sub->drain(200ms)) {
            if (e.type == EventType::StressRunStarted)  got_started  = true;
            if (e.type == EventType::StressRunFinished) {
                got_finished      = true;
                finished_payload  = e.payload;
            }
        }
    }
    EXPECT_TRUE(got_started);
    ASSERT_TRUE(got_finished);

    EXPECT_TRUE(finished_payload.contains("pass"));
    EXPECT_TRUE(finished_payload["pass"].get<bool>());
    EXPECT_TRUE(finished_payload.contains("verdict"));
    EXPECT_FALSE(finished_payload["verdict"].get<std::string>().empty());
    EXPECT_TRUE(finished_payload.contains("report_id"));
    EXPECT_FALSE(finished_payload["report_id"].get<std::string>().empty());
    EXPECT_GT(finished_payload.value("ended_at_ms", std::int64_t{0}), 0);

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST(StressEvents, MetricsSnapshotReflectsCompletedRun) {
    auto dir = uniqueDir("snapshot");
    ChannelManager  mgr(nullptr);
    EventBus        bus;
    auto cfg = makeCfg(dir);
    StressService   svc(mgr, cfg, {}, &bus);

    auto before = svc.metricsSnapshot();
    EXPECT_EQ(before.runs_total,     0);
    EXPECT_EQ(before.passes_total,   0);
    EXPECT_EQ(before.failures_total, 0);
    EXPECT_FALSE(before.running);

    auto sub = bus.subscribe();
    ASSERT_TRUE(svc.startNow(nullptr));

    // Drain until we see StressRunFinished (signals counters are updated).
    bool finished = false;
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!finished && std::chrono::steady_clock::now() < deadline) {
        for (const auto& e : sub->drain(200ms)) {
            if (e.type == EventType::StressRunFinished) finished = true;
        }
    }
    ASSERT_TRUE(finished);

    auto after = svc.metricsSnapshot();
    EXPECT_FALSE(after.running);
    EXPECT_EQ(after.runs_total,    1);
    EXPECT_EQ(after.passes_total,  1);
    EXPECT_EQ(after.failures_total, 0);
    EXPECT_TRUE(after.last_pass);
    EXPECT_GT(after.last_started_ms, 0);
    EXPECT_GT(after.last_ended_ms,   0);
    EXPECT_GE(after.last_ended_ms,   after.last_started_ms);
    EXPECT_EQ(after.reports_count,   1u);
    EXPECT_FALSE(after.scheduler_armed);   // cron empty

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST(StressMetricsRender, AllFamiliesPresentWhenServiceProvided) {
    auto dir = uniqueDir("render");
    ChannelManager  mgr(nullptr);
    EventBus        bus;
    auto cfg = makeCfg(dir);
    StressService   svc(mgr, cfg, {}, &bus);

    ProcessMetrics pm(/*start=*/1234567890);
    BuildInfo b{"1.0.0", "abc", "2026-05-06T10:00:00Z", "Release"};
    MetricsCollector mc(mgr, pm, b, /*gateways=*/nullptr, /*stress=*/&svc);

    const auto text = mc.renderPrometheus();
    EXPECT_NE(text.find("liveqx_stress_running"),         std::string::npos);
    EXPECT_NE(text.find("liveqx_stress_runs_total"),      std::string::npos);
    EXPECT_NE(text.find("liveqx_stress_passes_total"),    std::string::npos);
    EXPECT_NE(text.find("liveqx_stress_failures_total"),  std::string::npos);
    EXPECT_NE(text.find("liveqx_stress_last_started_ms"), std::string::npos);
    EXPECT_NE(text.find("liveqx_stress_last_ended_ms"),   std::string::npos);
    EXPECT_NE(text.find("liveqx_stress_last_pass"),       std::string::npos);
    EXPECT_NE(text.find("liveqx_stress_reports_count"),   std::string::npos);
    EXPECT_NE(text.find("liveqx_stress_scheduler_armed"), std::string::npos);

    // Counter values are zero before any run.
    EXPECT_NE(text.find("liveqx_stress_runs_total 0"),    std::string::npos);
    EXPECT_NE(text.find("liveqx_stress_running 0"),       std::string::npos);

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST(StressMetricsRender, FamiliesOmittedWhenServiceNull) {
    ChannelManager mgr(nullptr);
    ProcessMetrics pm(/*start=*/1234567890);
    BuildInfo b{"1.0.0", "abc", "2026-05-06T10:00:00Z", "Release"};
    MetricsCollector mc(mgr, pm, b, /*gateways=*/nullptr, /*stress=*/nullptr);

    const auto text = mc.renderPrometheus();
    EXPECT_EQ(text.find("liveqx_stress_"), std::string::npos);
}

TEST(StressMetricsRender, CountersReflectFinishedRun) {
    auto dir = uniqueDir("rendered_run");
    ChannelManager  mgr(nullptr);
    EventBus        bus;
    auto cfg = makeCfg(dir);
    StressService   svc(mgr, cfg, {}, &bus);

    auto sub = bus.subscribe();
    ASSERT_TRUE(svc.startNow(nullptr));
    bool finished = false;
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!finished && std::chrono::steady_clock::now() < deadline) {
        for (const auto& e : sub->drain(200ms)) {
            if (e.type == EventType::StressRunFinished) finished = true;
        }
    }
    ASSERT_TRUE(finished);

    ProcessMetrics pm(/*start=*/1234567890);
    BuildInfo b{"1.0.0", "abc", "2026-05-06T10:00:00Z", "Release"};
    MetricsCollector mc(mgr, pm, b, /*gateways=*/nullptr, /*stress=*/&svc);

    const auto text = mc.renderPrometheus();
    EXPECT_NE(text.find("liveqx_stress_runs_total 1"),   std::string::npos);
    EXPECT_NE(text.find("liveqx_stress_passes_total 1"), std::string::npos);
    EXPECT_NE(text.find("liveqx_stress_failures_total 0"), std::string::npos);
    EXPECT_NE(text.find("liveqx_stress_last_pass 1"),    std::string::npos);
    EXPECT_NE(text.find("liveqx_stress_reports_count 1"), std::string::npos);

    std::error_code ec;
    fs::remove_all(dir, ec);
}
