#include "stress/StressService.h"

#include <chrono>
#include <utility>

#include <spdlog/spdlog.h>

#include "events/EventBus.h"

namespace liveqx::stress {

namespace {

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

}  // namespace

StressService::StressService(ChannelManager& mgr,
                             StressConfig initial_cfg,
                             StressRunner::ChannelCfgBuilder builder,
                             liveqx::events::EventBus* events)
    : mgr_(mgr),
      builder_(std::move(builder)),
      events_(events),
      cfg_(std::move(initial_cfg)) {
    runner_ = std::make_unique<StressRunner>(mgr_, builder_);
    if (!cfg_.report_dir.empty()) {
        store_ = std::make_unique<StressReportStore>(cfg_.report_dir);
    }
    rearmSchedulerLocked();
}

StressService::~StressService() {
    // Tear down the scheduler first so a fired callback can't race the
    // runner's destruction.
    scheduler_.reset();
    runner_.reset();
}

StressConfig StressService::config() const {
    std::scoped_lock lk(mu_);
    return cfg_;
}

nlohmann::json StressService::configJson() const {
    std::scoped_lock lk(mu_);
    return cfg_.toJson();
}

void StressService::setPersistCallback(PersistCallback cb) {
    std::scoped_lock lk(mu_);
    persist_ = std::move(cb);
}

bool StressService::setConfigJson(const nlohmann::json& body, std::string* err) {
    auto next = StressConfig::fromJson(body);
    if (auto v = next.validate()) {
        if (err) *err = *v;
        return false;
    }
    PersistCallback persist_snapshot;
    bool cron_changed = false;
    bool report_dir_changed = false;
    {
        std::scoped_lock lk(mu_);
        cron_changed       = (next.schedule_cron != cfg_.schedule_cron);
        report_dir_changed = (next.report_dir    != cfg_.report_dir);
        cfg_ = std::move(next);
        persist_snapshot = persist_;
        if (cron_changed) rearmSchedulerLocked();
        if (report_dir_changed) {
            if (cfg_.report_dir.empty()) {
                store_.reset();
            } else {
                store_ = std::make_unique<StressReportStore>(cfg_.report_dir);
            }
        }
    }
    if (persist_snapshot) {
        try { persist_snapshot(this->config()); }
        catch (const std::exception& e) {
            spdlog::warn("stress: persist callback threw: {}", e.what());
        } catch (...) {
            spdlog::warn("stress: persist callback threw");
        }
    }
    return true;
}

void StressService::rearmSchedulerLocked() {
    // mu_ already held by caller.
    if (cfg_.schedule_cron.empty()) {
        scheduler_.reset();
        return;
    }
    if (scheduler_) {
        std::string err;
        if (!scheduler_->setCron(cfg_.schedule_cron, &err)) {
            spdlog::warn("stress: rearm — invalid cron '{}' ({}); leaving"
                         " previous schedule in place",
                         cfg_.schedule_cron, err);
        }
        return;
    }
    auto cb = [this] {
        // Capture by value so the cfg snapshot is stable for this run.
        StressConfig snapshot;
        {
            std::scoped_lock lk(mu_);
            snapshot = cfg_;
        }
        if (!snapshot.enabled) {
            spdlog::info("stress[scheduler]: cron fired but enabled=false — skipping");
            return;
        }
        if (runner_ && !runner_->running()) {
            onRunStarted(snapshot);
            runner_->start(snapshot,
                [this](const StressReport& rep) { onRunFinished(rep); });
        } else {
            spdlog::info("stress[scheduler]: cron fired but a run is already in progress");
        }
    };
    scheduler_ = std::make_unique<StressScheduler>(cfg_.schedule_cron, cb);
}

bool StressService::startNow(std::string* err) {
    StressConfig snapshot;
    {
        std::scoped_lock lk(mu_);
        snapshot = cfg_;
    }
    if (!snapshot.enabled) {
        if (err) *err = "stress is disabled (enabled=false)";
        return false;
    }
    if (auto v = snapshot.validate()) {
        if (err) *err = *v;
        return false;
    }
    if (runner_ && runner_->running()) {
        if (err) *err = "stress run already in progress";
        return false;
    }
    if (!runner_) {
        if (err) *err = "runner not initialized";
        return false;
    }
    onRunStarted(snapshot);
    const bool ok = runner_->start(snapshot,
        [this](const StressReport& rep) { onRunFinished(rep); });
    if (!ok && err) *err = "runner refused to start";
    return ok;
}

void StressService::onRunStarted(const StressConfig& cfg) {
    const auto ts = now_ms();
    {
        std::scoped_lock lk(mu_);
        last_started_ms_ = ts;
    }
    if (events_) {
        events_->publish(events::EventType::StressRunStarted, /*channel_id=*/-1, {
            {"started_at_ms", ts},
            {"duration_sec", cfg.duration_sec},
            {"channels",     cfg.channels},
            {"scenarios",    cfg.scenarios},
        });
    }
}

void StressService::stopNow() {
    if (runner_) runner_->stop();
}

void StressService::onRunFinished(const StressReport& rep) {
    std::string id;
    {
        std::scoped_lock lk(mu_);
        if (store_) id = store_->write(rep);
        last_report_id_ = id;
        ++runs_total_;
        if (rep.pass) ++passes_total_;
        else          ++failures_total_;
        last_ended_ms_ = rep.ended_at_ms;
        last_pass_     = rep.pass;
    }
    if (events_) {
        events_->publish(events::EventType::StressRunFinished, /*channel_id=*/-1, {
            {"pass",         rep.pass},
            {"verdict",      rep.verdict},
            {"report_id",    id},
            {"ended_at_ms",  rep.ended_at_ms},
        });
    }
}

StressService::MetricsSnapshot StressService::metricsSnapshot() const {
    MetricsSnapshot s;
    s.running         = runner_ && runner_->running();
    std::size_t reports = 0;
    {
        std::scoped_lock lk(mu_);
        s.runs_total       = runs_total_;
        s.passes_total     = passes_total_;
        s.failures_total   = failures_total_;
        s.last_started_ms  = last_started_ms_;
        s.last_ended_ms    = last_ended_ms_;
        s.last_pass        = last_pass_;
        s.scheduler_armed  = (scheduler_ != nullptr);
        if (store_) reports = store_->list().size();
    }
    s.reports_count = reports;
    return s;
}

nlohmann::json StressService::statusJson() const {
    nlohmann::json out;
    nlohmann::json runner_status = runner_ ? runner_->statusJson() : nlohmann::json::object();
    {
        std::scoped_lock lk(mu_);
        out["enabled"]        = cfg_.enabled;
        out["schedule_cron"]  = cfg_.schedule_cron;
        out["scheduler_armed"] = (scheduler_ != nullptr);
        out["report_dir"]     = cfg_.report_dir;
        out["last_report_id"] = last_report_id_;
    }
    out["runner"] = std::move(runner_status);
    return out;
}

nlohmann::json StressService::listReportsJson() const {
    std::scoped_lock lk(mu_);
    if (!store_) return nlohmann::json();
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& e : store_->list()) {
        arr.push_back({
            {"id",            e.id},
            {"started_at_ms", e.started_at_ms},
            {"ended_at_ms",   e.ended_at_ms},
            {"pass",          e.pass},
            {"verdict",       e.verdict},
        });
    }
    return arr;
}

std::optional<nlohmann::json>
StressService::readReportJson(const std::string& id) const {
    std::scoped_lock lk(mu_);
    if (!store_) return std::nullopt;
    return store_->read(id);
}

bool StressService::removeReport(const std::string& id) {
    std::scoped_lock lk(mu_);
    if (!store_) return false;
    return store_->remove(id);
}

}  // namespace liveqx::stress
