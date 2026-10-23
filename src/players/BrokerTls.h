#pragma once
//
// The TLS material the MQTT broker needs from us before it can start, besides
// the revocation list (CrlPublisher):
//
//   cafile.pem   the certificates of the device CA, every generation (what the
//                players' client certificates are checked against). Public.
//   broker.crt / broker.key
//                the broker's own certificate for its listener, signed by the
//                SERVER TLS CA (the root the players are told to trust in the
//                pairing package), valid for the host the players are given in
//                `mqtt_broker_url`.
//
// The server writes them, so that nothing about them is fixed at install time:
// the device CA does not exist until the ceremony, and the host in
// `mqtt_broker_url` is a setting an administrator can change later.
//
// The certificate is "managed" when the server holds the key of the root the
// pairing package carries: it is issued, and issued again when it is about to
// run out or the host changed. Otherwise (an administrator brought their own
// root) the certificate is "provided": the operator puts broker.crt and
// broker.key in the directory, and the server only checks them and says so.
//
// Nothing here talks to the broker; the caller tells it to re-read when
// `changed` comes back true.

#include "utils/DeviceCa.h"
#include "utils/Tls.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace liveqx::players {

struct BrokerTlsOptions {
    std::filesystem::path dir;                        // where cafile.pem, broker.crt, broker.key live
    std::function<std::vector<int>()> generations;    // of the device CA
    std::function<std::unique_ptr<tls::DeviceCa>(int generation, std::string& err)> open_generation;
    std::filesystem::path server_ca_dir;              // ca.crt + ca.key of the server TLS CA, if we hold it
    std::function<std::string()> trust_root_pem;      // the root the pairing package carries
    std::function<std::string()> host;                // host of mqtt_broker_url; "" = not set yet
    std::vector<std::string> san_extra;               // further names/addresses for the certificate
    int validity_days{365};
    int renew_before_days{30};
    int backdate_s{86400};                            // players' clocks drift (D-8)
    std::function<std::int64_t()> now;                // unix seconds; null = real clock
};

struct BrokerTlsState {
    // "managed" (we issue it), "provided" (the operator's), "missing" (not there or not usable)
    std::string  certificate{"missing"};
    std::int64_t not_after{0};
    std::string  error;                               // "" when all is in place
};

class BrokerTls {
public:
    explicit BrokerTls(BrokerTlsOptions opts);

    // Brings cafile.pem and the certificate into the state described above. Returns "" or a short
    // reason; `changed` is set when something was written (the broker should re-read).
    std::string ensure(bool& changed);

    BrokerTlsState state() const;

    // "mqtts://screens.example.com:8883" -> "screens.example.com"; "" when there is no host in it.
    static std::string hostOfBrokerUrl(const std::string& url);

private:
    std::string ensureCafile(bool& changed);
    std::string ensureCertificate(bool& changed);

    BrokerTlsOptions opts_;
    mutable std::mutex mu_;
    BrokerTlsState state_;
};

}  // namespace liveqx::players
