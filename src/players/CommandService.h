#pragma once
//
// Commands to players (A12/A13, B13): sending one to a screen, keeping the
// screen blocked against a second identical command until the first is settled,
// and settling it — from the player's own report in a heartbeat, by timeout,
// or by an administrator's hand.
//
// The command itself travels over MQTT (published through an injected
// function, so this layer knows nothing about the broker). The confirmation
// travels over HTTP: the player reports `last_command_result` in a heartbeat.
//
// Nothing here knows about HTTP or the audit log; the handlers translate the
// returned error codes and write the audit trail.

#include "events/EventBus.h"
#include "mqtt/MqttClient.h"
#include "players/PlayersDb.h"
#include "players/PlayersService.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace liveqx::players {

struct CommandServiceOptions {
    // Hands a message to the broker; null = no broker in this setup.
    std::function<mqtt::PublishResult(const mqtt::Message&)> publish;
    events::EventBus* events{nullptr};
};

struct CommandOutcome {
    bool        ok{false};
    // unsupported_type, player_not_found, player_not_active, broker_unavailable,
    // command_in_progress, storage_error
    std::string error;
    CommandRow  row;
    std::string busy_id;          // command_in_progress: the command that holds the block
    bool        player_online{false};
};

class CommandService {
public:
    static constexpr const char* kRestartPlayer = "restart_player";
    static constexpr const char* kRebootScreen  = "reboot_screen";
    static constexpr const char* kReloadPlaylist = "reload_playlist";

    CommandService(PlayersDb& db, PlayersService& players, CommandServiceOptions opts);

    // The types a player understands today (update_now waits for a later phase).
    static bool supportedType(const std::string& type);

    // Sends `type` to one screen. `now` = 0 reads the clock.
    CommandOutcome sendToPlayer(const std::string& player_id, const std::string& type,
                                std::optional<std::int64_t> issued_by, std::int64_t now = 0);

    // The player's report on a command, taken from a heartbeat. Only the
    // player the command was sent to can settle it; anything else is ignored.
    // `os_uptime_s` is how long the machine has been running, for reboot_screen.
    void onResult(const std::string& player_id, const std::string& command_id,
                  const std::string& status, std::optional<std::int64_t> os_uptime_s,
                  std::int64_t now = 0);

    // Settles commands nobody confirmed in time. Returns how many.
    std::size_t sweep(std::int64_t now = 0);

    // An administrator lifts the block by hand. nullopt if the command is
    // unknown or already settled.
    std::optional<CommandRow> release(const std::string& command_id, std::int64_t now = 0);

    // Seconds a screen is given to confirm a command of this type.
    static int confirmTimeoutFor(const PlayersSettings& s, const std::string& type);

private:
    void announce(const CommandRow& c);

    PlayersDb&            db_;
    PlayersService&       players_;
    CommandServiceOptions opts_;
};

}  // namespace liveqx::players
