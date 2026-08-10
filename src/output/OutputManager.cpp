#include "output/OutputManager.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <utility>

bool OutputManager::addDriver(std::string id, std::shared_ptr<IOutput> driver,
                               std::uint64_t queue_bytes_limit) {
    if (!driver) return false;
    std::unique_lock lk(mu_);
    const auto dup = std::any_of(entries_.begin(), entries_.end(),
        [&](const std::unique_ptr<Entry>& e) { return e->id == id; });
    if (dup) return false;

    auto entry               = std::make_unique<Entry>();
    entry->id                = std::move(id);
    entry->driver            = std::move(driver);
    entry->queue_bytes_limit = queue_bytes_limit ? queue_bytes_limit
                                                  : kDefaultQueueBytesLimit;
    auto* raw                = entry.get();
    entry->pump              = std::jthread(
        [this, raw](std::stop_token st) { pumpLoop(raw, st); });
    entries_.push_back(std::move(entry));
    return true;
}

bool OutputManager::removeDriver(const std::string& id) {
    std::unique_ptr<Entry> taken;
    {
        std::unique_lock lk(mu_);
        const auto it = std::find_if(entries_.begin(), entries_.end(),
            [&](const std::unique_ptr<Entry>& e) { return e->id == id; });
        if (it == entries_.end()) return false;
        taken = std::move(*it);
        entries_.erase(it);
    }
    // Join outside the lock so a still-running send() can complete and
    // a slow driver->send() inside the pump can't deadlock with us.
    stopAndJoin(*taken);
    return true;
}

std::shared_ptr<IOutput> OutputManager::getDriver(const std::string& id) const {
    std::shared_lock lk(mu_);
    const auto it = std::find_if(entries_.begin(), entries_.end(),
        [&](const std::unique_ptr<Entry>& e) { return e->id == id; });
    return it == entries_.end() ? nullptr : (*it)->driver;
}

void OutputManager::clear() {
    std::vector<std::unique_ptr<Entry>> taken;
    {
        std::unique_lock lk(mu_);
        taken.swap(entries_);
    }
    for (auto& e : taken) stopAndJoin(*e);
}

void OutputManager::send(const Packet& pkt) {
    // shared_lock keeps the hot encoder path lock-free against itself —
    // only addDriver/removeDriver/clear take the unique lock.
    const std::uint64_t pkt_bytes = pkt.data.size();
    std::shared_lock lk(mu_);
    for (auto& e : entries_) {
        // Per-driver byte budget. Pre-check is best-effort against a
        // concurrent pump consume — slight over/under is acceptable.
        const auto in_flight = e->queue_bytes.load(std::memory_order_acquire);
        if (in_flight + pkt_bytes > e->queue_bytes_limit) {
            e->drops.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        if (!e->queue.push(pkt)) {
            e->drops.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        e->queue_bytes.fetch_add(pkt_bytes, std::memory_order_acq_rel);
        e->queue.notify();
    }
}

bool OutputManager::allHealthy() const {
    std::shared_lock lk(mu_);
    return std::all_of(entries_.begin(), entries_.end(),
        [](const std::unique_ptr<Entry>& e) { return e->driver->isHealthy(); });
}

OutputManager::HealthCounts OutputManager::healthCounts() const {
    HealthCounts c;
    std::shared_lock lk(mu_);
    c.total = static_cast<int>(entries_.size());
    for (auto& e : entries_)
        if (e->driver->isHealthy()) ++c.healthy;
    return c;
}

nlohmann::json OutputManager::statusJson() const {
    nlohmann::json arr = nlohmann::json::array();
    std::shared_lock lk(mu_);
    for (auto& e : entries_) {
        auto j = e->driver->statusJson();
        j["id"]                = e->id;
        j["queue_drops"]       = e->drops.load(std::memory_order_relaxed);
        j["queue_bytes_used"]  = e->queue_bytes.load(std::memory_order_relaxed);
        j["queue_bytes_limit"] = e->queue_bytes_limit;
        arr.push_back(std::move(j));
    }
    return arr;
}

OutputManager::~OutputManager() {
    clear();
}

bool OutputManager::startAll() {
    // Snapshot under shared_lock so a long-running start() (RTMP handshake,
    // SRT listener bind) doesn't block REST hot-add or the encoder send
    // path. Hot-add concurrent with play() is a race we accept — the new
    // driver's REST handler is responsible for its own start().
    std::vector<std::shared_ptr<IOutput>> drivers;
    {
        std::shared_lock lk(mu_);
        drivers.reserve(entries_.size());
        for (auto& e : entries_) drivers.push_back(e->driver);
    }
    for (auto& d : drivers) {
        if (!d->start()) return false;
    }
    return true;
}

void OutputManager::stopAll() {
    std::vector<std::shared_ptr<IOutput>> drivers;
    {
        std::shared_lock lk(mu_);
        drivers.reserve(entries_.size());
        for (auto& e : entries_) drivers.push_back(e->driver);
    }
    for (auto& d : drivers) d->stop();
}

void OutputManager::pumpLoop(Entry* e, std::stop_token st) {
    using namespace std::chrono_literals;
    while (!st.stop_requested()) {
        auto p = e->queue.pop_wait(100ms);
        if (!p) continue;
        const std::uint64_t bytes = p->data.size();
        e->driver->send(*p);
        // Decrement after the driver consumes — pre-decrement would let
        // send() overshoot the limit during back-pressure.
        e->queue_bytes.fetch_sub(bytes, std::memory_order_acq_rel);
    }
}

void OutputManager::stopAndJoin(Entry& e) {
    e.pump.request_stop();
    // Wake the pump if it's parked inside pop_wait().
    e.queue.notify_all();
    if (e.pump.joinable()) e.pump.join();
}
