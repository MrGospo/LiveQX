#pragma once
//
// The server's MQTT client, behind an interface so everything that talks to
// the broker (commands, presence, broker administration) can be tested without
// one. The real implementation is MosquittoClient (libmosquitto).
//
// What every implementation promises:
//   * start() never blocks on the network: it begins connecting in the
//     background and keeps trying (with a growing pause) until stop();
//   * subscriptions are remembered and renewed at every (re)connect;
//   * publish() on a broken connection fails loudly (NotConnected) instead of
//     queueing without bound — the caller decides whether that is an error to
//     show or a message to retry;
//   * callbacks run on the client's own thread: they must be quick and must not
//     call back into the client in a way that waits for that thread.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace liveqx::mqtt {

struct Message {
    std::string  topic;
    std::string  payload;
    int          qos{1};
    bool         retain{false};
    // MQTT 5 message expiry: after this many seconds (counted by the BROKER'S
    // clock) the broker drops the message instead of delivering it late.
    std::optional<std::uint32_t> expiry_s;
};

struct ClientOptions {
    std::string host{"127.0.0.1"};
    int         port{8883};
    // The identity of the persistent session. Messages for an offline client
    // queue on the broker under this id.
    std::string client_id;

    // TLS material, in memory (PEM). The private key is never written to disk.
    std::string ca_pem;               // roots that may vouch for the broker's certificate
    std::string cert_chain_pem;       // our certificate, then intermediates
    std::string key_pem;
    bool        verify_hostname{true};

    int           keepalive_s{30};
    bool          clean_start{false};
    std::uint32_t session_expiry_s{3600};   // how long the broker keeps the session
    int           reconnect_min_s{1};
    int           reconnect_max_s{30};
};

enum class PublishResult {
    Ok,
    NotConnected,
    Rejected,        // the message itself is unacceptable (empty topic, wildcard, too large, ...)
    Failed,
};

class IMqttClient {
public:
    using MessageHandler = std::function<void(const Message&)>;
    // `detail` says why a connection was lost or refused; empty when it came up.
    using StateHandler   = std::function<void(bool connected, const std::string& detail)>;

    virtual ~IMqttClient() = default;

    // Handlers are set before start().
    virtual void setMessageHandler(MessageHandler h) = 0;
    virtual void setStateHandler(StateHandler h) = 0;

    // False if the configuration is unusable (nothing is started then).
    virtual bool start() = 0;
    virtual void stop() = 0;
    virtual bool connected() const = 0;

    virtual PublishResult publish(const Message& m) = 0;
    // Remembered and renewed at every reconnect.
    virtual void subscribe(const std::string& filter, int qos = 1) = 0;

    // The last problem worth showing an administrator ("" if none).
    virtual std::string lastError() const = 0;
};

}  // namespace liveqx::mqtt
