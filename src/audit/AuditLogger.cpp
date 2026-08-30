// Async audit writer with emergency JSONL fallback.
// See AuditLogger.h for the full contract.

#include "audit/AuditLogger.h"

#include <chrono>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

#include "audit/AuditDb.h"
#include "utils/Log.h"

namespace liveqx::audit {
namespace {

using json = nlohmann::json;

std::int64_t nowMonotonicNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::int64_t nowUnixMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

}  // namespace

AuditLogger::AuditLogger(AuditDb* db, std::filesystem::path emergency_file)
    : db_(db), emergency_path_(std::move(emergency_file)) {}

AuditLogger::~AuditLogger() {
    stop();
}

void AuditLogger::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) return;
    stopping_ = false;
    writer_ = std::thread([this] { writerLoop(); });
    LOG_INFO("AuditLogger started (db={}, emergency={})",
             db_ && db_->ok() ? db_->path().string() : "<absent>",
             emergency_path_.string());
}

void AuditLogger::stop() {
    if (!running_.exchange(false)) return;
    stopping_ = true;
    queue_cv_.notify_all();
    if (writer_.joinable()) writer_.join();
    // Flush any residual events synchronously so a graceful shutdown does
    // not lose the trail. The queue is protected by queue_mu_ against
    // concurrent log() calls (unlikely at this point but cheap).
    std::vector<AuditEvent> tail;
    {
        std::lock_guard<std::mutex> lk(queue_mu_);
        tail.reserve(queue_.size());
        for (auto& e : queue_) tail.push_back(std::move(e));
        queue_.clear();
    }
    if (!tail.empty()) drainBatch(tail);
}

bool AuditLogger::shouldRejectMutation() const noexcept {
    std::lock_guard<std::mutex> lk(queue_mu_);
    return queue_.size() > kBacklogHardCap;
}

void AuditLogger::log(AuditEvent ev) {
    if (ev.ts_unix_ms == 0) ev.ts_unix_ms = nowUnixMs();
    stat_enqueued_.fetch_add(1, std::memory_order_relaxed);

    bool wake = false;
    bool over_hard = false;
    {
        std::lock_guard<std::mutex> lk(queue_mu_);
        // Hard cap: spill directly to emergency file so we never silently
        // drop an event while the writer thread is stuck.
        if (queue_.size() > kBacklogHardCap) {
            over_hard = true;
        } else {
            queue_.push_back(std::move(ev));
            wake = queue_.size() >= kBatchMax;
        }
    }
    if (over_hard) {
        // Emergency spill under emergency_mu_ (not queue_mu_).
        std::lock_guard<std::mutex> elk(emergency_mu_);
        if (!writeEmergency(ev)) {
            stat_dropped_.fetch_add(1, std::memory_order_relaxed);
            LOG_ERROR("AuditLogger: dropped event action={} — queue over "
                      "hard cap and emergency file unwritable", ev.action);
        }
        return;
    }
    if (wake) queue_cv_.notify_one();
}

void AuditLogger::logSyncBrokenGlass(AuditEvent ev) {
    if (ev.ts_unix_ms == 0) ev.ts_unix_ms = nowUnixMs();
    stat_enqueued_.fetch_add(1, std::memory_order_relaxed);

    if (db_ && db_->ok()) {
        auto id = db_->insert(ev);
        if (id.has_value()) {
            stat_written_db_.fetch_add(1, std::memory_order_relaxed);
            stat_last_write_ns_.store(nowMonotonicNs(),
                                      std::memory_order_relaxed);
            return;
        }
        stat_db_failures_.fetch_add(1, std::memory_order_relaxed);
    }
    std::lock_guard<std::mutex> elk(emergency_mu_);
    if (!writeEmergency(ev)) {
        stat_dropped_.fetch_add(1, std::memory_order_relaxed);
        LOG_ERROR("AuditLogger: broken-glass write dropped action={} — "
                  "DB unavailable and emergency file unwritable", ev.action);
    }
}

void AuditLogger::writerLoop() {
    std::vector<AuditEvent> batch;
    batch.reserve(kBatchMax);

    while (!stopping_.load(std::memory_order_relaxed)) {
        {
            std::unique_lock<std::mutex> lk(queue_mu_);
            queue_cv_.wait_for(lk, kFlushInterval, [this] {
                return stopping_.load(std::memory_order_relaxed) ||
                       queue_.size() >= kBatchMax;
            });
            const std::size_t take = std::min(queue_.size(), kBatchMax);
            for (std::size_t i = 0; i < take; ++i) {
                batch.push_back(std::move(queue_.front()));
                queue_.pop_front();
            }
            if (queue_.size() > kBacklogSoftCap) {
                LOG_WARN("AuditLogger: backlog {} > soft cap {}",
                         queue_.size(), kBacklogSoftCap);
            }
        }
        if (batch.empty()) continue;
        drainBatch(batch);
        batch.clear();
    }
}

std::size_t AuditLogger::drainBatch(std::vector<AuditEvent>& batch) {
    if (batch.empty()) return 0;

    if (db_ && db_->ok()) {
        const auto written = db_->insertBatch(batch);
        if (written == batch.size()) {
            stat_written_db_.fetch_add(written, std::memory_order_relaxed);
            stat_last_write_ns_.store(nowMonotonicNs(),
                                      std::memory_order_relaxed);
            return written;
        }
        // Partial or complete failure — spill everything to emergency
        // rather than trying to reconcile which rows landed.
        stat_db_failures_.fetch_add(1, std::memory_order_relaxed);
        LOG_ERROR("AuditLogger: batch DB insert failed ({}/{}), "
                  "spilling to emergency", written, batch.size());
    }

    std::lock_guard<std::mutex> elk(emergency_mu_);
    std::size_t ok = 0;
    for (const auto& ev : batch) {
        if (writeEmergency(ev)) ++ok;
        else stat_dropped_.fetch_add(1, std::memory_order_relaxed);
    }
    stat_written_emerg_.fetch_add(ok, std::memory_order_relaxed);
    if (ok > 0) stat_last_write_ns_.store(nowMonotonicNs(),
                                          std::memory_order_relaxed);
    return ok;
}

bool AuditLogger::writeEmergency(const AuditEvent& ev) {
    // Lazy open. Once open we keep the stream around; a write error
    // closes it so the next attempt reopens fresh.
    if (!emergency_stream_.is_open()) {
        std::error_code ec;
        if (emergency_path_.has_parent_path()) {
            std::filesystem::create_directories(emergency_path_.parent_path(),
                                                ec);
        }
        emergency_stream_.open(emergency_path_,
                               std::ios::app | std::ios::binary);
        if (!emergency_stream_.is_open()) {
            LOG_ERROR("AuditLogger: cannot open emergency file {}",
                      emergency_path_.string());
            return false;
        }
    }
    const std::string line = toJsonLine(ev);
    emergency_stream_.write(line.data(),
                            static_cast<std::streamsize>(line.size()));
    emergency_stream_.put('\n');
    emergency_stream_.flush();
    if (!emergency_stream_.good()) {
        LOG_ERROR("AuditLogger: emergency write failed on {}",
                  emergency_path_.string());
        emergency_stream_.close();
        return false;
    }
    return true;
}

std::string AuditLogger::toJsonLine(const AuditEvent& ev) {
    // Emergency lines are self-describing so ops can replay them into
    // audit.db later. prev_mac/mac/key_fingerprint are absent — the
    // replay recomputes the chain against the current tail.
    json j;
    j["ts_unix_ms"]     = ev.ts_unix_ms;
    j["category"]       = categoryName(ev.category);
    j["action"]         = ev.action;
    if (ev.actor_user_id) j["actor_user_id"] = *ev.actor_user_id;
    else                  j["actor_user_id"] = nullptr;
    j["actor_username"] = ev.actor_username;
    j["actor_role"]     = ev.actor_role;
    j["actor_ip"]       = ev.actor_ip;
    j["target_type"]    = ev.target_type;
    j["target_id"]      = ev.target_id;
    j["http_method"]    = ev.http_method;
    j["http_path"]      = ev.http_path;
    j["http_status"]    = ev.http_status;
    j["elapsed_ms"]     = ev.elapsed_ms;
    j["summary"]        = ev.summary;
    j["details_json"]   = ev.details_json;
    j["request_id"]     = ev.request_id;
    return j.dump();
}

AuditLoggerStats AuditLogger::stats() const {
    AuditLoggerStats s;
    s.enqueued         = stat_enqueued_.load(std::memory_order_relaxed);
    s.written_db       = stat_written_db_.load(std::memory_order_relaxed);
    s.written_emergency= stat_written_emerg_.load(std::memory_order_relaxed);
    s.db_failures      = stat_db_failures_.load(std::memory_order_relaxed);
    s.dropped_overflow = stat_dropped_.load(std::memory_order_relaxed);
    s.last_write_ns    = stat_last_write_ns_.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(queue_mu_);
        s.queue_depth  = queue_.size();
        s.fail_closed  = queue_.size() > kBacklogHardCap;
    }
    return s;
}

}  // namespace liveqx::audit
