#include "players/RevocationList.h"

#include <cctype>
#include <mutex>

namespace liveqx::players {

std::string RevocationList::normalise(std::string_view serial_hex) {
    std::string out;
    out.reserve(serial_hex.size());
    for (char c : serial_hex) out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    const auto first = out.find_first_not_of('0');
    return first == std::string::npos ? std::string{} : out.substr(first);
}

bool RevocationList::isRevoked(std::string_view serial_hex) const {
    const std::string key = normalise(serial_hex);
    if (key.empty()) return false;
    std::shared_lock lk(mu_);
    return serials_.count(key) != 0;
}

void RevocationList::add(const std::vector<CertRow>& certs) {
    std::unique_lock lk(mu_);
    for (const auto& c : certs)
        if (auto k = normalise(c.serial_hex); !k.empty()) serials_.insert(std::move(k));
}

void RevocationList::replaceAll(const std::vector<CertRow>& certs) {
    std::unordered_set<std::string> fresh;
    for (const auto& c : certs)
        if (auto k = normalise(c.serial_hex); !k.empty()) fresh.insert(std::move(k));
    std::unique_lock lk(mu_);
    serials_ = std::move(fresh);
}

std::size_t RevocationList::rebuildFrom(PlayersDb& db, std::int64_t now) {
    replaceAll(db.revokedCerts(now));
    return size();
}

std::size_t RevocationList::size() const {
    std::shared_lock lk(mu_);
    return serials_.size();
}

}  // namespace liveqx::players
