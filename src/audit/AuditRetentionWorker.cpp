#include "audit/AuditRetentionWorker.h"

#include <chrono>

#include "audit/AuditDb.h"
#include "audit/AuditTypes.h"
#include "utils/Log.h"

namespace liveqx::audit {
namespace {

constexpr Category kAllCategories[] = {
    Category::Auth, Category::Channel, Category::Output, Category::Gateway,
    Category::Plugin, Category::Mount, Category::System, Category::Access,
};

std::int64_t nowUnixMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

}  // namespace

AuditRetentionWorker::AuditRetentionWorker(AuditDb* db,
                                           std::chrono::minutes run_interval)
    : db_(db), interval_(run_interval) {}

AuditRetentionWorker::~AuditRetentionWorker() { stop(); }

void AuditRetentionWorker::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) return;
    stopping_ = false;
    worker_ = std::thread([this] { loop(); });
    LOG_INFO("AuditRetentionWorker started (interval={}min)", interval_.count());
}

void AuditRetentionWorker::stop() {
    if (!running_.exchange(false)) return;
    stopping_ = true;
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

int AuditRetentionWorker::runOnce() {
    if (!db_ || !db_->ok()) return 0;
    const std::int64_t now_ms = nowUnixMs();
    int total = 0;
    for (auto cat : kAllCategories) {
        const int days = defaultRetentionDays(cat);
        if (days <= 0) continue;   // 0 = keep forever
        const std::int64_t cutoff =
            now_ms - static_cast<std::int64_t>(days) * 86400LL * 1000LL;
        const int removed = db_->purgeOlderThan(cat, cutoff);
        if (removed > 0) {
            LOG_INFO("AuditRetentionWorker: purged {} rows category={} older_than_days={}",
                     removed, categoryName(cat), days);
            total += removed;
        }
    }
    return total;
}

void AuditRetentionWorker::loop() {
    // Run once on start so a freshly-restarted daemon closes overdue
    // windows quickly instead of waiting a full interval.
    runOnce();

    std::unique_lock<std::mutex> lk(mu_);
    while (!stopping_.load()) {
        if (cv_.wait_for(lk, interval_, [this] { return stopping_.load(); }))
            break;
        lk.unlock();
        runOnce();
        lk.lock();
    }
}

}  // namespace liveqx::audit
