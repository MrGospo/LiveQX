#pragma once
//
// The in-process list of revoked player certificates — layer 1 of the
// three-layer revocation (A9). A certificate added here is refused by the
// device port on the very next request; nothing has to be written to disk or
// sent to the broker first.
//
// The database stays the source of truth: the server rebuilds the list from
// it at startup (rebuildFrom), so a revocation survives a restart, and the
// revoke hook of PlayersService only keeps the list current in between.
//
// Serial numbers are compared in a normal form (upper case, no leading
// zeros), so the CA's notation and OpenSSL's can differ without effect.

#include "players/PlayersDb.h"

#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace liveqx::players {

class RevocationList {
public:
    bool        isRevoked(std::string_view serial_hex) const;
    void        add(const std::vector<CertRow>& certs);
    void        replaceAll(const std::vector<CertRow>& certs);
    // Rebuilds from every revoked, not yet expired certificate in the
    // database. Returns how many entries the list holds afterwards.
    std::size_t rebuildFrom(PlayersDb& db, std::int64_t now = 0);
    std::size_t size() const;

    static std::string normalise(std::string_view serial_hex);

private:
    mutable std::shared_mutex       mu_;
    std::unordered_set<std::string> serials_;
};

}  // namespace liveqx::players
