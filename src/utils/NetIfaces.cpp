#include "utils/NetIfaces.h"

#include <map>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>

namespace liveqx::utils {

namespace {

struct IfaceAccum {
    std::vector<std::string> addrs;
    bool up        = false;
    bool loopback  = false;
    bool multicast = false;
};

}  // namespace

nlohmann::json enumerateInterfacesJson() {
    auto out = nlohmann::json::array();

    ifaddrs* head = nullptr;
    if (::getifaddrs(&head) != 0) return out;

    std::map<std::string, IfaceAccum> by_name;
    for (ifaddrs* p = head; p; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
        const std::string name = p->ifa_name ? p->ifa_name : "";
        if (name.empty()) continue;

        char buf[INET_ADDRSTRLEN] = {};
        const auto* sin = reinterpret_cast<const sockaddr_in*>(p->ifa_addr);
        if (!::inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf))) continue;

        auto& slot = by_name[name];
        slot.addrs.emplace_back(buf);
        slot.up        = (p->ifa_flags & IFF_UP) != 0;
        slot.loopback  = (p->ifa_flags & IFF_LOOPBACK) != 0;
        slot.multicast = (p->ifa_flags & IFF_MULTICAST) != 0;
    }
    ::freeifaddrs(head);

    for (auto& [name, info] : by_name) {
        out.push_back(nlohmann::json{
            {"name",      name},
            {"addresses", info.addrs},
            {"up",        info.up},
            {"loopback",  info.loopback},
            {"multicast", info.multicast},
        });
    }
    return out;
}

}  // namespace liveqx::utils
