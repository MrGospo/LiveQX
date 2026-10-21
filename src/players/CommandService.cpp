#include "players/CommandService.h"

#include <nlohmann/json.hpp>

#include <ctime>

namespace liveqx::players {
namespace {

std::int64_t nowSeconds() { return static_cast<std::int64_t>(std::time(nullptr)); }

std::string rfc3339(std::int64_t unix_s) {
    std::time_t t = static_cast<std::time_t>(unix_s);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

// A restarted machine has been up for less than the time since the command,
// allowing for clocks that differ by a minute.
constexpr std::int64_t kClockSlackS = 60;

}  // namespace

CommandService::CommandService(PlayersDb& db, PlayersService& players, CommandServiceOptions opts)
    : db_(db), players_(players), opts_(std::move(opts)) {}

bool CommandService::supportedType(const std::string& type) {
    return type == kRestartPlayer || type == kRebootScreen || type == kReloadPlaylist;
}

int CommandService::confirmTimeoutFor(const PlayersSettings& s, const std::string& type) {
    if (type == kRebootScreen)  return s.confirm_timeout_reboot_s;
    if (type == kReloadPlaylist) return s.confirm_timeout_reload_s;
    return s.confirm_timeout_restart_s;
}

void CommandService::announce(const CommandRow& c) {
    if (!opts_.events) return;
    nlohmann::json payload = {{"player_id", c.player_id}, {"command_id", c.id}, {"type", c.type},
                              {"state", c.state}, {"result", c.result}};
    // Where the screen sits now, so that whoever sees only part of the tree
    // gets only its own news.
    if (auto place = db_.placeOfPlayer(c.player_id)) {
        payload["group_id"]  = place->group_id;
        payload["object_id"] = place->object_id;
    }
    opts_.events->publish(events::EventType::PlayerCommand, -1, std::move(payload));
}

CommandOutcome CommandService::sendToPlayer(const std::string& player_id, const std::string& type,
                                            std::optional<std::int64_t> issued_by, std::int64_t now) {
    CommandOutcome out;
    if (!supportedType(type)) { out.error = "unsupported_type"; return out; }
    auto p = db_.findPlayer(player_id);
    if (!p) { out.error = "player_not_found"; return out; }
    if (p->state != PlayerState::Active) { out.error = "player_not_active"; return out; }
    if (!opts_.publish) { out.error = "broker_unavailable"; return out; }
    if (now == 0) now = nowSeconds();

    const PlayersSettings s = players_.settings();
    CommandRow row;
    row.id          = PlayersDb::newId();
    row.player_id   = player_id;
    row.type        = type;
    row.state       = "pending";
    row.issued_at   = now;
    row.ttl_seconds = s.command_ttl_s;
    row.deadline_at = now + confirmTimeoutFor(s, type);
    row.issued_by   = issued_by;

    // Recorded before it is sent: the block must exist the moment the command
    // can arrive, and a second request meets it.
    switch (db_.createCommand(row, &out.busy_id)) {
        case CommandCreate::Ok: break;
        case CommandCreate::Busy: out.error = "command_in_progress"; return out;
        case CommandCreate::Storage: out.error = "storage_error"; return out;
    }

    mqtt::Message m;
    m.topic   = "liveqx/v1/players/" + player_id + "/commands";
    m.payload = nlohmann::json{{"id", row.id}, {"issued_at", rfc3339(now)},
                               {"ttl_seconds", row.ttl_seconds}, {"type", type}}.dump();
    m.qos       = 1;
    m.retain    = false;                                   // a retained command would repeat at every reconnect
    m.expiry_s  = static_cast<std::uint32_t>(row.ttl_seconds);
    const mqtt::PublishResult pr = opts_.publish(m);
    if (pr != mqtt::PublishResult::Ok) {
        // It never left: do not leave the screen blocked for it.
        auto settled = db_.settleCommand(row.id, "not_sent",
                                         pr == mqtt::PublishResult::NotConnected ? "broker_not_connected"
                                                                                 : "broker_error", now);
        out.error = "broker_unavailable";
        if (settled) { out.row = *settled; announce(*settled); }
        return out;
    }

    out.ok  = true;
    out.row = row;
    if (auto st = db_.status(player_id)) out.player_online = st->online;
    db_.addEvent(player_id, "command_sent",
                 nlohmann::json{{"command_id", row.id}, {"type", type}}.dump(), now);
    announce(row);
    return out;
}

void CommandService::onResult(const std::string& player_id, const std::string& command_id,
                              const std::string& status, std::optional<std::int64_t> os_uptime_s,
                              std::int64_t now) {
    if (now == 0) now = nowSeconds();
    auto cmd = db_.findCommand(command_id);
    // Only the screen the command went to can settle it; a repeated report of
    // an already settled command changes nothing.
    if (!cmd || cmd->player_id != player_id || cmd->state != "pending") return;

    std::string state, result = status;
    if (status == "executed" || status == "duplicate") {
        state = "executed";
        // A screen that was told to reboot must have rebooted: the machine's
        // own uptime shows it. A player that does not report uptime is believed.
        if (cmd->type == kRebootScreen && os_uptime_s &&
            now - *os_uptime_s < cmd->issued_at - kClockSlackS) {
            state  = "failed";
            result = "os_not_rebooted";
        }
    } else {
        state = "failed";                               // expired, unsupported, failed
    }
    if (auto settled = db_.settleCommand(command_id, state, result, now)) {
        db_.addEvent(player_id, "command_" + state,
                     nlohmann::json{{"command_id", command_id}, {"type", cmd->type}, {"result", result}}.dump(), now);
        announce(*settled);
    }
}

std::size_t CommandService::sweep(std::int64_t now) {
    const auto timed_out = db_.timeoutCommands(now);
    for (const auto& c : timed_out) {
        db_.addEvent(c.player_id, "command_timeout",
                     nlohmann::json{{"command_id", c.id}, {"type", c.type}}.dump(), now);
        announce(c);
    }
    return timed_out.size();
}

std::optional<CommandRow> CommandService::release(const std::string& command_id, std::int64_t now) {
    auto settled = db_.settleCommand(command_id, "released", "released_by_admin", now);
    if (!settled) return std::nullopt;
    db_.addEvent(settled->player_id, "command_released",
                 nlohmann::json{{"command_id", command_id}, {"type", settled->type}}.dump(), now);
    announce(*settled);
    return settled;
}

}  // namespace liveqx::players
