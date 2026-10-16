#pragma once
//
// Who is the client when the server sits behind a reverse proxy.
//
// Behind a proxy every connection comes from the proxy's address, so
// per-address limits turn into one shared bucket and the audit trail names the
// proxy. X-Forwarded-For carries the real address, but anyone can send it, so:
//
//   * it is believed only when the TCP peer is one of the configured trusted
//     proxies (tls.trust_proxy_cidrs);
//   * it is read from the RIGHT: the first entry that is not a trusted proxy
//     is the client. A client can forge the left side of the header, never the
//     entry its own proxy appended;
//   * anything unreadable means "do not trust the header" and the peer is
//     used — never an invented address.

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace liveqx::net {

class TrustedProxies {
public:
    // Parses "a.b.c.d/n", "x::y/n" or a bare address (host route). Invalid
    // entries are skipped and, if `bad` is given, listed there: trusting
    // fewer proxies is the safe way to fail.
    static TrustedProxies parse(const std::vector<std::string>& cidrs,
                                std::vector<std::string>* bad = nullptr);

    bool empty() const noexcept { return nets_.empty(); }
    bool trusts(std::string_view address) const;

    // The client address for a request from `peer` carrying `x_forwarded_for`
    // (all header values joined with ", "; empty if absent).
    std::string resolve(std::string_view peer, std::string_view x_forwarded_for) const;

private:
    using Bytes = std::array<std::uint8_t, 16>;   // IPv4 is stored as ::ffff:a.b.c.d
    struct Net {
        Bytes bytes{};
        int   prefix{0};                          // in the 128-bit space
    };
    static bool toBytes(std::string_view text, Bytes& out);
    static bool covers(const Net& n, const Bytes& b);

    std::vector<Net> nets_;
};

}  // namespace liveqx::net
