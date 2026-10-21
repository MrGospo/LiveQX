#pragma once
//
// The server's standing connection to the MQTT broker and the upkeep of the
// broker's player records.
//
// What it does, on its own thread:
//   * waits until the server has what it needs to connect (the device CA, so
//     the server's identity can be issued; the root the broker's certificate is
//     checked against) and only then connects — a missing piece is "not ready
//     yet", never an error that stops the server;
//   * keeps the connection under the current identity: when the identity is
//     re-issued (expiry, CA generation) it reconnects with the new one;
//   * reconciles the broker with the player database (BrokerAdmin::reconcile):
//     on request (a player was created or revoked), after every (re)connect,
//     and on a timer — so a broker that was down during a pairing catches up.
//
// Everything outside is injected, so the logic is tested without a broker.

#include "mqtt/BrokerAdmin.h"
#include "mqtt/MqttClient.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace liveqx::mqtt {

struct BrokerIdentityPem {
    std::string cert_chain_pem;     // leaf then intermediates
    std::string key_pem;            // SECRET, memory only
    std::string serial;             // changes when the identity is re-issued
};

struct BrokerLinkOptions {
    std::string host{"127.0.0.1"};
    int         port{8883};
    bool        verify_hostname{true};
    int         reconcile_interval_s{300};
    int         retry_s{5};

    // false + reason while the server cannot yet have an identity.
    std::function<bool(BrokerIdentityPem&, std::string& why)> identity;
    // The root(s) the broker's certificate is checked against; empty = not yet.
    std::function<std::string()> trust_pem;
    // Player ids that must exist on the broker (not revoked, not removed).
    std::function<std::set<std::string>()> wanted;
    std::function<std::unique_ptr<IMqttClient>(const ClientOptions&)> make_client;
    BrokerAdmin::Options admin;
    // Topics to listen to besides the administrative replies, and where their
    // messages go. Called on the client's thread: keep it short.
    std::vector<std::string> subscriptions;
    std::function<void(const Message&)> on_message;
};

struct BrokerLinkStatus {
    // waiting (nothing to connect with yet), connecting, connected, stopped
    std::string     state{"waiting"};
    std::string     detail;               // why waiting / the last problem
    std::int64_t    last_reconcile_at{0}; // unix seconds, 0 = never
    bool            last_reconcile_ok{false};
    ReconcileReport last_report;
};

class BrokerLink {
public:
    explicit BrokerLink(BrokerLinkOptions opts);
    ~BrokerLink();
    BrokerLink(const BrokerLink&)            = delete;
    BrokerLink& operator=(const BrokerLink&) = delete;

    void start();
    void stop();

    // Something changed in the player set: reconcile soon. Never blocks.
    void requestSync();

    BrokerLinkStatus status() const;

    // Asks for the retained message of a topic to be deleted (an empty retained
    // publish) as soon as the link is up. Safe to call from a message handler:
    // it only queues, the link's own thread does the publishing.
    void clearRetained(const std::string& topic);

    // Publishes through the standing connection. NotConnected while there is
    // none (not set up yet, broker down); never blocks on the network.
    PublishResult publish(const Message& m);

private:
    void run();
    bool bringUp(std::string& why);
    void tearDown();
    void setStatus(const std::string& state, const std::string& detail);
    void wait(int seconds);

    BrokerLinkOptions opts_;
    std::thread       thread_;
    std::atomic<bool> stopping_{false};

    mutable std::mutex mu_;
    std::condition_variable cv_;
    bool             dirty_{true};              // a reconcile is due
    std::set<std::string> clear_retained_;      // topics waiting to be wiped
    bool             poke_{false};              // wake the thread (a request or a state change)
    BrokerLinkStatus status_;

    // Replaced by the thread only, read by publish() under client_mu_.
    mutable std::mutex client_mu_;
    std::unique_ptr<IMqttClient> client_;
    std::unique_ptr<BrokerAdmin> admin_;
    std::string                  serial_;
    std::int64_t                 next_tick_{0};
    bool                         was_up_{false};
};

}  // namespace liveqx::mqtt
