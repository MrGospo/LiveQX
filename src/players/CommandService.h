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
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace liveqx::players {

// Where a job stands, counted from its screens and their commands.
struct JobSummary {
    std::size_t total{0};
    std::size_t queued{0};            // not yet sent
    std::size_t pending{0};           // sent, waiting for the screen
    std::size_t executed{0};          // sent and confirmed
    std::size_t unconfirmed{0};       // sent, and refused / timed out / released
    std::size_t failed{0};            // could not be sent
    std::size_t skipped_offline{0};   // not connected (at the start or when its turn came)
    std::size_t cancelled{0};
};

struct JobItemView {
    JobItem     item;
    std::string command_state;        // empty if nothing was sent
    std::string command_result;
};

struct JobView {
    JobRow                   job;
    JobSummary               summary;
    std::vector<JobItemView> items;
};

struct CommandServiceOptions {
    // Hands a message to the broker; null = no broker in this setup.
    std::function<mqtt::PublishResult(const mqtt::Message&)> publish;
    events::EventBus* events{nullptr};
    // A command of a job was sent (any batch): for the audit trail.
    std::function<void(const CommandRow&, const JobRow&)> on_bulk_sent;
    // A job ended: done, stopped, or cancelled.
    std::function<void(const JobRow&, const JobSummary&)> on_job_end;
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

// What a group, object or "everyone" command is aimed at.
enum class TargetKind { Group, Object, All };

struct CommandTarget {
    TargetKind   kind{TargetKind::All};
    std::int64_t id{0};          // group or object id; unused for All
};

struct BusyScreen {
    std::string player_id;
    std::string command_id;      // the unsettled command of this type that holds the block
};

// What a bulk command would do right now. Nothing is changed by asking.
struct BulkPlan {
    bool        found{false};    // the target exists (and, for All, there is something)
    std::string target_name;     // "Main / Hall", "Main", "all"
    std::vector<std::string> online;      // active screens that are connected: they get the command
    std::vector<std::string> offline;     // active screens that are not: skipped, reported
    std::vector<BusyScreen>  busy;        // among the online: already waiting for this command
    // The phrase a person must type to confirm; differs for group, object and all.
    std::string required_phrase;
    // Number of active screens in the target, connected or not. The interface
    // fills it in; a different number now means the target changed under the person.
    std::size_t total{0};
};

struct BulkOutcome {
    bool        ok{false};
    // unsupported_type, target_not_found, no_targets, confirm_phrase_mismatch,
    // expected_count_mismatch, screens_busy, broker_unavailable
    std::string error;
    BulkPlan    plan;
    std::string job_id;
    std::size_t queued{0};            // waiting for a later batch
    std::int64_t batch_size{0};
    struct Sent   { std::string player_id, command_id; };
    struct Failed { std::string player_id, error; };
    std::vector<Sent>   sent;
    std::vector<Failed> failed;
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

    // Looks at what a command to this target would do, without doing it.
    BulkPlan plan(const CommandTarget& target, const std::string& type);

    // The phrase that confirms `type` aimed at a target of this kind and name.
    static std::string requiredPhrase(const std::string& type, TargetKind kind, const std::string& name);

    // Sends `type` to every connected active screen of the target. The person's
    // phrase and the count they were shown must match the present state, and no
    // targeted screen may be waiting for the same command: otherwise nothing is sent.
    // The connected screens are split into batches; the first goes out now, the
    // rest as each previous batch is settled (see tick()). `sent` and `failed`
    // describe the first batch.
    BulkOutcome sendToTarget(const CommandTarget& target, const std::string& type,
                             const std::string& confirm_phrase, std::size_t expected_count,
                             std::optional<std::int64_t> issued_by, std::int64_t now = 0,
                             const std::string& actor_name = "", const std::string& actor_role = "");

    // Moves every running job on: waits for a batch to be settled, stops the job
    // if too many of it did not come back, otherwise sends the next batch.
    // Called often; cheap when nothing is due.
    void tick(std::int64_t now = 0);

    // Stops the rest of a running job. What already went out stays. nullopt if
    // the job is unknown or already over.
    std::optional<JobRow> cancelJob(const std::string& job_id, std::int64_t now = 0);

    std::optional<JobView> jobView(const std::string& job_id);

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
    // Runs a job as far as it can go right now. Caller holds jobs_mu_.
    void advance(JobRow job, std::int64_t now);
    void endJob(const std::string& job_id, const std::string& state, const std::string& reason,
                std::int64_t now, bool notify);

    PlayersDb&            db_;
    PlayersService&       players_;
    CommandServiceOptions opts_;
    std::mutex            jobs_mu_;       // one thing at a time may move jobs on
};

}  // namespace liveqx::players
