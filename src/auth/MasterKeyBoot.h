#pragma once
//
// How the service obtains its master key at start.
//
// Checks the configured store (file / creds / env), loads the key, refuses
// to start when the key was delivered differently than configured, warns when
// the key file sits next to the data, and — if the key file is
// passphrase-wrapped — runs the sealed start: only a small HTTPS page on the
// API port is served until somebody types the passphrase (see SealedServer.h).
//
// Everything that touches secrets starts after this returns Ready.

#include <nlohmann/json_fwd.hpp>

#include <functional>

namespace liveqx::paths { class Paths; }
namespace liveqx::sysd { class SystemdNotifier; }

namespace liveqx::auth {

class MasterKey;

enum class BootResult {
    Ready,     // key loaded: continue with the normal start
    Failed,    // the service must not start (reason already logged)
    Aborted,   // asked to stop while waiting for the passphrase
};

// `should_abort` is polled while waiting for the passphrase (e.g. SIGTERM).
// `notifier` may be null; if set, the sealed wait reports READY=1 with a
// "sealed" status and keeps the systemd watchdog fed.
BootResult bootMasterKey(MasterKey& key,
                         const liveqx::paths::Paths& paths,
                         const nlohmann::json& cfg,
                         const std::function<bool()>& should_abort,
                         liveqx::sysd::SystemdNotifier* notifier = nullptr);

}  // namespace liveqx::auth
