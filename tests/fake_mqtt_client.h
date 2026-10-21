#pragma once
// An in-memory IMqttClient for tests of code that talks to the broker, with an
// optional stand-in for Dynamic Security so the administrative traffic can be
// exercised without a real Mosquitto.
//
// The Dynamic Security stand-in keeps clients and roles in sets and answers
// like the real plugin does for the cases we rely on ("... already exists",
// "... not found"). It is a stand-in for logic tests; the real plugin is
// covered by the live-broker tests.

#include "mqtt/MqttClient.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <map>
#include <mutex>
#include <set>
#include <vector>

namespace liveqx::testing {

class FakeMqttClient : public mqtt::IMqttClient {
public:
    void setMessageHandler(MessageHandler h) override { handler_ = std::move(h); }
    void setStateHandler(StateHandler h) override { state_handler_ = std::move(h); }
    bool start() override { return true; }
    void stop() override {}
    bool connected() const override { return up; }
    void subscribe(const std::string& filter, int) override {
        std::lock_guard<std::mutex> lk(mu_);
        subscriptions.push_back(filter);
    }
    std::string lastError() const override { return {}; }

    mqtt::PublishResult publish(const mqtt::Message& m) override {
        if (!up) return mqtt::PublishResult::NotConnected;
        {
            std::lock_guard<std::mutex> lk(mu_);
            published.push_back(m);
        }
        if (dynsec && m.topic == "$CONTROL/dynamic-security/v1" && handler_ && !silent) {
            mqtt::Message reply;
            reply.topic   = "$CONTROL/dynamic-security/v1/response";
            reply.payload = answer(nlohmann::json::parse(m.payload)).dump();
            handler_(reply);
        }
        return mqtt::PublishResult::Ok;
    }

    // A message arrives from the broker.
    void deliver(const mqtt::Message& m) { if (handler_) handler_(m); }

    // The link goes up or down, and whoever listens is told.
    void setLink(bool v) {
        up = v;
        if (state_handler_) state_handler_(v, v ? "" : "connection lost");
    }

    // --- knobs and records ---------------------------------------------
    std::atomic<bool> up{true};
    bool dynsec{false};           // answer administrative commands
    bool silent{false};           // swallow them (to test the timeout)
    std::vector<mqtt::Message> published;
    std::vector<std::string>   subscriptions;
    std::set<std::string> clients, roles;          // the stand-in's state
    std::map<std::string, std::set<std::string>> bindings;   // client -> roles
    std::vector<std::string> commandLog;           // every command name, in order

    std::size_t countCommand(const std::string& name) const {
        std::size_t n = 0;
        for (const auto& c : commandLog) if (c == name) ++n;
        return n;
    }

private:
    nlohmann::json answer(const nlohmann::json& req) {
        using nlohmann::json;
        json out = json::array();
        for (const auto& c : req["commands"]) {
            const std::string name = c.value("command", "");
            commandLog.push_back(name);
            json r = {{"command", name}, {"correlationData", c.value("correlationData", "")}};
            const std::string user = c.value("username", "");
            const std::string role = c.value("rolename", "");
            if (name == "createClient") {
                if (!clients.insert(user).second) r["error"] = "Client already exists";
            } else if (name == "createRole") {
                if (!roles.insert(role).second) r["error"] = "Role already exists";
            } else if (name == "addClientRole") {
                if (!clients.count(user)) r["error"] = "Client not found";
                else if (!roles.count(role)) r["error"] = "Role not found";
                else if (!bindings[user].insert(role).second) r["error"] = "Internal error";   // as 2.0.18 does
            } else if (name == "addRoleACL") {
                if (!roles.count(role)) r["error"] = "Role not found";
            } else if (name == "deleteClient") {
                bindings.erase(user);
                if (!clients.erase(user)) r["error"] = "Client not found";
            } else if (name == "deleteRole") {
                if (!roles.erase(role)) r["error"] = "Role not found";
            } else if (name == "getClient") {
                if (!clients.count(user)) { r["error"] = "Client not found"; }
                else {
                    json rs = json::array();
                    for (const auto& b : bindings[user]) rs.push_back({{"rolename", b}});
                    r["data"] = {{"client", {{"username", user}, {"roles", rs}}}};
                }
            } else if (name == "listClients") {
                r["data"] = {{"totalCount", clients.size()}, {"clients", clients}};
            } else if (name == "listRoles") {
                r["data"] = {{"totalCount", roles.size()}, {"roles", roles}};
            }
            // setDefaultACLAccess: accepted.
            out.push_back(std::move(r));
        }
        return {{"responses", out}};
    }

    MessageHandler handler_;
    StateHandler   state_handler_;
    std::mutex     mu_;
};

}  // namespace liveqx::testing
