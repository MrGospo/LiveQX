#pragma once
//
// Changing how the server connects to the MQTT broker, from the interface
// (PUT /api/players/broker).
//
// A wrong address cuts the server off from the broker, and with it commands and
// presence — so a change that points somewhere new is first tried with a
// throwaway connection under the server's own identity, and applied only if the
// broker answers. What is applied is stored (it survives a restart and wins over
// the configuration file) and the running link switches to it without a restart.
// Rights on the broker are kept by the broker itself, not by the connection: a
// reconnect changes none of them, and a different broker is brought in line by
// the usual reconciliation.
//
// Nothing here knows about HTTP; the handler passes the body and sends back the
// status and JSON it is given.

#include "mqtt/BrokerLink.h"
#include "players/PlayersService.h"

#include <nlohmann/json.hpp>

namespace liveqx::players {

struct BrokerUpdateResult {
    int            status{200};
    nlohmann::json body;
    nlohmann::json audit;       // what to record: which fields, whether it was checked
};

// Body: any of host (string), port (integer), verify_hostname (boolean),
// reconcile_interval_s (integer); absent = unchanged. Also:
//   check_only: true   try the connection, change nothing
//   skip_check: true   apply without trying (for a broker that is not up yet)
// Answers: 200 applied / unchanged / checked, 400 invalid_* or unknown_field,
// 409 broker_disabled (no link), 502 probe_failed (nothing was changed),
// 500 storage_error.
BrokerUpdateResult updateBrokerConnection(const nlohmann::json& body, PlayersService& players,
                                          mqtt::BrokerLink* link);

nlohmann::json brokerConnectionJson(const mqtt::BrokerConnection& c);

}  // namespace liveqx::players
