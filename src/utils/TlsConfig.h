#pragma once
//
// fix38 — TLS configuration schema.
//
// Lives separately from utils/Tls.h because the schema parser pulls in
// nlohmann::json, while the X509 primitives (utils/Tls.h) deliberately
// keep the header lightweight. main.cpp parses TlsConfig once at boot
// and passes the resolved struct to ControlApi + the auto-bootstrap
// path. ControlApi uses only the cert/key paths and the bind+port
// hints; it does not re-parse JSON.
//
// Modes (mirrors docs/TLS.md):
//   * Auto         — server self-manages CA + server cert in tlsDir/.
//   * Provided     — operator supplies cert_path + key_path; we only load.
//   * BehindProxy  — plain HTTP listener; trust_proxy_cidrs gates
//                    X-Forwarded-* so spoofed client-IP headers from
//                    untrusted upstreams are ignored.
//   * Disabled     — plain HTTP, refused if bind is non-loopback unless
//                    allow_insecure_bind=true. Dev/local-only escape hatch.

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace liveqx::tls {

enum class Mode { Auto, Provided, BehindProxy, Disabled };

const char* modeToString(Mode m) noexcept;
Mode        modeFromString(std::string_view s, bool& ok) noexcept;

struct Config {
    Mode mode{Mode::Auto};

    // `provided` mode inputs. Empty in other modes.
    std::filesystem::path cert_path;
    std::filesystem::path key_path;

    // Optional CA bundle for chain validation on /api/tls/import. Not
    // required at boot — when empty, import accepts any well-formed cert.
    std::filesystem::path ca_path;

    // `auto` mode: extra SAN entries to embed beyond the auto-detected
    // hostname + interface IPs. DNS names, IPv4 strings, or IPv6 strings.
    std::vector<std::string> san_extra;

    // `behind_proxy` mode: only requests originating from these CIDRs
    // are allowed to set X-Forwarded-For / X-Forwarded-Proto. Empty = no
    // proxy headers honoured (safe default).
    std::vector<std::string> trust_proxy_cidrs;

    // `disabled` mode: must be explicitly true to allow non-loopback
    // bind. Otherwise the server refuses to start.
    bool allow_insecure_bind{false};
};

struct ParseResult {
    Config                   config;
    std::vector<std::string> errors;     // any non-empty entry = fatal
    std::vector<std::string> warnings;   // surfaced in boot log
    bool ok() const noexcept { return errors.empty(); }
};

// Parses `cfg["tls"]` if present, else returns Config{} (Auto mode).
// Error messages are user-facing; main.cpp prints them and aborts.
ParseResult parseConfig(const nlohmann::json& cfg);

}  // namespace liveqx::tls
