#pragma once
//
// Resolves the TLS settings of the control-API port from the config.
//
// Used by the normal start (and its TLS hot-reload) and by the sealed start,
// so both serve the same certificate on the same port. In "auto" mode this
// also creates the internal CA and (re)issues the server certificate.

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "api/ControlApi.h"

namespace liveqx::paths { class Paths; }

// On failure `failed` is set to true (and the reason is logged); the returned
// bindings must then not be used. `failed` is never reset to false here.
TlsBindings resolveTlsBindings(const nlohmann::json& cfg,
                               const liveqx::paths::Paths& paths,
                               const std::string& api_bind,
                               bool& failed);

// In "auto" mode the server certificate is the server's own; it is renewed when fewer than this
// many days are left. The same rule runs at start (resolveTlsBindings) and on a timer while the
// server runs (renewServerCertIfDue), so a server that is never restarted does not outlive it.
inline constexpr int kServerCertRenewBeforeDays = 30;

// True when the certificate at `cert_path` is missing, cannot be read, or ends in under
// kServerCertRenewBeforeDays. `now_unix` 0 = the clock.
bool serverCertDue(const std::filesystem::path& cert_path, std::int64_t now_unix = 0);

enum class ServerCertRenewal { NotDue, Renewed, Failed };

// For the timer: when `<tls_dir>/server.crt` is due, issues a new certificate and key in place
// (signed by the internal CA, the SANs detected now plus `san_extra`). Nothing is touched when it is
// not due, or when issuing fails (`error` says why): the old certificate keeps serving and the next
// look tries again. The caller rebuilds the listener after `Renewed`.
ServerCertRenewal renewServerCertIfDue(const std::filesystem::path& tls_dir,
                                       const std::vector<std::string>& san_extra,
                                       std::string& error, std::int64_t now_unix = 0);
