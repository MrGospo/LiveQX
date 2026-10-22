#include "mqtt/BrokerAdmin.h"

#include <cctype>

namespace liveqx::mqtt {
namespace {

using nlohmann::json;

json aclCommand(const std::string& role, const char* type, const std::string& topic, bool allow = true) {
    return {{"command", "addRoleACL"}, {"rolename", role}, {"acltype", type},
            {"topic", topic}, {"priority", 0}, {"allow", allow}};
}

// What the broker says when the thing is already as we want it. Anything else
// is a real failure.
bool benign(const std::string& command, const std::string& error) {
    if (command == "deleteClient" || command == "deleteRole")
        return error.find("not found") != std::string::npos;
    if (command == "createClient" || command == "createRole" || command == "addRoleACL" ||
        false)
        return error.find("already") != std::string::npos;
    return false;
}

}  // namespace

BrokerAdmin::BrokerAdmin(IMqttClient& client, Options opts) : client_(client), opts_(opts) {
    // Replies come back on this topic; subscribing here (before the client is
    // started) means every reconnect renews it.
    client_.subscribe(kResponseTopic, 1);
}

std::string BrokerAdmin::commandsTopic(const std::string& id) { return "liveqx/v1/players/" + id + "/commands"; }
std::string BrokerAdmin::presenceTopic(const std::string& id) { return "liveqx/v1/players/" + id + "/presence"; }

bool BrokerAdmin::looksLikePlayerId(const std::string& s) {
    if (s.size() != 36) return false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (c != '-') return false; continue; }
        if (!std::isxdigit(static_cast<unsigned char>(c)) || std::isupper(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

json BrokerAdmin::baselineCommands() {
    const std::string common = kCommonRole, ops = kServerRole;
    json c = json::array();
    c.push_back({{"command", "setDefaultACLAccess"},
                 {"acls", json::array({{{"acltype", "publishClientSend"}, {"allow", false}},
                                       {{"acltype", "publishClientReceive"}, {"allow", false}},
                                       {{"acltype", "subscribe"}, {"allow", false}},
                                       {{"acltype", "unsubscribe"}, {"allow", false}}})}});
    c.push_back({{"command", "createRole"}, {"rolename", common}});
    c.push_back(aclCommand(common, "subscribeLiteral", kBroadcastTopic));
    c.push_back(aclCommand(common, "publishClientReceive", kBroadcastTopic));
    c.push_back({{"command", "createRole"}, {"rolename", ops}});
    c.push_back(aclCommand(ops, "publishClientSend", "liveqx/v1/players/+/commands"));
    c.push_back(aclCommand(ops, "publishClientSend", kBroadcastTopic));
    c.push_back(aclCommand(ops, "subscribePattern", "liveqx/v1/players/+/presence"));
    c.push_back(aclCommand(ops, "publishClientReceive", "liveqx/v1/players/+/presence"));
    // To wipe the retained "online"/"offline" of a player that no longer exists.
    c.push_back(aclCommand(ops, "publishClientSend", "liveqx/v1/players/+/presence"));
    return c;
}

std::vector<BrokerAdmin::Binding> BrokerAdmin::serverBindings() { return {{kServerRole, 5}}; }

std::vector<BrokerAdmin::Binding> BrokerAdmin::playerBindings(const std::string& id) {
    return {{kCommonRole, 1}, {kPlayerRolePrefix + id, 2}};
}

json BrokerAdmin::playerCommands(const std::string& id) {
    const std::string role = kPlayerRolePrefix + id;
    json c = json::array();
    c.push_back({{"command", "createClient"}, {"username", id}});
    c.push_back({{"command", "createRole"}, {"rolename", role}});
    c.push_back(aclCommand(role, "publishClientSend", presenceTopic(id)));
    c.push_back(aclCommand(role, "subscribeLiteral", commandsTopic(id)));
    c.push_back(aclCommand(role, "publishClientReceive", commandsTopic(id)));
    return c;
}

json BrokerAdmin::removalCommands(const std::string& id) {
    return json::array({{{"command", "deleteClient"}, {"username", id}},
                        {{"command", "deleteRole"}, {"rolename", kPlayerRolePrefix + id}}});
}

bool BrokerAdmin::handleMessage(const Message& m) {
    if (m.topic != kResponseTopic) return false;
    json j = json::parse(m.payload, nullptr, false);
    if (!j.is_object() || !j.contains("responses") || !j["responses"].is_array()) return true;
    std::string corr;
    for (const auto& r : j["responses"])
        if (r.is_object() && r.contains("correlationData") && r["correlationData"].is_string()) {
            corr = r["correlationData"].get<std::string>();
            break;
        }
    if (corr.empty()) return true;
    {
        std::lock_guard<std::mutex> lk(mu_);
        // Only the call that is waiting keeps its reply; a late one is dropped.
        if (corr == waiting_) replies_[corr] = std::move(j);
    }
    cv_.notify_all();
    return true;
}

BrokerAdminResult BrokerAdmin::run(json commands) {
    BrokerAdminResult out;
    std::lock_guard<std::mutex> call(call_mu_);
    std::string corr;
    {
        std::lock_guard<std::mutex> lk(mu_);
        corr = "lqx-" + std::to_string(++counter_);
        waiting_ = corr;
        replies_.erase(corr);
    }
    for (auto& c : commands) c["correlationData"] = corr;

    Message m;
    m.topic   = kControlTopic;
    m.payload = json{{"commands", commands}}.dump();
    m.qos     = 1;
    const PublishResult pr = client_.publish(m);
    if (pr != PublishResult::Ok) {
        std::lock_guard<std::mutex> lk(mu_);
        waiting_.clear();
        out.error = pr == PublishResult::NotConnected ? "broker_not_connected"
                  : pr == PublishResult::Rejected     ? "broker_rejected_request"
                                                      : "broker_publish_failed";
        return out;
    }

    json reply;
    {
        std::unique_lock<std::mutex> lk(mu_);
        const bool got = cv_.wait_for(lk, opts_.reply_timeout, [&] { return replies_.count(corr) != 0; });
        waiting_.clear();
        if (!got) { out.error = "broker_no_reply"; return out; }
        reply = std::move(replies_[corr]);
        replies_.erase(corr);
    }

    // Walk the answers in the order of the commands.
    for (const auto& a : reply["responses"]) {
        const std::string cmd = a.value("command", "");
        if (a.contains("data")) out.data = a["data"];
        if (a.contains("error") && a["error"].is_string()) {
            const std::string err = a["error"].get<std::string>();
            if (!benign(cmd, err)) { out.error = cmd + ": " + err; return out; }
        }
    }
    out.ok = true;
    return out;
}

BrokerAdminResult BrokerAdmin::ensureBindings(const std::string& username, const std::vector<Binding>& wanted) {
    auto have = run(json::array({{{"command", "getClient"}, {"username", username}}}));
    if (!have.ok) return have;
    std::set<std::string> bound;
    if (have.data.is_object() && have.data.contains("client") && have.data["client"].contains("roles"))
        for (const auto& r : have.data["client"]["roles"])
            if (r.is_object() && r.contains("rolename") && r["rolename"].is_string())
                bound.insert(r["rolename"].get<std::string>());
    json add = json::array();
    for (const auto& [role, priority] : wanted)
        if (!bound.count(role))
            add.push_back({{"command", "addClientRole"}, {"username", username},
                           {"rolename", role}, {"priority", priority}});
    if (add.empty()) { BrokerAdminResult ok; ok.ok = true; return ok; }
    return run(std::move(add));
}

BrokerAdminResult BrokerAdmin::ensureBaseline() {
    auto r = run(baselineCommands());
    if (!r.ok) return r;
    return ensureBindings(kServerClient, serverBindings());
}

BrokerAdminResult BrokerAdmin::provisionPlayer(const std::string& id) {
    auto r = run(playerCommands(id));
    if (!r.ok) return r;
    return ensureBindings(id, playerBindings(id));
}
BrokerAdminResult BrokerAdmin::removePlayer(const std::string& id) {
    auto r = run(removalCommands(id));
    if (r.ok) clearPresence(id);
    return r;
}

bool BrokerAdmin::clearPresence(const std::string& id) {
    // An empty retained message deletes the retained one. Done AFTER the client
    // is gone: removing it makes the broker publish its will ("offline"), and
    // that must not be the last word.
    Message m;
    m.topic  = presenceTopic(id);
    m.retain = true;
    m.qos    = 1;
    return client_.publish(m) == PublishResult::Ok;
}

BrokerAdminResult BrokerAdmin::listClients(std::set<std::string>& out) {
    auto r = run(json::array({{{"command", "listClients"}, {"verbose", false}}}));
    if (!r.ok) return r;
    out.clear();
    if (r.data.is_object() && r.data.contains("clients") && r.data["clients"].is_array())
        for (const auto& c : r.data["clients"])
            if (c.is_string()) out.insert(c.get<std::string>());
    return r;
}

BrokerAdminResult BrokerAdmin::listRoles(std::set<std::string>& out) {
    auto r = run(json::array({{{"command", "listRoles"}, {"verbose", false}}}));
    if (!r.ok) return r;
    out.clear();
    if (r.data.is_object() && r.data.contains("roles") && r.data["roles"].is_array())
        for (const auto& c : r.data["roles"])
            if (c.is_string()) out.insert(c.get<std::string>());
    return r;
}

ReconcileReport BrokerAdmin::reconcile(const std::set<std::string>& wanted,
                                       const std::set<std::string>& rebuild) {
    ReconcileReport rep;
    std::set<std::string> clients, roles;
    if (auto r = ensureBaseline(); !r.ok) { rep.error = r.error; return rep; }
    if (auto r = listClients(clients); !r.ok) { rep.error = r.error; return rep; }
    if (auto r = listRoles(roles); !r.ok) { rep.error = r.error; return rep; }

    // Dropped first, so the loop below makes them again as if they were missing.
    for (const auto& id : rebuild) {
        if (!wanted.count(id) || !(clients.count(id) || roles.count(kPlayerRolePrefix + id))) continue;
        if (!removePlayer(id).ok) { ++rep.failed; continue; }
        clients.erase(id);
        roles.erase(kPlayerRolePrefix + id);
    }

    for (const auto& id : wanted) {
        if (clients.count(id) && roles.count(kPlayerRolePrefix + id)) continue;
        if (provisionPlayer(id).ok) ++rep.issued; else ++rep.failed;
    }

    std::set<std::string> stale;
    for (const auto& c : clients)
        if (looksLikePlayerId(c) && !wanted.count(c)) stale.insert(c);
    const std::string prefix = kPlayerRolePrefix;
    for (const auto& r : roles)
        if (r.size() > prefix.size() && r.compare(0, prefix.size(), prefix) == 0) {
            const std::string id = r.substr(prefix.size());
            if (looksLikePlayerId(id) && !wanted.count(id)) stale.insert(id);
        }
    for (const auto& id : stale)
        if (removePlayer(id).ok) ++rep.removed; else ++rep.failed;

    rep.ok = rep.failed == 0;
    if (!rep.ok) rep.error = "some players could not be reconciled";
    return rep;
}

}  // namespace liveqx::mqtt
