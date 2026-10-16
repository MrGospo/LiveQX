// Master key at service start — see auth/MasterKeyBoot.h.

#include "auth/MasterKeyBoot.h"

#include "api/TlsBootstrap.h"
#include "auth/MasterKey.h"
#include "auth/SealedServer.h"
#include "utils/Log.h"
#include "utils/Paths.h"
#include "utils/SystemdNotifier.h"

#include <nlohmann/json.hpp>

namespace liveqx::auth {

BootResult bootMasterKey(MasterKey& key,
                         const liveqx::paths::Paths& paths,
                         const nlohmann::json& cfg,
                         const std::function<bool()>& should_abort,
                         liveqx::sysd::SystemdNotifier* notifier) {
    const std::string key_path = paths.masterKey().string();
    const std::string store    = paths.masterKeyStore();
    if (store != "file" && store != "creds" && store != "env") {
        LOG_ERROR("master_key_store \"{}\" is not one of file, creds, env", store);
        return BootResult::Failed;
    }

    key.setStateDir(paths.stateDir().string());
    // Keys delivered from outside must never be silently replaced by a
    // freshly generated file: existing data would become unreadable.
    key.setAutoGenerate(store == "file");

    if (!key.load()) {
        if (!key.sealed()) {
            LOG_ERROR("Cannot load master key from {}", key_path);
            return BootResult::Failed;
        }

        // Passphrase-wrapped key: serve the unlock page on the API port and
        // wait. Nothing that needs a secret has started yet.
        const std::string bind = cfg.value("api_bind", std::string{"0.0.0.0"});
        bool tls_failed = false;
        const TlsBindings tls = resolveTlsBindings(cfg, paths, bind, tls_failed);
        if (tls_failed) return BootResult::Failed;

        SealedServer::Options opts;
        opts.bind      = bind;
        opts.port      = cfg.value("api_port", 8080);
        opts.cert_path = tls.cert_path;
        opts.key_path  = tls.key_path;
        // The second way in, for `liveqx --unseal`. Private (0600) and inside
        // the state directory, which only the service user and root can enter.
        opts.unseal_socket = paths.stateDir() / "unseal.sock";
        SealedServer server(key, opts);
        std::string err;
        if (!server.start(err)) {
            LOG_ERROR("sealed: {}", err);
            return BootResult::Failed;
        }

        if (notifier) {
            notifier->ready("sealed: waiting for the master-key passphrase");
            notifier->startWatchdogLoop(
                [] { return std::string{"sealed: waiting for the master-key passphrase"}; });
        }
        const bool unlocked = server.waitUnlocked(should_abort);
        if (notifier) notifier->stopWatchdogLoop();
        server.stop();   // frees the port for the real service

        if (!unlocked) {
            LOG_INFO("sealed: stop requested before the key was unlocked");
            return BootResult::Aborted;
        }
        LOG_INFO("sealed: master key unlocked, starting");
    }

    const std::string prot = key.getInfo().protection;
    if ((store == "creds" && prot != "systemd_credential") ||
        (store == "env" && prot != "env")) {
        LOG_ERROR("master_key_store is \"{}\" but the key was loaded as \"{}\"; "
                  "check the service unit", store, prot);
        return BootResult::Failed;
    }
    if (prot == "file_in_state") {
        LOG_WARN("Master key is stored inside the state directory ({}): any "
                 "copy of the state (backup, VM snapshot, disk image) carries "
                 "the key together with the data it protects. Keep the key "
                 "outside the state directory, deliver it through "
                 "systemd-creds, or protect it with a passphrase.", key_path);
    }
    return BootResult::Ready;
}

}  // namespace liveqx::auth
