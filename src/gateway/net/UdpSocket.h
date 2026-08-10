#pragma once
//
// Reusable IPv4 UDP receive-socket helpers extracted from DemuxGateway so the
// PSI probe endpoint can bind + join multicast + read for a bounded interval
// without dragging in the whole gateway lifecycle.
//
// Kept intentionally free of spdlog/InputCfg deps — callers convert their own
// config into UdpRxOptions and format the error string as they see fit.

#include <cstdint>
#include <string>

struct in_addr;

namespace liveqx::gateway::net {

// Resolve a NIC name (e.g. "eth0", "lo") to its first IPv4 address.
// Returns false if the interface isn't found or has no AF_INET address.
bool nicNameToIPv4(const std::string& name, in_addr& out) noexcept;

// Options for opening a receive-only UDP socket.
//   * address = unicast (bound as-is) or multicast (bound INADDR_ANY + join).
//   * interface_name / interface_addr — same semantics as InputCfg: whichever
//     is set is used for BINDTODEVICE (unicast) or IP_ADD_MEMBERSHIP interface
//     selection (multicast). interface_addr wins over interface_name if both.
//   * recv_buffer_kb — SO_RCVBUF requested (kernel may cap).
//   * rcv_timeout_ms — SO_RCVTIMEO so recv() unblocks periodically for
//     cooperative cancellation. Set to 0 to leave the socket fully blocking.
struct UdpRxOptions {
    std::string address;
    int         port           = 0;
    std::string interface_name;
    std::string interface_addr;
    int         recv_buffer_kb = 1024;
    int         rcv_timeout_ms = 200;
};

// Open the socket and configure it per opts. Returns fd on success, -1 on
// failure. On failure, if err_out != nullptr, a human-readable reason is
// written into *err_out. The caller owns the returned fd (must ::close()).
int openRxSocket(const UdpRxOptions& opts, std::string* err_out = nullptr) noexcept;

} // namespace liveqx::gateway::net
