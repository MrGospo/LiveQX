#include "players/BrokerSettings.h"

namespace liveqx::players {

using nlohmann::json;

json brokerConnectionJson(const mqtt::BrokerConnection& c) {
    return {{"host", c.host}, {"port", c.port}, {"verify_hostname", c.verify_hostname},
            {"reconcile_interval_s", c.reconcile_interval_s}};
}

BrokerUpdateResult updateBrokerConnection(const json& body, PlayersService& players, mqtt::BrokerLink* link) {
    BrokerUpdateResult out;
    auto fail = [&](int status, const std::string& code, json extra = json::object()) {
        out.status = status;
        out.body = std::move(extra);
        out.body["error"] = code;
        return out;
    };
    if (!link) return fail(409, "broker_disabled");
    if (!body.is_object()) return fail(400, "invalid_body");

    const mqtt::BrokerConnection current = link->connection();
    mqtt::BrokerConnection next = current;
    bool check_only = false, skip_check = false;
    for (auto it = body.begin(); it != body.end(); ++it) {
        const std::string& k = it.key();
        const json& v = it.value();
        if (k == "host") {
            if (!v.is_string()) return fail(400, "invalid_host");
            next.host = v.get<std::string>();
        } else if (k == "port") {
            if (!v.is_number_integer()) return fail(400, "invalid_port");
            const auto n = v.get<std::int64_t>();
            next.port = (n < 0 || n > 1000000) ? 0 : static_cast<int>(n);       // out of range -> invalid_port below
        } else if (k == "verify_hostname") {
            if (!v.is_boolean()) return fail(400, "invalid_verify_hostname");
            next.verify_hostname = v.get<bool>();
        } else if (k == "reconcile_interval_s") {
            if (!v.is_number_integer()) return fail(400, "invalid_reconcile_interval_s");
            const auto n = v.get<std::int64_t>();
            next.reconcile_interval_s = (n < 0 || n > 10000000) ? 0 : static_cast<int>(n);
        } else if (k == "check_only") {
            if (!v.is_boolean()) return fail(400, "invalid_check_only");
            check_only = v.get<bool>();
        } else if (k == "skip_check") {
            if (!v.is_boolean()) return fail(400, "invalid_skip_check");
            skip_check = v.get<bool>();
        } else {
            return fail(400, "unknown_field", {{"field", k}});
        }
    }
    if (check_only && skip_check) return fail(400, "check_only_and_skip_check");
    if (auto e = mqtt::validateBrokerConnection(next); !e.empty()) return fail(400, e);

    json changed = json::array();
    if (next.host != current.host) changed.push_back("host");
    if (next.port != current.port) changed.push_back("port");
    if (next.verify_hostname != current.verify_hostname) changed.push_back("verify_hostname");
    if (next.reconcile_interval_s != current.reconcile_interval_s) changed.push_back("reconcile_interval_s");

    // Where the connection goes (or how strictly it is checked) is what can cut the
    // server off; a different period cannot.
    const bool reaches_somewhere_new = next.host != current.host || next.port != current.port ||
                                       next.verify_hostname != current.verify_hostname;
    const bool must_probe = check_only || (reaches_somewhere_new && !skip_check);

    out.audit = {{"changed", changed}, {"check_only", check_only}, {"skip_check", skip_check},
                 {"verify_hostname", next.verify_hostname}};

    if (must_probe) {
        const auto r = link->probe(next);
        out.audit["probe_ok"] = r.ok;
        if (check_only) {
            out.status = 200;
            out.body = {{"checked", true}, {"ok", r.ok}, {"detail", r.error}, {"connection", brokerConnectionJson(next)}};
            return out;
        }
        if (!r.ok) {
            out.status = 502;
            out.body = {{"error", "probe_failed"}, {"detail", r.error}, {"connection", brokerConnectionJson(current)}};
            return out;
        }
    }

    if (next == current) {
        out.status = 200;
        out.body = {{"applied", false}, {"unchanged", true}, {"connection", brokerConnectionJson(current)}};
        return out;
    }
    if (auto e = players.storeBrokerConnection(next); !e.empty()) return fail(e == "storage_error" ? 500 : 400, e);
    link->reconfigure(next);

    out.status = 200;
    out.body = {{"applied", true}, {"checked", must_probe}, {"connection", brokerConnectionJson(next)}};
    if (!next.verify_hostname) out.body["warning"] = "verify_hostname_disabled";
    return out;
}

}  // namespace liveqx::players
