#include "stress/StressRunner.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

#include "api/ChannelManager.h"
#include "core/ChannelInstance.h"
#include "metrics/ChannelMetrics.h"
#include "stress/Scenario.h"

namespace liveqx::stress {

namespace {

using clock_t_ = std::chrono::steady_clock;
using nlohmann::json;

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// Build IScenario instances from cfg.scenarios, dropping unknowns.
std::vector<std::unique_ptr<IScenario>> buildScenarios(
        const std::vector<std::string>& names,
        const nlohmann::json& options) {
    std::vector<std::unique_ptr<IScenario>> out;
    out.reserve(names.size());
    for (const auto& n : names) {
        nlohmann::json opt = nlohmann::json::object();
        if (options.is_object() && options.contains(n)) opt = options[n];
        if (auto sc = makeScenario(n, opt)) out.push_back(std::move(sc));
    }
    return out;
}

}  // namespace

StressRunner::StressRunner(ChannelManager& mgr, ChannelCfgBuilder builder)
    : mgr_(mgr),
      builder_(builder ? std::move(builder)
                       : ChannelCfgBuilder{&StressRunner::defaultCfgBuilder}) {}

StressRunner::~StressRunner() {
    stop();
}

bool StressRunner::start(const StressConfig& cfg, FinishedCallback on_finished) {
    if (auto err = cfg.validate()) {
        spdlog::warn("stress: refusing to start — invalid cfg: {}", *err);
        return false;
    }
    State expected = State::Idle;
    if (!state_.compare_exchange_strong(expected, State::Running,
                                        std::memory_order_acq_rel)) {
        return false;
    }
    {
        std::lock_guard lk(mu_);
        started_at_ms_ = now_ms();
    }
    thread_.emplace([this, cfg, cb = std::move(on_finished)](std::stop_token tok) {
        StressReport rep = this->runOnce(cfg, tok);
        StressReport rep_copy = rep;
        {
            std::lock_guard lk(mu_);
            last_report_ = std::move(rep);
        }
        if (cb) {
            try { cb(rep_copy); }
            catch (const std::exception& e) {
                spdlog::error("stress: finished callback threw: {}", e.what());
            } catch (...) {
                spdlog::error("stress: finished callback threw");
            }
        }
        state_.store(State::Idle, std::memory_order_release);
    });
    return true;
}

void StressRunner::stop() {
    if (thread_) {
        thread_->request_stop();
        thread_->join();   // jthread joins on destruction too, but be explicit.
        thread_.reset();
    }
    state_.store(State::Idle, std::memory_order_release);
}

json StressRunner::statusJson() const {
    std::lock_guard lk(mu_);
    json out;
    out["state"] = (state_.load(std::memory_order_acquire) == State::Running)
                       ? "running" : "idle";
    if (state_.load(std::memory_order_acquire) == State::Running) {
        out["started_at_ms"] = started_at_ms_;
    }
    if (last_report_) {
        out["last_pass"]      = last_report_->pass;
        out["last_verdict"]   = last_report_->verdict;
        out["last_ended_ms"]  = last_report_->ended_at_ms;
    }
    return out;
}

json StressRunner::lastReportJson() const {
    std::lock_guard lk(mu_);
    if (!last_report_) return json();
    return last_report_->toJson();
}

double StressRunner::expectedFpsFor(const std::string& resolution) {
    // "WxHpFPS" or "WxH@FPS" or "720p25" / "1080p25".
    auto digits_after = [&](char delim) -> double {
        auto pos = resolution.find(delim);
        if (pos == std::string::npos) return 0.0;
        try { return std::stod(resolution.substr(pos + 1)); }
        catch (...) { return 0.0; }
    };
    if (auto v = digits_after('p'); v > 0) return v;
    if (auto v = digits_after('@'); v > 0) return v;
    return 25.0;
}

json StressRunner::defaultCfgBuilder(int idx, const StressConfig& cfg) {
    // Synthesize a minimal cfg keyed by idx so ports don't collide. Real
    // deployments will inject a builder via stress_runner_cli (c13) that
    // points at canned media + gateway/output configs.
    const int port_base = 49100;
    return json{
        {"name",        "stress-ch-" + std::to_string(idx)},
        {"_ephemeral",  true},
        {"resolution",  "320x240"},
        {"fps",         static_cast<int>(expectedFpsFor(cfg.channel_resolution))},
        {"bitrate",     1'000'000},
        {"preset",      "ultrafast"},
        {"output", {
            {"port",        port_base + idx},
            {"latency_ms",  200},
        }},
    };
}

StressReport StressRunner::runOnce(const StressConfig& cfg, std::stop_token tok) {
    StressReport rep;
    rep.started_at_ms      = now_ms();
    rep.duration_sec       = cfg.duration_sec;
    rep.channels_requested = cfg.channels;
    rep.rss_kb_start       = readRssKb();
    const double expected_fps = expectedFpsFor(cfg.channel_resolution);

    // 1) Create+play channels. We capture every assigned id so teardown
    //    only touches what we own — pre-existing channels are untouched.
    std::vector<int> our_ids;
    our_ids.reserve(cfg.channels);
    for (int i = 1; i <= cfg.channels; ++i) {
        if (tok.stop_requested()) break;
        json chcfg;
        try { chcfg = builder_(i, cfg); }
        catch (const std::exception& e) {
            spdlog::warn("stress: cfg builder threw on idx {}: {}", i, e.what());
            continue;
        }
        chcfg["_ephemeral"] = true;  // always set — cleaned up on restart if crash
        int id = 0;
        auto cr = mgr_.create(chcfg, &id);
        if (cr != ChannelManager::Result::Ok) {
            spdlog::warn("stress: create failed on idx {} ({})", i,
                         static_cast<int>(cr));
            continue;
        }
        auto pr = mgr_.play(id);
        if (pr != ChannelManager::Result::Ok) {
            spdlog::warn("stress: play failed on id {} idx {} ({})", id, i,
                         static_cast<int>(pr));
            // Roll back — we created it but couldn't start it.
            mgr_.remove(id);
            continue;
        }
        our_ids.push_back(id);
    }
    rep.channels_started = static_cast<int>(our_ids.size());

    // 2) Build scenarios + driver context and tick at 1 Hz for the duration
    //    OR until cancelled. Scenarios with no work just no-op per tick.
    auto scenarios = buildScenarios(cfg.scenarios, cfg.scenario_options);
    ScenarioContext ctx;
    ctx.mgr            = &mgr_;
    ctx.channel_ids    = our_ids;
    ctx.run_started_ms = rep.started_at_ms;
    // Per-run RNG seed: use the start ms so logs are reproducible enough
    // for triage but not deterministic across runs (real stress wants
    // randomness; tests inject scenarios directly with a seeded ctx).
    ctx.rng.seed(static_cast<std::uint64_t>(rep.started_at_ms));
    for (auto& sc : scenarios) sc->onStart(ctx);

    {
        using namespace std::chrono_literals;
        const auto tick_period = 1s;
        const auto deadline    = clock_t_::now()
                               + std::chrono::seconds(cfg.duration_sec);
        auto next_tick = clock_t_::now() + tick_period;
        while (clock_t_::now() < deadline) {
            if (tok.stop_requested()) break;
            // Sleep up to next_tick (or deadline) in 100ms chunks.
            const auto sleep_until = std::min(next_tick, deadline);
            while (clock_t_::now() < sleep_until) {
                if (tok.stop_requested()) break;
                const auto remaining = sleep_until - clock_t_::now();
                std::this_thread::sleep_for(
                    std::min<std::chrono::nanoseconds>(remaining, 100ms));
            }
            if (tok.stop_requested()) break;
            const auto now_ms_v     = now_ms();
            const auto elapsed_ms_v = now_ms_v - rep.started_at_ms;
            for (auto& sc : scenarios) {
                sc->tick(ctx, now_ms_v, elapsed_ms_v, rep.scenario_events);
            }
            next_tick += tick_period;
        }
    }
    for (auto& sc : scenarios) sc->onFinish(ctx, rep.scenario_events);

    // 3) Collect per-channel snapshots while everything is still alive.
    std::unordered_set<int> our_set(our_ids.begin(), our_ids.end());
    std::unordered_map<int, ChannelMetricsSnapshot> snaps;
    snaps.reserve(our_set.size());
    mgr_.forEachChannel([&](const ChannelInstance& ch) {
        const int id = ch.id();
        if (!our_set.count(id)) return;
        if (auto m = ch.metrics()) snaps[id] = m->snapshot();
    });
    int alive_count = 0;
    for (int id : our_ids) {
        ChannelStats s;
        s.id           = id;
        s.expected_fps = expected_fps;
        auto it = snaps.find(id);
        if (it != snaps.end()) {
            s.alive = true;
            ++alive_count;
            const auto& sn = it->second;
            s.frames_rendered = sn.frames_rendered;
            s.frames_dropped  = sn.frames_dropped;
            // Rolling-window FPS can be 0 for the last-started channel if its
            // first window hasn't been sampled yet. Fall back to total frames /
            // duration so a healthy channel doesn't look like 100% drop.
            s.actual_fps = (sn.actual_fps > 0.0)
                ? sn.actual_fps
                : (rep.duration_sec > 0 && sn.frames_rendered > 0
                    ? static_cast<double>(sn.frames_rendered) / rep.duration_sec
                    : 0.0);
            if (expected_fps > 0.0) {
                const double drop = (expected_fps - s.actual_fps) / expected_fps * 100.0;
                s.fps_drop_pct = std::max(0.0, drop);
            }
        } else {
            s.alive = false;
        }
        rep.worst_fps_drop_pct = std::max(rep.worst_fps_drop_pct, s.fps_drop_pct);
        rep.per_channel.push_back(s);
    }
    rep.channels_alive_at_end = alive_count;
    rep.crashes               = rep.channels_started - alive_count;

    // 4) Tear down — remove all channels in parallel so 50 × stop() costs
    //    the same as 1 × stop() instead of 50 × 2s sequential.
    {
        std::vector<std::thread> workers;
        workers.reserve(our_ids.size());
        for (int id : our_ids) {
            workers.emplace_back([&, id] {
                auto rr = mgr_.remove(id);
                if (rr != ChannelManager::Result::Ok)
                    spdlog::warn("stress: remove({}) returned {} during teardown",
                                 id, static_cast<int>(rr));
            });
        }
        for (auto& w : workers) w.join();
    }

    // 5) Memory growth, normalized to per-hour.
    rep.rss_kb_end = readRssKb();
    if (rep.rss_kb_start > 0 && cfg.duration_sec > 0) {
        const double growth_pct =
            (static_cast<double>(rep.rss_kb_end - rep.rss_kb_start)
             / static_cast<double>(rep.rss_kb_start)) * 100.0;
        const double hours = static_cast<double>(cfg.duration_sec) / 3600.0;
        rep.memory_growth_pct_per_hour = (hours > 0) ? (growth_pct / hours) : growth_pct;
    }

    rep.ended_at_ms = now_ms();
    evaluatePass(rep, cfg);
    return rep;
}

void StressRunner::evaluatePass(StressReport& rep, const StressConfig& cfg) const {
    rep.fail_reasons.clear();
    if (rep.crashes > cfg.pass.max_crashes) {
        rep.fail_reasons.push_back(
            "crashes=" + std::to_string(rep.crashes) +
            " > max " + std::to_string(cfg.pass.max_crashes));
    }
    if (rep.worst_fps_drop_pct > cfg.pass.max_fps_drop_pct) {
        char buf[96];
        std::snprintf(buf, sizeof(buf),
                      "worst_fps_drop=%.2f%% > max %.2f%%",
                      rep.worst_fps_drop_pct, cfg.pass.max_fps_drop_pct);
        rep.fail_reasons.emplace_back(buf);
    }
    // Memory-growth check is only meaningful over hour-scale runs. Anything
    // shorter than 1h amplifies allocator/page-cache noise by at least 60×
    // and produces false positives. Real stress runs are 24h (86400s).
    constexpr int kMinDurationForMemoryCheckSec = 3600;
    if (cfg.duration_sec >= kMinDurationForMemoryCheckSec &&
        rep.memory_growth_pct_per_hour > cfg.pass.max_memory_growth_pct_per_hour) {
        char buf[120];
        std::snprintf(buf, sizeof(buf),
                      "memory_growth=%.4f%%/h > max %.4f%%/h",
                      rep.memory_growth_pct_per_hour,
                      cfg.pass.max_memory_growth_pct_per_hour);
        rep.fail_reasons.emplace_back(buf);
    }
    rep.pass = rep.fail_reasons.empty();
    if (rep.pass) {
        rep.verdict = "pass";
    } else {
        std::string v = "fail:";
        for (const auto& r : rep.fail_reasons) { v += " "; v += r; v += ";"; }
        rep.verdict = std::move(v);
    }
}

std::int64_t StressRunner::readRssKb() {
    // Simple /proc/self/status parser. Returns 0 on any I/O or parse error
    // — callers treat 0 as "unknown" and skip the memory-growth check.
    std::ifstream f("/proc/self/status");
    if (!f) return 0;
    std::string line;
    while (std::getline(f, line)) {
        if (line.compare(0, 6, "VmRSS:") != 0) continue;
        const char* p = line.c_str() + 6;
        while (*p == ' ' || *p == '\t') ++p;
        char* end = nullptr;
        const auto v = std::strtoll(p, &end, 10);
        if (end == p) return 0;
        return static_cast<std::int64_t>(v);
    }
    return 0;
}

}  // namespace liveqx::stress
