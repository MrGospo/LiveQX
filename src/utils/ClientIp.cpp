#include "utils/ClientIp.h"

#include <arpa/inet.h>

#include <algorithm>
#include <cctype>
#include <cstring>

namespace liveqx::net {
namespace {

constexpr std::size_t kMaxHeaderBytes = 2048;

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

}  // namespace

bool TrustedProxies::toBytes(std::string_view text, Bytes& out) {
    if (text.empty() || text.size() > 64) return false;
    const std::string z(text);                       // inet_pton needs a terminated string
    in_addr  v4{};
    in6_addr v6{};
    if (inet_pton(AF_INET, z.c_str(), &v4) == 1) {
        out.fill(0);
        out[10] = 0xff;
        out[11] = 0xff;
        std::memcpy(out.data() + 12, &v4, 4);
        return true;
    }
    if (inet_pton(AF_INET6, z.c_str(), &v6) == 1) {
        std::memcpy(out.data(), &v6, 16);
        return true;
    }
    return false;
}

bool TrustedProxies::covers(const Net& n, const Bytes& b) {
    int bits = n.prefix;
    for (std::size_t i = 0; i < 16 && bits > 0; ++i, bits -= 8) {
        const std::uint8_t mask = bits >= 8 ? 0xff : static_cast<std::uint8_t>(0xff << (8 - bits));
        if ((n.bytes[i] & mask) != (b[i] & mask)) return false;
    }
    return true;
}

TrustedProxies TrustedProxies::parse(const std::vector<std::string>& cidrs,
                                     std::vector<std::string>* bad) {
    TrustedProxies out;
    for (const auto& raw : cidrs) {
        const std::string_view entry = trim(raw);
        auto reject = [&] { if (bad) bad->push_back(raw); };

        std::string_view addr = entry;
        int prefix = -1;
        if (const auto slash = entry.find('/'); slash != std::string_view::npos) {
            addr = entry.substr(0, slash);
            const std::string_view p = entry.substr(slash + 1);
            if (p.empty() || p.size() > 3 ||
                !std::all_of(p.begin(), p.end(), [](unsigned char c) { return std::isdigit(c); })) {
                reject();
                continue;
            }
            prefix = std::stoi(std::string(p));
        }
        Net n;
        if (!toBytes(addr, n.bytes)) { reject(); continue; }

        // IPv4 text keeps its own prefix range; shift it into the 128-bit space.
        const bool is_v4 = addr.find(':') == std::string_view::npos;
        const int max_prefix = is_v4 ? 32 : 128;
        if (prefix < 0) prefix = max_prefix;
        if (prefix > max_prefix) { reject(); continue; }
        n.prefix = is_v4 ? 96 + prefix : prefix;
        out.nets_.push_back(n);
    }
    return out;
}

bool TrustedProxies::trusts(std::string_view address) const {
    Bytes b{};
    if (!toBytes(trim(address), b)) return false;
    return std::any_of(nets_.begin(), nets_.end(), [&](const Net& n) { return covers(n, b); });
}

std::string TrustedProxies::resolve(std::string_view peer, std::string_view xff) const {
    if (nets_.empty() || !trusts(peer)) return std::string(peer);
    xff = trim(xff);
    if (xff.empty() || xff.size() > kMaxHeaderBytes) return std::string(peer);

    // Walk from the right, past our own proxies, to the first foreign address.
    while (!xff.empty()) {
        const auto comma = xff.rfind(',');
        const std::string_view entry =
            trim(comma == std::string_view::npos ? xff : xff.substr(comma + 1));
        xff = comma == std::string_view::npos ? std::string_view{} : xff.substr(0, comma);

        Bytes b{};
        if (!toBytes(entry, b)) return std::string(peer);   // unreadable: do not trust the header
        if (trusts(entry)) continue;
        return std::string(entry);
    }
    return std::string(peer);
}

}  // namespace liveqx::net
