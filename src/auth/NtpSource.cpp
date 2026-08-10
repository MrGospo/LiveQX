// fix33 A3 — NtpSource implementation.

#include "auth/NtpSource.h"

#include <algorithm>
#include <utility>

namespace liveqx::auth {

namespace {

// "host" → {"host", 123}; "host:port" → {"host", port}; невалидный port → 123.
std::pair<std::string, int> splitHostPort(const std::string& spec) {
    const auto colon = spec.rfind(':');
    if (colon == std::string::npos) return {spec, 123};
    // IPv6-literal "[::1]:123" — отрезаем скобки.
    if (!spec.empty() && spec.front() == '[') {
        const auto rbr = spec.find(']');
        if (rbr == std::string::npos) return {spec, 123};
        const std::string host = spec.substr(1, rbr - 1);
        if (rbr + 1 >= spec.size() || spec[rbr + 1] != ':') return {host, 123};
        try {
            return {host, std::stoi(spec.substr(rbr + 2))};
        } catch (...) { return {host, 123}; }
    }
    try {
        return {spec.substr(0, colon), std::stoi(spec.substr(colon + 1))};
    } catch (...) { return {spec, 123}; }
}

std::int64_t systemUnixSec() noexcept {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}  // namespace

NtpSource::NtpSource(NtpSettings                  settings,
                     std::shared_ptr<ISntpClient> sntp,
                     SyncCallback                 on_sync)
    : settings_(std::move(settings)),
      sntp_(std::move(sntp)),
      on_sync_(std::move(on_sync)) {
    // Применяем "ранее сохранённый" offset до первого sync'а — чтобы
    // now() уже был осмысленным, если БД помнит вчерашнее значение.
    if (settings_.last_offset_ms) {
        offset_ms_.store(*settings_.last_offset_ms, std::memory_order_relaxed);
    }

    // Warm-up sync — best-effort, не падаем если все серверы недоступны.
    if (settings_.enabled && sntp_ && !settings_.servers.empty()) {
        (void)syncNow();
    }

    if (settings_.enabled) {
        worker_ = std::jthread([this](std::stop_token st) { pollLoop(st); });
    }
}

NtpSource::~NtpSource() {
    if (worker_.joinable()) {
        worker_.request_stop();
        cv_.notify_all();
        // jthread join'ится в dtor'е автоматически.
    }
}

std::chrono::system_clock::time_point NtpSource::now() const {
    return std::chrono::system_clock::now() +
           std::chrono::milliseconds(offset_ms_.load(std::memory_order_relaxed));
}

std::int64_t NtpSource::offsetMs() const {
    return offset_ms_.load(std::memory_order_relaxed);
}

bool NtpSource::syncNow() {
    if (!sntp_ || settings_.servers.empty()) return false;

    for (const auto& spec : settings_.servers) {
        auto [host, port] = splitHostPort(spec);
        if (host.empty()) continue;
        auto r = sntp_->query(host, port);
        if (!r) continue;

        offset_ms_.store(r->offset_ms, std::memory_order_relaxed);
        if (on_sync_) {
            on_sync_(r->offset_ms, systemUnixSec());
        }
        return true;
    }
    return false;
}

void NtpSource::pollLoop(std::stop_token st) {
    // Минимум 5 секунд — защита от runaway-цикла, даже если конфиг битый.
    const auto interval = std::chrono::seconds(
        std::max(5, settings_.poll_interval_s));

    while (!st.stop_requested()) {
        std::unique_lock lk(cv_mu_);
        cv_.wait_for(lk, st, interval, [&] { return st.stop_requested(); });
        if (st.stop_requested()) break;
        lk.unlock();
        (void)syncNow();
    }
}

}  // namespace liveqx::auth
