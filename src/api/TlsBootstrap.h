#pragma once
//
// Resolves the TLS settings of the control-API port from the config.
//
// Used by the normal start (and its TLS hot-reload) and by the sealed start,
// so both serve the same certificate on the same port. In "auto" mode this
// also creates the internal CA and (re)issues the server certificate.

#include <nlohmann/json_fwd.hpp>

#include <string>

#include "api/ControlApi.h"

namespace liveqx::paths { class Paths; }

// On failure `failed` is set to true (and the reason is logged); the returned
// bindings must then not be used. `failed` is never reset to false here.
TlsBindings resolveTlsBindings(const nlohmann::json& cfg,
                               const liveqx::paths::Paths& paths,
                               const std::string& api_bind,
                               bool& failed);
