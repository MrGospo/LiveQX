#pragma once
//
// How the server connects to the MQTT broker: the part an administrator may
// change at run time. (Whether the link exists at all, `players.broker.enabled`,
// stays in the configuration file.)

#include <algorithm>
#include <cctype>
#include <string>

namespace liveqx::mqtt {

struct BrokerConnection {
    std::string host{"127.0.0.1"};
    int         port{8883};
    bool        verify_hostname{true};
    int         reconcile_interval_s{300};

    bool operator==(const BrokerConnection& o) const {
        return host == o.host && port == o.port && verify_hostname == o.verify_hostname &&
               reconcile_interval_s == o.reconcile_interval_s;
    }
    bool operator!=(const BrokerConnection& o) const { return !(*this == o); }
};

// "" if usable; otherwise invalid_host, invalid_port or invalid_reconcile_interval_s.
inline std::string validateBrokerConnection(const BrokerConnection& c) {
    if (c.host.empty() || c.host.size() > 253 || c.host.front() == '-' ||
        !std::all_of(c.host.begin(), c.host.end(), [](unsigned char ch) {
            return std::isalnum(ch) || ch == '.' || ch == '-' || ch == '_' || ch == ':';
        }))
        return "invalid_host";
    if (c.port < 1 || c.port > 65535) return "invalid_port";
    if (c.reconcile_interval_s < 10 || c.reconcile_interval_s > 86400) return "invalid_reconcile_interval_s";
    return "";
}

}  // namespace liveqx::mqtt
