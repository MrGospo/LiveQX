#include "auth/TimeConfigRepo.h"

#include <chrono>

#include <nlohmann/json.hpp>

#include "utils/Log.h"

namespace liveqx::auth {
namespace {

using nlohmann::json;

std::string serversToJson(const std::vector<std::string>& servers) {
    json arr = json::array();
    for (const auto& s : servers) arr.push_back(s);
    return arr.dump();
}

std::vector<std::string> serversFromJson(const std::string& s) {
    std::vector<std::string> out;
    if (s.empty()) return out;
    try {
        auto arr = json::parse(s);
        if (!arr.is_array()) return out;
        for (const auto& v : arr) {
            if (v.is_string()) out.push_back(v.get<std::string>());
        }
    } catch (...) {
        // Грязный JSON в БД — игнорируем, отдаём пустой список.
    }
    return out;
}

std::int64_t nowUnixSec() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

}  // namespace

TimeConfigRepo::TimeConfigRepo(AuthDb& db) : db_(db) {}

std::optional<TimeConfig> TimeConfigRepo::load() {
    auto row = db_.readSystemTimeConfigRow();
    if (!row.has_value()) return std::nullopt;

    TimeConfig cfg;
    if (auto s = timeSourceFromString(row->source); s.has_value()) {
        cfg.source = *s;
    }
    cfg.server_timezone        = row->server_timezone.empty()
                                   ? std::string("UTC")
                                   : row->server_timezone;
    cfg.ntp.enabled            = row->ntp_enabled;
    cfg.ntp.servers            = serversFromJson(row->ntp_servers_json);
    cfg.ntp.poll_interval_s    = row->ntp_poll_interval_s;
    cfg.ntp.last_offset_ms     = row->ntp_last_offset_ms;
    cfg.ntp.last_sync_at       = row->ntp_last_sync_at;
    cfg.manual.offset_ms       = row->manual_offset_ms;
    cfg.manual.set_at          = row->manual_set_at;
    return cfg;
}

bool TimeConfigRepo::save(const TimeConfig& cfg) {
    AuthDb::SystemTimeConfigRow row;
    row.source              = timeSourceToString(cfg.source);
    row.server_timezone     = cfg.server_timezone;
    row.ntp_enabled         = cfg.ntp.enabled;
    row.ntp_servers_json    = serversToJson(cfg.ntp.servers);
    row.ntp_poll_interval_s = cfg.ntp.poll_interval_s;
    row.ntp_last_offset_ms  = cfg.ntp.last_offset_ms;
    row.ntp_last_sync_at    = cfg.ntp.last_sync_at;
    row.manual_offset_ms    = cfg.manual.offset_ms;
    row.manual_set_at       = cfg.manual.set_at;
    row.updated_at          = nowUnixSec();

    if (!db_.writeSystemTimeConfigRow(row)) {
        LOG_ERROR("TimeConfigRepo::save: writeSystemTimeConfigRow failed");
        return false;
    }
    return true;
}

bool TimeConfigRepo::updateNtpSyncResult(std::int64_t offset_ms,
                                        std::int64_t sync_at_unix_sec) {
    return db_.updateSystemTimeNtpSync(offset_ms, sync_at_unix_sec);
}

std::string TimeConfigRepo::validate(const TimeConfig& cfg) {
    if (!isValidIanaTimezone(cfg.server_timezone)) {
        return "invalid IANA timezone: " + cfg.server_timezone;
    }
    if (cfg.source == TimeSource::Ntp) {
        if (cfg.ntp.servers.empty()) {
            return "ntp source requires at least one server";
        }
        if (cfg.ntp.poll_interval_s < 5 || cfg.ntp.poll_interval_s > 3600) {
            return "ntp poll_interval_s must be in [5, 3600]";
        }
        for (const auto& s : cfg.ntp.servers) {
            if (s.empty()) return "ntp server entry is empty";
        }
    }
    // Manual offset защищаем от абсурдных значений: ±100 лет в мс.
    constexpr std::int64_t kMaxOffsetMs = 100LL * 365 * 24 * 3600 * 1000;
    if (cfg.manual.offset_ms < -kMaxOffsetMs ||
        cfg.manual.offset_ms >  kMaxOffsetMs) {
        return "manual offset_ms out of plausible range";
    }
    return "";
}

}  // namespace liveqx::auth
