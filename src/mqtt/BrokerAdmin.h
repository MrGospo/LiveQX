#pragma once
//
// Administration of the MQTT broker (Mosquitto Dynamic Security) by the server.
//
// The server holds one MQTT connection (IMqttClient) under its own certificate
// identity; that identity is the broker's administrator. Through it the server
// issues each player what the broker needs before the player's first
// connection (A12): a client record named by the player id, a personal role
// with literal topics, and the shared role for the broadcast topic.
//
// Everything is idempotent: issuing twice is the same as once, removing what is
// not there is fine. That is what makes the periodic reconciliation (the broker
// brought in line with the player database) safe to run at any time, and what
// lets a pairing succeed while the broker is down — the reconciliation catches up.
//
// Replies to the administrative topic arrive on the shared message handler of
// the client; the owner routes messages here with handleMessage().

#include "mqtt/MqttClient.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace liveqx::mqtt {

struct BrokerAdminResult {
    bool        ok{false};
    // Not connected, timeout, rejected, or the first real error the broker reported.
    std::string error;
    nlohmann::json data;         // `data` of the last reply that carried some
};

struct ReconcileReport {
    bool        ok{false};
    std::string error;
    std::size_t issued{0};       // players that were missing on the broker
    std::size_t removed{0};      // clients/roles on the broker that no player owns
    std::size_t failed{0};
};

class BrokerAdmin {
public:
    static constexpr const char* kControlTopic  = "$CONTROL/dynamic-security/v1";
    static constexpr const char* kResponseTopic = "$CONTROL/dynamic-security/v1/response";
    static constexpr const char* kServerClient  = "liveqx-server";
    static constexpr const char* kCommonRole    = "player-common";
    static constexpr const char* kServerRole    = "server-ops";
    static constexpr const char* kPlayerRolePrefix = "player-";
    static constexpr const char* kBroadcastTopic = "liveqx/v1/broadcast/commands";

    struct Options {
        std::chrono::milliseconds reply_timeout{5000};
    };

    BrokerAdmin(IMqttClient& client, Options opts);
    explicit BrokerAdmin(IMqttClient& client) : BrokerAdmin(client, Options{}) {}

    // Routes a message of the shared handler: true if it was a reply to us.
    bool handleMessage(const Message& m);

    // Topics of the model (A12).
    static std::string commandsTopic(const std::string& player_id);
    static std::string presenceTopic(const std::string& player_id);

    // The command lists, exposed for tests.
    // Without the role bindings: binding twice is answered by the plugin with a
    // bare "Internal error" (2.0.18), so bindings are only sent for roles the
    // client does not have yet (see ensureBindings()).
    static nlohmann::json baselineCommands();
    static nlohmann::json playerCommands(const std::string& player_id);
    using Binding = std::pair<std::string, int>;            // role, priority
    static std::vector<Binding> playerBindings(const std::string& player_id);
    static std::vector<Binding> serverBindings();
    static nlohmann::json removalCommands(const std::string& player_id);

    // Default-deny, the shared roles, and the server's own working rights.
    BrokerAdminResult ensureBaseline();
    // Client + personal role + shared role for one player.
    BrokerAdminResult provisionPlayer(const std::string& player_id);
    // Removes the player's client and role (and so closes its rights at once).
    BrokerAdminResult removePlayer(const std::string& player_id);
    // Names of all clients / roles on the broker.
    BrokerAdminResult listClients(std::set<std::string>& out);
    BrokerAdminResult listRoles(std::set<std::string>& out);

    // Brings the broker in line with `wanted` (player ids that must exist):
    // issues the missing, removes player clients/roles nobody owns. Anything
    // that is not a player id (the server itself, the shared roles) is never touched.
    ReconcileReport reconcile(const std::set<std::string>& wanted);

    // A player id as the model names it (lowercase UUID).
    static bool looksLikePlayerId(const std::string& s);

private:
    // Sends `commands` as one message and waits for the reply.
    BrokerAdminResult run(nlohmann::json commands);
    // Binds `username` to the roles it does not have yet.
    BrokerAdminResult ensureBindings(const std::string& username, const std::vector<Binding>& wanted);

    IMqttClient&  client_;
    Options       opts_;

    std::mutex    call_mu_;           // one administrative call at a time
    std::mutex    mu_;
    std::condition_variable cv_;
    std::map<std::string, nlohmann::json> replies_;     // by correlation id
    std::string   waiting_;           // correlation id of the call that awaits a reply
    unsigned long counter_{0};
};

}  // namespace liveqx::mqtt
