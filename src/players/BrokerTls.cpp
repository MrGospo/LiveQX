#include "players/BrokerTls.h"

#include "utils/Log.h"

#include <arpa/inet.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iterator>
#include <set>
#include <system_error>

namespace liveqx::players {

namespace fs = std::filesystem;

namespace {

std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

std::string trimmed(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    std::size_t at = 0;
    while (at < s.size() && (s[at] == '\n' || s[at] == '\r' || s[at] == ' ')) ++at;
    return s.substr(at);
}

// The PEM certificates in `pem`, one string per block, each ending in a newline.
std::vector<std::string> pemBlocks(const std::string& pem) {
    static const std::string begin = "-----BEGIN CERTIFICATE-----";
    static const std::string end   = "-----END CERTIFICATE-----";
    std::vector<std::string> out;
    for (std::size_t at = pem.find(begin); at != std::string::npos; at = pem.find(begin, at)) {
        const std::size_t stop = pem.find(end, at);
        if (stop == std::string::npos) break;
        out.push_back(pem.substr(at, stop + end.size() - at) + "\n");
        at = stop + end.size();
    }
    return out;
}

bool isIpAddress(const std::string& s, bool& v6) {
    unsigned char buf[16];
    if (inet_pton(AF_INET, s.c_str(), buf) == 1) { v6 = false; return true; }
    if (inet_pton(AF_INET6, s.c_str(), buf) == 1) { v6 = true; return true; }
    return false;
}

bool listed(const std::vector<std::string>& list, const std::string& what) {
    return std::find(list.begin(), list.end(), what) != list.end();
}

}  // namespace

BrokerTls::BrokerTls(BrokerTlsOptions opts) : opts_(std::move(opts)) {
    opts_.validity_days     = std::max(2, opts_.validity_days);
    opts_.renew_before_days = std::clamp(opts_.renew_before_days, 1, opts_.validity_days - 1);
    opts_.backdate_s        = std::max(0, opts_.backdate_s);
}

std::string BrokerTls::hostOfBrokerUrl(const std::string& url) {
    std::string rest = url;
    if (const auto scheme = rest.find("://"); scheme != std::string::npos) rest = rest.substr(scheme + 3);
    if (const auto slash = rest.find_first_of("/?#"); slash != std::string::npos) rest = rest.substr(0, slash);
    if (const auto at = rest.rfind('@'); at != std::string::npos) rest = rest.substr(at + 1);
    if (!rest.empty() && rest.front() == '[') {                       // [v6]:port
        const auto close = rest.find(']');
        return close == std::string::npos ? std::string() : rest.substr(1, close - 1);
    }
    if (const auto colon = rest.find(':'); colon != std::string::npos) rest = rest.substr(0, colon);
    return rest;
}

BrokerTlsState BrokerTls::state() const {
    std::lock_guard<std::mutex> lk(mu_);
    return state_;
}

std::string BrokerTls::ensure(bool& changed) {
    changed = false;
    std::string problem = ensureCafile(changed);
    const std::string cert_problem = ensureCertificate(changed);
    if (problem.empty()) problem = cert_problem;
    {
        std::lock_guard<std::mutex> lk(mu_);
        state_.error = problem;
    }
    return problem;
}

std::string BrokerTls::ensureCafile(bool& changed) {
    const std::vector<int> gens = opts_.generations ? opts_.generations() : std::vector<int>{};
    if (gens.empty()) return "no_device_ca";

    // Every generation: certificates issued by an older one stay valid until they run out.
    std::string all;
    std::set<std::string> seen;
    for (int g : gens) {
        std::string err;
        auto ca = opts_.open_generation ? opts_.open_generation(g, err) : nullptr;
        if (!ca) return "generation_" + std::to_string(g) + ": " + (err.empty() ? "cannot be opened" : err);
        for (const std::string& pem : {ca->rootPem(), ca->certPem(), ca->chainPem()})
            for (const std::string& block : pemBlocks(pem))
                if (seen.insert(block).second) all += block;
    }
    if (all.empty()) return "no_ca_certificates";

    const fs::path file = opts_.dir / "cafile.pem";
    if (slurp(file) == all) return {};
    std::error_code ec;
    fs::create_directories(opts_.dir, ec);
    if (const std::string err = tls::writePem(file, all, /*is_secret=*/false); !err.empty())
        return "cannot_write_cafile: " + err;
    changed = true;
    return {};
}

std::string BrokerTls::ensureCertificate(bool& changed) {
    const fs::path cert = opts_.dir / "broker.crt";
    const fs::path key  = opts_.dir / "broker.key";
    const std::int64_t now = opts_.now ? opts_.now() : static_cast<std::int64_t>(std::time(nullptr));
    const std::string host = opts_.host ? opts_.host() : std::string();

    // Do we hold the key of the root the players are told to trust?
    const fs::path ca_crt = opts_.server_ca_dir / "ca.crt";
    const fs::path ca_key = opts_.server_ca_dir / "ca.key";
    std::error_code ec;
    const bool holds_ca = !opts_.server_ca_dir.empty() && fs::exists(ca_crt, ec) && fs::exists(ca_key, ec) &&
                          trimmed(slurp(ca_crt)) == trimmed(opts_.trust_root_pem ? opts_.trust_root_pem() : std::string());

    auto set_state = [&](const std::string& what, std::int64_t not_after) {
        std::lock_guard<std::mutex> lk(mu_);
        state_.certificate = what;
        state_.not_after   = not_after;
    };

    if (!holds_ca) {
        // The operator's own certificate: only look at it.
        if (!fs::exists(cert, ec) || !fs::exists(key, ec)) {
            set_state("missing", 0);
            return "broker_certificate_missing";
        }
        const auto ok = tls::verifyKeyPair(cert, key);
        const auto info = tls::readCertInfo(cert);
        set_state(ok.ok ? "provided" : "missing", info.not_after_unix);
        if (!ok.ok) return "broker_certificate_invalid: " + ok.error;
        return {};
    }

    // Managed: what the certificate must cover.
    tls::SanList sans = tls::autoDetectSans(opts_.san_extra);
    bool v6 = false;
    if (!host.empty()) {
        if (isIpAddress(host, v6)) (v6 ? sans.ip_v6 : sans.ip_v4).push_back(host);
        else sans.dns_names.push_back(host);
    }

    bool reissue = !fs::exists(cert, ec) || !fs::exists(key, ec);
    tls::CertInfo info;
    if (!reissue) {
        info = tls::readCertInfo(cert);
        if (info.subject.empty() || !tls::verifyKeyPair(cert, key).ok || !tls::verifyChain(cert, ca_crt).ok) {
            reissue = true;                                            // unreadable, not its key's, not from this root
        } else if ((info.not_after_unix - now) < std::int64_t{opts_.renew_before_days} * 86400) {
            reissue = true;                                            // about to run out
        } else if (!host.empty()) {
            const bool covered = isIpAddress(host, v6) ? listed(info.san_ip, host) : listed(info.san_dns, host);
            if (!covered) reissue = true;                              // the host in the settings changed
        }
    }

    if (reissue) {
        std::error_code mk;
        fs::create_directories(opts_.dir, mk);
        const auto issued = tls::issueServerCertTo(opts_.server_ca_dir, cert, key, "liveqx-broker", sans,
                                                   opts_.validity_days, opts_.backdate_s);
        if (!issued.ok()) {
            set_state("missing", 0);
            return "cannot_issue_broker_certificate: " + issued.error;
        }
        changed = true;
        info = tls::readCertInfo(cert);
        LOG_INFO("Broker TLS: issued the broker certificate{}{} (valid until day {})",
                 host.empty() ? "" : " for ", host, info.not_after_unix / 86400);
    }
    set_state("managed", info.not_after_unix);
    return {};
}

}  // namespace liveqx::players
