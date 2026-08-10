#pragma once
#include <nlohmann/json.hpp>

namespace liveqx::utils {

// Enumerate IPv4 interfaces visible to this process via getifaddrs(3).
// Returns a JSON array of objects:
//   { "name": "eth0",
//     "addresses": ["10.0.0.5", "10.0.0.6"],
//     "up": true,
//     "loopback": false,
//     "multicast": true }
//
// Multiple AF_INET addresses on the same NIC are merged into one entry.
// Non-AF_INET (IPv6/AF_PACKET) and pseudo-interfaces with no address are
// skipped. Used by REST GET /api/system/interfaces so operators can pick
// a NIC for gateway input/output binding without ssh-ing onto the box.
nlohmann::json enumerateInterfacesJson();

}  // namespace liveqx::utils
