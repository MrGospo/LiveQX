#pragma once
//
// IMqttClient on libmosquitto (MQTT 5, TLS with a client certificate).
//
// TLS material is handed to the library as an in-memory SSL_CTX, so the
// server's private key (which administers the broker) never touches the disk.
// The first connection and its retries run on a thread of ours (the library's
// MQTT 5 connect is blocking); once connected, the library's own thread keeps
// the session alive and reconnects with a growing pause.

#include "mqtt/MqttClient.h"

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

struct mosquitto;

namespace liveqx::mqtt {

class MosquittoClient final : public IMqttClient {
public:
    explicit MosquittoClient(ClientOptions opts);
    ~MosquittoClient() override;
    MosquittoClient(const MosquittoClient&)            = delete;
    MosquittoClient& operator=(const MosquittoClient&) = delete;

    void setMessageHandler(MessageHandler h) override;
    void setStateHandler(StateHandler h) override;
    bool start() override;
    void stop() override;
    bool connected() const override { return connected_.load(); }
    PublishResult publish(const Message& m) override;
    void subscribe(const std::string& filter, int qos) override;
    std::string lastError() const override;

private:
    struct Callbacks;                       // libmosquitto trampolines
    friend struct Callbacks;

    void connectLoop();
    void onConnect(int reason);
    void onDisconnect(int reason);
    void setError(std::string e);
    void notifyState(bool up, const std::string& detail);

    ClientOptions opts_;
    mosquitto*    mosq_{nullptr};
    std::thread   connector_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> started_{false};
    std::atomic<bool> connected_{false};

    mutable std::mutex mu_;
    MessageHandler     on_message_;
    StateHandler       on_state_;
    std::vector<std::pair<std::string, int>> subscriptions_;
    std::string        last_error_;
};

}  // namespace liveqx::mqtt
