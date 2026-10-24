#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "api/ChannelManager.h"
#include "api/ControlApi.h"
#include "api/MetricsCollector.h"
#include "audit/AuditDb.h"
#include "audit/AuditLogger.h"
#include "audit/AuditRateLimiter.h"
#include "audit/AuditRetentionWorker.h"
#include "auth/AuthCli.h"
#include "auth/AuthDb.h"
#include "auth/AuthService.h"
#include "auth/JwtIssuer.h"
#include "auth/LdapClient.h"
#include "auth/LdapConfigRepo.h"
#include "auth/MasterKey.h"
#include "auth/MasterKeyReset.h"
#include "auth/RbacMiddleware.h"
#include "auth/SmtpConfigRepo.h"
#include "auth/SntpClient.h"
#include "auth/TimeConfig.h"
#include "auth/TimeConfigRepo.h"
#include "auth/TimeSource.h"
#include "core/ChannelInstance.h"
#include "core/Watchdog.h"
#include "events/EventBus.h"
#include "gateway/Gateway.h"
#include "gateway/GatewayManager.h"
#include "gateway/TranscodeGateway.h"
#include "gateway/transcode/FfmpegTranscodePipeline.h"
#include "gateway/transcode/ITranscodePipeline.h"
#include "logging/SqlitePlaybackSink.h"
#include "metrics/ProcessMetrics.h"
#include "metrics/ProfileSampler.h"
#include "mounts/MountManager.h"
#include "mounts/MountdRpcClient.h"
#include "mounts/MountsDb.h"
#include "mqtt/BrokerLink.h"
#include "mqtt/BrokerReload.h"
#ifdef LIVEQX_HAVE_MOSQUITTO
#include "mqtt/MosquittoClient.h"
#endif
#include "players/AuditScope.h"
#include "players/BrokerSettings.h"
#include "players/CertSummary.h"
#include "players/CertWatch.h"
#include "players/CommandService.h"
#include "players/BrokerTls.h"
#include "players/CrlPublisher.h"
#include "players/DeviceApi.h"
#include "players/EnrollService.h"
#include "players/PresenceService.h"
#include "players/RenewService.h"
#include "players/PlayersDb.h"
#include "players/RevocationList.h"
#include "utils/DeviceCa.h"
#include "players/PlayersService.h"
#include "plugins/PluginManager.h"
#include "preview/PreviewManager.h"
#include "stress/StressConfig.h"
#include "stress/StressService.h"
#include "utils/LegacyMigration.h"
#include "utils/Log.h"
#include "utils/Paths.h"
#include "utils/RuntimeLock.h"
#include "utils/SimdRuntime.h"
#include "utils/SystemdNotifier.h"
#include "utils/Tls.h"
#include "utils/TlsConfig.h"
#include "api/TlsBootstrap.h"
#include "auth/KeyProtectCli.h"
#include "auth/UnsealCli.h"
#include "utils/CredentialStore.h"
#include "utils/DeviceCaCli.h"
#include "auth/MasterKeyBoot.h"

#include <sodium.h>

using json = nlohmann::json;

static std::atomic<bool> g_running{true};
static void on_signal(int) { g_running.store(false); }

namespace {

// 32-byte CSPRNG token in lowercase hex (64 chars). libsodium is already
// linked for the master-key code path; reuse rather than pulling OpenSSL
// just for this one call site.
std::string generateMetricsTokenHex() {
    std::array<unsigned char, 32> buf{};
    randombytes_buf(buf.data(), buf.size());
    std::ostringstream oss;
    for (auto b : buf)
        oss << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<int>(b);
    return oss.str();
}

// Atomic write of the metrics token state file: tmp → chmod 0600 → rename.
// Empty `token` is valid (means "operator chose to disable auth"); the file
// is written with zero bytes so the next start can distinguish "operator
// chose open mode" (file exists, empty) from "fresh install" (file absent).
bool writeMetricsTokenFile(const std::filesystem::path& target,
                           const std::string&           token) {
    std::error_code ec;
    std::filesystem::create_directories(target.parent_path(), ec);
    if (ec) {
        LOG_ERROR("metrics token: cannot create state dir {}: {}",
                  target.parent_path().string(), ec.message());
        return false;
    }
    const auto tmp = target.string() + ".tmp";
    {
        std::ofstream of(tmp, std::ios::trunc | std::ios::binary);
        if (!of) {
            LOG_ERROR("metrics token: cannot open {} for writing", tmp);
            return false;
        }
        of << token;
        if (!of) {
            LOG_ERROR("metrics token: write to {} failed", tmp);
            std::filesystem::remove(tmp, ec);
            return false;
        }
    }
    std::filesystem::permissions(
        tmp,
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace,
        ec);
    if (ec) {
        LOG_ERROR("metrics token: chmod 0600 on {} failed: {}",
                  tmp, ec.message());
        std::filesystem::remove(tmp, ec);
        return false;
    }
    std::filesystem::rename(tmp, target, ec);
    if (ec) {
        LOG_ERROR("metrics token: rename {}→{} failed: {}",
                  tmp, target.string(), ec.message());
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

// Trim a single trailing newline. Tokens generated by the engine contain
// no newline, but an operator who edits the file by hand may add one.
std::string readMetricsTokenFile(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream oss;
    oss << f.rdbuf();
    std::string s = oss.str();
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
        s.pop_back();
    return s;
}

}  // namespace

int main(int argc, char* argv[]) {
    // fix16: process boot timestamp captured at the very start so it survives
    // across config-load and bootstrap, and is always identical to what we
    // expose via process_start_time_seconds.
    const std::int64_t process_start_unix_sec =
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

    // fix35 — single CLI parse populates a Paths::Args bundle. Every
    // path the binary touches at startup goes through Paths::resolve()
    // so an operator can pin any of them via flag, env, config, or
    // FHS-aware default. Pre-fix35 the same fields were spread across
    // ad-hoc cfg.value() calls with hard-coded fallbacks.
    namespace scp = liveqx::paths;
    std::string         config_path = "config/default.json";
    bool                config_path_explicit = false;
    scp::Args           path_args;
    std::string         reset_admin_user;
    bool                do_print_bootstrap   = false;
    bool                do_reset_admin       = false;
    bool                do_rotate_master_key = false;
    // fix35 A3.13 — destructive master-key reset. Two flags: the "do it"
    // verb and a separate `--confirm-reset` that operators must add by
    // hand. Without the second flag we print a warning and exit 1 — the
    // gate is intentional, see MasterKeyReset.h for the semantics.
    bool                do_reset_master_key  = false;
    // Passphrase protection of the master key (auth/KeyProtectCli.h).
    bool                do_protect_master_key   = false;
    bool                do_unprotect_master_key = false;
    bool                do_change_master_pass   = false;
    bool                passphrase_from_stdin   = false;
    // Two-tier device CA ceremony (utils/DeviceCaCli.h).
    bool                do_unseal         = false;
    bool                do_ca_root_init   = false;
    bool                do_ca_request     = false;
    bool                do_ca_root_sign   = false;
    bool                do_ca_import      = false;
    std::string         ca_out_path, ca_root_dir, ca_csr_path, ca_cert_path, ca_root_cert_path;
    int                 ca_days = 1825;
    bool                confirm_reset        = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "--config" || arg == "-c") && i + 1 < argc) {
            config_path = argv[++i];
            config_path_explicit = true;
        } else if (arg == "--state-dir" && i + 1 < argc) {
            path_args.state_dir = argv[++i];
        } else if (arg == "--log-dir" && i + 1 < argc) {
            path_args.log_dir = argv[++i];
        } else if (arg == "--plugin-dir" && i + 1 < argc) {
            path_args.plugin_dir = argv[++i];
        } else if (arg == "--ui-dir" && i + 1 < argc) {
            path_args.ui_dir = argv[++i];
        } else if (arg == "--config-dir" && i + 1 < argc) {
            path_args.config_dir = argv[++i];
        } else if (arg == "--channels-dir" && i + 1 < argc) {
            path_args.channels_dir = argv[++i];
        } else if (arg == "--gateways-dir" && i + 1 < argc) {
            path_args.gateways_dir = argv[++i];
        } else if (arg == "--auth-db" && i + 1 < argc) {
            path_args.auth_db = argv[++i];
        } else if (arg == "--master-key-file" && i + 1 < argc) {
            path_args.master_key = argv[++i];
        } else if (arg == "--print-bootstrap-password") {
            do_print_bootstrap = true;
        } else if (arg == "--reset-admin-password" && i + 1 < argc) {
            do_reset_admin   = true;
            reset_admin_user = argv[++i];
        } else if (arg == "--rotate-master-key") {
            do_rotate_master_key = true;
        } else if (arg == "--reset-master-key") {
            do_reset_master_key = true;
        } else if (arg == "--protect-master-key") {
            do_protect_master_key = true;
        } else if (arg == "--unprotect-master-key") {
            do_unprotect_master_key = true;
        } else if (arg == "--change-master-passphrase") {
            do_change_master_pass = true;
        } else if (arg == "--passphrase-stdin") {
            passphrase_from_stdin = true;
        } else if (arg == "--unseal") {
            do_unseal = true;
        } else if (arg == "--device-ca-root-init") {
            do_ca_root_init = true;
        } else if (arg == "--device-ca-request") {
            do_ca_request = true;
        } else if (arg == "--device-ca-root-sign") {
            do_ca_root_sign = true;
        } else if (arg == "--device-ca-import") {
            do_ca_import = true;
        } else if (arg == "--out" && i + 1 < argc) {
            ca_out_path = argv[++i];
        } else if (arg == "--root-dir" && i + 1 < argc) {
            ca_root_dir = argv[++i];
        } else if (arg == "--csr" && i + 1 < argc) {
            ca_csr_path = argv[++i];
        } else if (arg == "--cert" && i + 1 < argc) {
            ca_cert_path = argv[++i];
        } else if (arg == "--root" && i + 1 < argc) {
            ca_root_cert_path = argv[++i];
        } else if (arg == "--days" && i + 1 < argc) {
            ca_days = std::atoi(argv[++i]);
        } else if (arg == "--confirm-reset") {
            confirm_reset = true;
        } else if (arg[0] != '-') {
            config_path = arg;
            config_path_explicit = true;
        }
    }

    // fix22 c10/24, c15/24: recovery-CLI ветка. Запускается до полного
    // startup'а — config может отсутствовать (например, на свежей
    // системе перед install.sh), поэтому мы не падаем, если config_path
    // не открывается; Paths::resolve просто не увидит cfg-полей.
    auto try_load_config = [&](const std::string& path) -> json {
        std::ifstream cf(path);
        if (!cf) return json::object();
        try { return json::parse(cf); }
        catch (...) { return json::object(); }
    };

    if (do_print_bootstrap || do_reset_admin || do_rotate_master_key
        || do_reset_master_key || do_protect_master_key
        || do_unprotect_master_key || do_change_master_pass
        || do_unseal
        || do_ca_root_init || do_ca_request || do_ca_root_sign || do_ca_import) {
        const json recovery_cfg = try_load_config(config_path);
        const auto recovery_paths = scp::Paths::resolve(path_args, recovery_cfg);
        const auto db_path  = recovery_paths.authDb();
        const auto key_path = recovery_paths.masterKey();
        if (do_print_bootstrap)
            return liveqx::auth::printBootstrap(db_path);
        if (do_reset_admin)
            return liveqx::auth::resetAdmin(db_path, reset_admin_user);
        // Passphrase from the terminal (echo off) or, for scripts, one line
        // per prompt from stdin. Never from argv.
        const auto source = passphrase_from_stdin
            ? liveqx::auth::makeStreamPassphraseSource(std::cin)
            : liveqx::auth::makeTerminalPassphraseSource();
        if (do_unseal)
            return liveqx::auth::unsealViaSocket(
                recovery_paths.stateDir() / "unseal.sock", source);
        if (do_ca_root_init || do_ca_request || do_ca_root_sign || do_ca_import) {
            namespace tlsns = liveqx::tls;
            const auto& store = recovery_paths.masterKeyStore();
            if (do_ca_root_init) {
                if (ca_out_path.empty()) {
                    std::cerr << "ERROR: --device-ca-root-init needs --out <directory>\n";
                    return 1;
                }
                return tlsns::deviceCaRootInit(ca_out_path, source);
            }
            if (do_ca_request)
                return tlsns::deviceCaRequest(key_path, recovery_paths.deviceCaDir(), store, source);
            if (do_ca_root_sign) {
                if (ca_root_dir.empty() || ca_csr_path.empty() || ca_out_path.empty()) {
                    std::cerr << "ERROR: --device-ca-root-sign needs --root-dir, --csr and --out\n";
                    return 1;
                }
                return tlsns::deviceCaRootSign(ca_root_dir, ca_csr_path, ca_out_path, ca_days, source);
            }
            if (ca_cert_path.empty() || ca_root_cert_path.empty()) {
                std::cerr << "ERROR: --device-ca-import needs --cert and --root\n";
                return 1;
            }
            return tlsns::deviceCaImport(key_path, recovery_paths.deviceCaDir(), store,
                                         ca_cert_path, ca_root_cert_path, source);
        }
        if (do_protect_master_key || do_unprotect_master_key || do_change_master_pass) {
            const auto& store = recovery_paths.masterKeyStore();
            if (do_protect_master_key)
                return liveqx::auth::protectMasterKey(
                    key_path, recovery_paths.stateDir(), store, source);
            if (do_unprotect_master_key)
                return liveqx::auth::unprotectMasterKey(
                    key_path, recovery_paths.stateDir(), store, source);
            return liveqx::auth::changeMasterPassphrase(
                key_path, recovery_paths.stateDir(), store, source);
        }
        if (do_rotate_master_key && recovery_paths.masterKeyStore() == "creds") {
            auto creds = liveqx::util::makeSystemdCredentialStore();
            return liveqx::auth::rotateMasterKeyCredential(
                db_path, recovery_paths.masterKeyCredentialPath(),
                recovery_paths.masterKeyCredentialName(), *creds,
                recovery_paths.mountsDb(), recovery_paths.deviceCaDir(),
                recovery_paths.stateDir());
        }
        if (do_rotate_master_key)
            return liveqx::auth::rotateMasterKey(db_path, key_path,
                                                 recovery_paths.mountsDb(),
                                                 recovery_paths.deviceCaDir(),
                                                 recovery_paths.stateDir(),
                                                 recovery_paths.masterKeyStore(),
                                                 source);
        if (do_reset_master_key) {
            // fix35 A3.13 — destructive master-key reset. The CLI is
            // responsible for printing the warning when the operator
            // forgot --confirm-reset; the helper just returns
            // NoConfirmation in that case.
            if (!confirm_reset) {
                std::cerr
                    << "WARNING: --reset-master-key destroys the current master key.\n"
                    << "Encrypted secrets (LDAP/SMTP passwords, CIFS passwords of mounts, jwt_secret) will be wiped.\n"
                    << "The device CA (player certificates) is moved aside and re-created; players must be paired again.\n"
                    << "A backup of the current key will be saved to "
                    << key_path.string() << ".<unix_timestamp>.bak.\n"
                    << "The LiveQX service must be stopped before running this.\n"
                    << "User accounts and Argon2 password hashes are preserved.\n"
                    << "Re-run with --confirm-reset to proceed.\n";
                return 1;
            }
            const auto report =
                liveqx::auth::resetMasterKey(recovery_paths, true);
            using R = liveqx::auth::ResetResult;
            switch (report.result) {
                case R::Success: {
                    std::cout << "master_key_reset: true\n";
                    std::cout << "key_file: " << key_path.string() << "\n";
                    std::cout << "backup_file: "
                              << (report.backup_path.empty()
                                      ? std::string{"(none)"}
                                      : report.backup_path.string())
                              << "\n";
                    std::cout << "wiped: ";
                    if (report.wiped.empty()) std::cout << "(none)";
                    for (std::size_t i = 0; i < report.wiped.size(); ++i) {
                        if (i) std::cout << ',';
                        std::cout << report.wiped[i];
                    }
                    std::cout << "\n";
                    std::cout.flush();
                    return 0;
                }
                case R::ServiceRunning:
                    std::cerr << "ERROR: " << report.error << "\n";
                    return 2;
                case R::DbOpenFailed:
                    std::cerr << "ERROR: cannot open auth.db: "
                              << report.error << "\n";
                    return 1;
                case R::BackupFailed:
                    std::cerr << "ERROR: backup of current master.key failed: "
                              << report.error << "\n";
                    return 1;
                case R::WipeFailed:
                    std::cerr << "ERROR: cannot wipe encrypted secrets: "
                              << report.error << "\n";
                    return 1;
                case R::GenerateFailed:
                    std::cerr << "ERROR: cannot generate new master.key: "
                              << report.error << "\n";
                    return 1;
                case R::UnsupportedKeyStore:
                    std::cerr << "ERROR: " << report.error << "\n";
                    return 1;
                case R::NoConfirmation:
                    // Should be unreachable — we already gated above.
                    std::cerr << "ERROR: internal: NoConfirmation after gate\n";
                    return 1;
            }
            return 1;
        }
    }

    std::ifstream f(config_path);
    if (!f) {
        // Without --config the operator probably just hasn't deployed
        // /etc/liveqx/config.json yet — boot with an empty cfg
        // so the FHS defaults fully take over. With --config the
        // operator demanded a specific file, which we still treat as a
        // hard error so a typo doesn't silently skip the override.
        if (config_path_explicit) {
            std::cerr << "Cannot open config: " << config_path << "\n";
            return 1;
        }
        std::cerr << "Note: " << config_path << " not found, "
                     "running with defaults\n";
    }

    json cfg = json::object();
    if (f) {
        try { cfg = json::parse(f); }
        catch (const json::exception& e) {
            std::cerr << "Config parse error: " << e.what() << "\n";
            return 1;
        }
    }

    const auto paths = scp::Paths::resolve(path_args, cfg);

    // fix35 (A3.12) — single-instance lock. Held for the lifetime of
    // main(); released automatically on process exit (or kernel reaping
    // the fd). Acquired BEFORE Log::init so a second instance doesn't
    // race for a writable rotation handle on the same audit/app log.
    auto runtime_lock = liveqx::util::RuntimeLock::tryAcquire(
        paths.stateDir() / "liveqx.lock");
    if (!runtime_lock.held()) {
        std::cerr << "Another LiveQX instance is running ("
                  << runtime_lock.error() << ")\n"
                  << "  lockfile: " << runtime_lock.path().string() << "\n";
        return 1;
    }

    Log::init(paths.logDir().string(),
              cfg.value("log_level", std::string("info")));
    LOG_INFO("Paths resolved: {}", paths.summary());
    Log::installFFmpegRedirect();
    LOG_INFO("LiveQX starting, config={}", config_path);

    // fix28 — pick AVX2 vs scalar before any render code touches a frame.
    liveqx::simd::initFromConfig(
        cfg.value("simd", std::string("auto")));

    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);

    // Master key. Everything below that touches a secret (auth, LDAP/SMTP,
    // CIFS passwords, the audit chain, the device CA) needs it, so a
    // passphrase-wrapped key stops the start here, serving only the unlock
    // page on the API port, until the passphrase is entered. Channels are
    // not loaded yet: while sealed, nothing plays.
    liveqx::sysd::SystemdNotifier boot_notifier;
    liveqx::auth::MasterKey master_key(paths.masterKey().string());
    switch (liveqx::auth::bootMasterKey(
        master_key, paths, cfg, [] { return !g_running.load(); }, &boot_notifier)) {
        case liveqx::auth::BootResult::Ready:   break;
        case liveqx::auth::BootResult::Aborted: return 0;
        case liveqx::auth::BootResult::Failed:  return 1;
    }

    Watchdog       watchdog;
    // fix35: channel root resolved through Paths (CLI > ENV > config >
    // default = state_dir/channels). The `channel_root_dir` config key
    // is still recognised for backward compat.
    const std::string channel_root = paths.channelsDir().string();

    // fix7 step 7: one-shot migration of pre-fix7 layouts into per-channel
    // folders. Idempotent: skipped when channels/ is already populated.
    if (!channel_root.empty()) {
        try {
            const auto migrated = LegacyMigration::run(config_path, channel_root);
            if (migrated > 0)
                LOG_INFO("Legacy migration: {} channel(s) moved into {}",
                         migrated, channel_root);
        } catch (const std::exception& e) {
            LOG_ERROR("Legacy migration aborted: {}", e.what());
        }
    }

    ChannelManager manager(&watchdog, channel_root);

    // fix26 c3 — global profiler sampler thread (10ms period).
    // Must outlive manager so unregisterChannel is never called on a dead object.
    liveqx::profiler::ProfileSampler profile_sampler;
    manager.setProfileSampler(&profile_sampler);

    // fix8: process-wide owner of all per-channel proof-of-play SQLite files.
    // Channels with playback_log.sink == "db" share this sink; "file" /
    // "none" channels keep their own sink and ignore this injection.
    const int default_retention_days =
        cfg.value("playback_log_default_retention_days", 90);
    liveqx::logging::SqlitePlaybackSink playback_sink(default_retention_days);
    manager.setSqlitePlaybackSink(&playback_sink);

    // fix7: every channel lives on disk under channel_root. Pre-fix7 configs
    // are auto-migrated above (LegacyMigration::run) so by this point the
    // global config has no `channels: [...]` array — there is nothing else
    // to consult.
    const std::size_t loaded = manager.loadFromRoot();
    if (loaded > 0) {
        LOG_INFO("Bootstrapped {} channel(s) from {}", loaded, channel_root);
        // fix17: skip auto-play for channels that were paused at last save.
        // Persisting the operator's stop intent across restarts is the whole
        // point of state.db — auto-playing them would silently undo it. We
        // collect (id, paused) under the iterate lock and play() outside.
        struct BootEntry { int id; bool paused; };
        std::vector<BootEntry> entries;
        manager.forEachChannel([&entries](const ChannelInstance& ch) {
            BootEntry be;
            be.id     = ch.id();
            be.paused = ch.wasPausedAtLastSave();
            entries.push_back(be);
        });
        for (const auto& e : entries) {
            if (e.paused) {
                LOG_INFO("Channel {} stays stopped (paused=true in state.db)",
                         e.id);
                continue;
            }
            const auto r = manager.play(e.id);
            if (r != ChannelManager::Result::Ok)
                LOG_ERROR("Failed to start channel {}: {}", e.id,
                          channelManagerResultName(r));
        }
    }
    // fix17: every per-channel state.db has been opened by build() and any
    // cursor/paused state has been consumed by the loop above. Flip readiness
    // so /readyz reports state_persistence_loaded=true and Kubernetes shifts
    // traffic to this pod.
    manager.setStatePersistenceLoaded(true);

    if (manager.size() == 0) {
        // fix7: an empty start is a legitimate state — the operator can
        // POST /api/channels later. We just keep the API up.
        LOG_WARN("No channels on disk — waiting for POST /api/channels");
    }

    // fix18: packet-level UDP/multicast forwarders live in their own root.
    // fix35: resolved through Paths (default = state_dir/gateways).
    // fix40 A6 step 9: register the FFmpeg-backed transcode pipeline factory
    // so newly-constructed Transcode-mode gateways automatically attach a
    // decode→encode pipeline. Tests that don't link FFmpeg leave this
    // registration absent and the gateway runs in pass-through.
    liveqx::gateway::TranscodeGateway::registerPipelineFactory(
        [](const liveqx::gateway::TranscodeCfg& tc)
            -> std::unique_ptr<liveqx::gateway::transcode::ITranscodePipeline> {
            return std::make_unique<
                liveqx::gateway::transcode::FfmpegTranscodePipeline>(tc);
        });
    const std::string gateway_root = paths.gatewaysDir().string();
    liveqx::gateway::GatewayManager gateways(gateway_root);
    if (!gateway_root.empty()) {
        const auto gw_loaded = gateways.loadFromRoot();
        if (gw_loaded > 0)
            LOG_INFO("Bootstrapped {} gateway(s) from {}",
                     gw_loaded, gateway_root);
    }

    watchdog.start();

    {
        const int api_port = cfg.value("api_port", 8080);

        // fix16: process-wide /proc reader + Prometheus exporter. ProcessMetrics
        // caches /proc/self snapshots (1s TTL); MetricsCollector is stateless
        // and walks ChannelManager + ProcessMetrics on each scrape.
        ProcessMetrics process_metrics(process_start_unix_sec);
        BuildInfo build_info{
            LIVEQX_VERSION,
            LIVEQX_COMMIT,
            LIVEQX_BUILD_TIME,
            LIVEQX_BUILD_TYPE
        };
        // MetricsCollector is built later, after EventBus + StressService so
        // it can include stress_* families in /api/metrics.

        // fix16: liveness threshold is operator-tunable. 10 s ≈ 250 missed
        // ticks at 25fps; raise on slow-disk benches, lower for k8s probes
        // that want pages within a frame budget.
        //
        // The metrics bearer token now lives in a state-dir file owned by
        // the engine, not in /etc/liveqx/config.json. Shipped templates
        // therefore never carry a real secret, and revoke/regenerate via
        // the UI no longer requires write access to /etc/. Three-state
        // semantics of the file:
        //   * absent           → first start, generate + persist
        //   * present, content → that string is the active token
        //   * present, empty   → operator explicitly disabled auth
        LivezOptions livez_opts;
        if (cfg.contains("livez") && cfg["livez"].is_object()) {
            livez_opts.stuck_threshold_seconds =
                cfg["livez"].value("stuck_threshold_seconds", 10.0);
        }
        const auto metrics_token_path = paths.metricsBearerTokenFile();
        if (std::filesystem::exists(metrics_token_path)) {
            livez_opts.metrics_bearer_token =
                readMetricsTokenFile(metrics_token_path);
        } else {
            // One-shot migration: existing installs may have the legacy
            // `livez.metrics_bearer_token` in config.json. Honour it on
            // first boot of the new binary so Prometheus scrapers keep
            // working, then write it to the new location and let the
            // config field become inert.
            std::string seed;
            if (cfg.contains("livez") && cfg["livez"].is_object()
                && cfg["livez"].contains("metrics_bearer_token")
                && cfg["livez"]["metrics_bearer_token"].is_string()) {
                seed = cfg["livez"]["metrics_bearer_token"].get<std::string>();
            }
            if (seed.empty()) {
                seed = generateMetricsTokenHex();
                LOG_INFO("metrics token: generated fresh ({}…), persisting to {}",
                         seed.substr(0, 8), metrics_token_path.string());
            } else {
                LOG_INFO("metrics token: migrated from config.json to {}",
                         metrics_token_path.string());
            }
            if (!writeMetricsTokenFile(metrics_token_path, seed)) {
                LOG_ERROR("metrics token: bootstrap write failed; /api/metrics will be "
                          "unauthenticated until the next successful write");
            } else {
                livez_opts.metrics_bearer_token = seed;
            }
        }
        livez_opts.on_metrics_token_changed =
            [metrics_token_path](const std::string& new_token) -> bool {
                if (!writeMetricsTokenFile(metrics_token_path, new_token))
                    return false;
                if (new_token.empty())
                    LOG_INFO("metrics token: cleared (auth disabled)");
                else
                    LOG_INFO("metrics token: updated ({}…)",
                             new_token.substr(0, 8));
                return true;
            };

        // ── fix22 c24/24 — Auth + RBAC wiring ──────────────────────────
        //
        // Этот блок собирает все auth-объекты в стек: AuthDb → MasterKey →
        // JwtIssuer (с persisted secret) → AuthService → LdapConfigRepo /
        // SmtpConfigRepo / RbacMiddleware. ControlApi получает их сырыми
        // указателями и регистрирует RBAC-правила на старте. После
        // bootstrap'а initial admin'а manager.setAuthLoaded(true) флипает
        // /readyz в зелёный.
        namespace sa = liveqx::auth;
        const std::string auth_db_path    = paths.authDb().string();

        sa::AuthDb auth_db(auth_db_path);
        if (!auth_db.open()) {
            LOG_ERROR("Cannot open auth.db at {}", auth_db_path);
            return 1;
        }

        // JWT signing secret: либо берём расшифрованный из БД, либо
        // generates 32 random hex bytes (64-char string ≥256 bit) и
        // запоминаем в jwt_secret под master key'ом. Rotation — через
        // recovery-CLI --rotate-master-key.
        std::string jwt_secret;
        if (auto blob = auth_db.readJwtSecret(); blob.has_value()) {
            auto dec = master_key.decrypt(*blob);
            if (!dec) {
                LOG_ERROR("Cannot decrypt jwt_secret with master key — wrong key?");
                return 1;
            }
            jwt_secret = std::move(*dec);
        } else {
            std::array<std::uint8_t, 32> rb{};
            randombytes_buf(rb.data(), rb.size());
            char hex[65];
            for (std::size_t i = 0; i < rb.size(); ++i)
                std::snprintf(hex + 2 * i, 3, "%02x", rb[i]);
            hex[64] = 0;
            jwt_secret.assign(hex, 64);
            const auto now_sec =
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count();
            if (!auth_db.storeJwtSecret(master_key.encrypt(jwt_secret), now_sec)) {
                LOG_ERROR("Cannot persist freshly-generated jwt_secret");
                return 1;
            }
            LOG_INFO("Generated fresh jwt_secret and stored in auth.db");
        }

        sa::JwtIssuer        jwt(jwt_secret);
        sa::LdapConfigRepo   ldap_repo(auth_db, master_key);
        sa::SmtpConfigRepo   smtp_repo(auth_db, master_key);
        sa::TimeConfigRepo   time_repo(auth_db);
        sa::TimeSourceManager time_src;
        auto                 sntp_client = std::make_shared<sa::SntpClient>();
        time_src.setNtpDependencies(
            sntp_client,
            [&time_repo](std::int64_t off_ms, std::int64_t at_unix_sec) {
                time_repo.updateNtpSyncResult(off_ms, at_unix_sec);
            });
        // Loaded TimeConfig | дефолт SystemLocal/UTC. reconfigure() сразу
        // запускает NtpSource'а если source=Ntp.
        {
            sa::TimeConfig boot_cfg;
            if (auto cur = time_repo.load(); cur.has_value())
                boot_cfg = std::move(*cur);
            time_src.reconfigure(boot_cfg);
        }

        // fix33 C — серверная TZ-провайдер для inherit-каналов.
        // ChannelInstance дёргает этот колл-бэк, когда channel_timezone null/absent;
        // ControlApi /api/system/time PUT после сохранения вызывает
        // manager.notifyServerTimezoneChanged(), обходящий все inherit-каналы.
        manager.setServerTimezoneGetter([&time_repo]() -> std::string {
            if (auto cur = time_repo.load(); cur.has_value() &&
                !cur->server_timezone.empty()) {
                return cur->server_timezone;
            }
            return "UTC";
        });

        // Locale-user grants resolver: читает channel_permissions по uid.
        sa::AuthService auth_svc(auth_db, jwt,
            [&auth_db](std::int64_t uid) {
                std::vector<sa::ChannelGrant> out;
                for (const auto& row :
                     auth_db.listChannelPermissionsForUser(uid)) {
                    sa::ChannelGrant g;
                    g.channel_id = row.channel_id;
                    g.permission = row.permission;
                    out.push_back(g);
                }
                return out;
            });

        // LDAP authenticator: lambda поверх LdapConfigRepo + LdapClient.
        // Каждый login() создаёт новый LdapClient (cheap relative to RTT).
        // Отключённый LDAP — Disabled, AuthService уйдёт в Local-only.
        auth_svc.setLdapAuthenticator(
            [&ldap_repo](std::string_view user, std::string_view pass) {
                sa::AuthService::LdapLoginContext lctx;
                auto lcfg = ldap_repo.load();
                if (!lcfg.has_value() || !lcfg->enabled) {
                    lctx.reason = sa::AuthService::LdapReason::Disabled;
                    return lctx;
                }
                sa::LdapClient cli(*lcfg);
                auto r = cli.authenticate(user, pass);
                lctx.email  = r.email;
                lctx.groups = r.groups;
                if (!r.ok) {
                    using Rsn = sa::LdapClient::AuthResult::Reason;
                    using Out = sa::AuthService::LdapReason;
                    switch (r.reason) {
                        case Rsn::ConnectionFailed:   lctx.reason = Out::ConnectionFailed;   break;
                        case Rsn::BindServiceFailed:  lctx.reason = Out::BindServiceFailed;  break;
                        case Rsn::UserNotFound:       lctx.reason = Out::UserNotFound;       break;
                        case Rsn::InvalidCredentials: lctx.reason = Out::InvalidCredentials; break;
                        case Rsn::ConfigError:        lctx.reason = Out::ConfigError;        break;
                        default:                      lctx.reason = Out::Other;              break;
                    }
                    lctx.error = r.error;
                    return lctx;
                }
                auto role = sa::AuthService::pickRoleForGroups(
                    r.groups, lcfg->group_role_map);
                if (!role) {
                    lctx.reason = sa::AuthService::LdapReason::NoRoleMapped;
                    return lctx;
                }
                lctx.ok   = true;
                lctx.role = *role;
                std::vector<sa::AuthService::ChannelAclEntry> acls;
                acls.reserve(lcfg->channel_acl.size());
                for (const auto& a : lcfg->channel_acl) {
                    sa::AuthService::ChannelAclEntry e;
                    e.channel_id  = a.channel_id;
                    e.group_perms = a.group_perms;
                    acls.push_back(std::move(e));
                }
                lctx.channel_grants =
                    sa::AuthService::pickAclGrantsForGroups(r.groups, acls);
                lctx.reason = sa::AuthService::LdapReason::Ok;
                return lctx;
            });

        // LDAP grants resolver на refresh — пересчитывает per-channel
        // grants по cached groups, не требует nового LDAP-bind'а.
        auth_svc.setLdapGrantsResolver(
            [&ldap_repo](const std::vector<std::string>& groups) {
                std::vector<sa::ChannelGrant> empty;
                auto lcfg = ldap_repo.load();
                if (!lcfg.has_value()) return empty;
                std::vector<sa::AuthService::ChannelAclEntry> acls;
                acls.reserve(lcfg->channel_acl.size());
                for (const auto& a : lcfg->channel_acl) {
                    sa::AuthService::ChannelAclEntry e;
                    e.channel_id  = a.channel_id;
                    e.group_perms = a.group_perms;
                    acls.push_back(std::move(e));
                }
                return sa::AuthService::pickAclGrantsForGroups(groups, acls);
            });

        sa::RbacMiddleware rbac(auth_db, jwt);

        // fix35 (A3.16) — wire the initial-admin password file path so
        // bootstrapInitialAdmin can write the plaintext into state/ and
        // changeOwnPassword can auto-delete it on first password change.
        auth_svc.setInitialAdminPasswordFile(paths.initialAdminPasswordFile());

        // Idempotent admin bootstrap. The plaintext is dropped into
        // <state_dir>/initial_admin_password.txt (mode 0600) by
        // bootstrapInitialAdmin when password_file_path is wired. Only
        // when the file write fails do we fall back to logging the
        // password — otherwise the LOG_WARN below is silent on plaintext.
        if (auto br = auth_svc.bootstrapInitialAdmin(); br.created) {
            if (br.password_file_path.empty()) {
                // File-write path unavailable (no setter wired) — single
                // logged copy is the only way the operator gets the
                // password. Pre-fix35 behaviour, retained as a fallback.
                LOG_WARN("Bootstrap admin '{}' created — TEMP_PASSWORD='{}' "
                         "(change at first login)",
                         br.username, br.plaintext_password);
            } else {
                LOG_WARN("Bootstrap admin '{}' created — password is in {}",
                         br.username, br.password_file_path.string());
            }
        }

        manager.setAuthLoaded(true);

        // ── Audit-log stack ─────────────────────────────────────────────
        //
        // Separate DB from auth.db so retention/backup/vacuum on the
        // append-only trail can not stall the login path. HMAC-chain is
        // anchored to the master key so any offline row edit is
        // detectable via AuditDb::verifyChain. AuditLogger owns an async
        // writer thread + emergency JSONL fallback for DB outages; the
        // rate-limiter caps mutations per actor to prevent trail-DoS
        // (bulk-import loops, misbehaving admin scripts).
        namespace sad = liveqx::audit;
        sad::AuditDb audit_db(paths.auditDb(), &master_key);
        if (!audit_db.open())
            LOG_ERROR("Cannot open audit.db at {} — audit will emergency-spill",
                      paths.auditDb().string());
        sad::AuditLogger audit_logger(&audit_db, paths.auditEmergencyFile());
        audit_logger.start();
        sad::RateLimitConfig audit_rl_cfg;
        if (cfg.contains("audit") && cfg["audit"].is_object()) {
            const auto& ac = cfg["audit"];
            audit_rl_cfg.tokens_per_minute =
                ac.value("rate_tokens_per_minute", audit_rl_cfg.tokens_per_minute);
            audit_rl_cfg.burst_capacity =
                ac.value("rate_burst_capacity",    audit_rl_cfg.burst_capacity);
        }
        sad::AuditRateLimiter audit_rate(audit_rl_cfg);
        auth_svc.setAuditLogger(&audit_logger);

        // Retention janitor — hourly sweep, per-category cutoff from
        // defaultRetentionDays(). Runs on its own thread so a heavy
        // purge on a year-old install never blocks AuditLogger's writer.
        std::chrono::minutes audit_retention_interval{60};
        if (cfg.contains("audit") && cfg["audit"].is_object()) {
            const auto& ac = cfg["audit"];
            const int mins = ac.value("retention_interval_minutes", 60);
            if (mins > 0)
                audit_retention_interval = std::chrono::minutes{mins};
        }
        sad::AuditRetentionWorker audit_retention(&audit_db,
                                                  audit_retention_interval);
        audit_retention.start();

        // fix23 — process-wide event bus for SSE fan-out and per-channel
        // WebRTC preview registry. Both are passed down to ControlApi
        // (REST surface) and event_bus is wired into ChannelManager so
        // every existing + future ChannelInstance publishes lifecycle
        // events. PreviewManager defaults can be tuned via the optional
        // "preview" object in the global config.
        liveqx::events::EventBus event_bus;
        manager.setEventBus(&event_bus);
        gateways.setEventBus(&event_bus);
        auth_svc.setEventBus(&event_bus);

        // ── fix41 — CIFS/NFS mounts ─────────────────────────────────────
        //
        // liveqx is the source-of-truth for mount config — UI
        // writes here, reconcile-on-boot reapplies via the privileged
        // liveqx-mountd helper over AF_UNIX. Opening mounts.db
        // unconditionally (table is empty on first boot). reconcileOnBoot
        // is best-effort: if mountd isn't up yet, helper RPCs fail with
        // "rpc_connect_failed" and the periodic sync thread retries.
        namespace mns = liveqx::mounts;
        mns::MountsDb mounts_db(paths.mountsDb());
        if (!mounts_db.open()) {
            LOG_ERROR("Cannot open mounts.db at {}", paths.mountsDb().string());
            return 1;
        }
        mns::MountdRpcClient::Config mountd_rpc_cfg;
        if (cfg.contains("mounts") && cfg["mounts"].is_object()) {
            const auto& mc = cfg["mounts"];
            if (mc.contains("socket_path") && mc["socket_path"].is_string())
                mountd_rpc_cfg.socket_path =
                    mc["socket_path"].get<std::string>();
        }
        auto mountd_rpc =
            std::make_shared<mns::MountdRpcClient>(mountd_rpc_cfg);
        mns::MountManager mounts_mgr(mounts_db, master_key, mountd_rpc);
        LOG_INFO("Mounts: db={} mountd_socket={} ({} row/s)",
                 paths.mountsDb().string(),
                 mountd_rpc_cfg.socket_path.string(),
                 mounts_mgr.size());
        mounts_mgr.reconcileOnBoot();

        // Player integration (phase 2). A store that will not open disables
        // the module (the endpoints answer 503); it must not stop the
        // broadcast.
        liveqx::players::PlayersDb players_db(paths.playersDb());
        // The audit logger outlives the players database: it must not be able to ask
        // it for a place once it is gone (set below, cleared here on the way out).
        struct ScopeResolverGuard {
            sad::AuditLogger& logger;
            ~ScopeResolverGuard() { logger.setScopeResolver({}); }
        } scope_resolver_guard{audit_logger};
        liveqx::players::RevocationList cert_revocations;
        std::unique_ptr<liveqx::players::PlayersService> players_service;
        std::unique_ptr<liveqx::players::EnrollService>  enroll_service;
        std::unique_ptr<liveqx::players::RenewService>   renew_service;
        std::unique_ptr<liveqx::players::PresenceService> presence_service;
        std::unique_ptr<liveqx::players::DeviceApi>      device_api;        // started once TLS is resolved
        // Told when the set of players changes; the broker link (created later,
        // destroyed earlier) is reached through it and only while it exists.
        struct BrokerHook {
            std::mutex mu;
            liveqx::mqtt::BrokerLink* link{nullptr};
            void poke() { std::lock_guard<std::mutex> lk(mu); if (link) link->requestSync(); }
            void rebuild(const std::string& id) { std::lock_guard<std::mutex> lk(mu); if (link) link->requestRebuild(id); }
            void set(liveqx::mqtt::BrokerLink* l) { std::lock_guard<std::mutex> lk(mu); link = l; }
            std::function<void()> on_revoked;                  // asks for a new revocation list; set once, cleared on the way out
            void pokeRevoked() { std::function<void()> f; { std::lock_guard<std::mutex> lk(mu); f = on_revoked; } if (f) f(); }
            void clearRetained(const std::string& topic) {
                std::lock_guard<std::mutex> lk(mu);
                if (link) link->clearRetained(topic);
            }
            liveqx::mqtt::PublishResult publish(const liveqx::mqtt::Message& m) {
                std::lock_guard<std::mutex> lk(mu);
                return link ? link->publish(m) : liveqx::mqtt::PublishResult::NotConnected;
            }
        };
        auto broker_hook = std::make_shared<BrokerHook>();
        // The broker page is told when the link or the revocation file changes, so it
        // does not have to ask every few seconds.
        const auto announce_broker_change = [&event_bus]() {
            event_bus.publish(liveqx::events::EventType::PlayersBrokerStatus, -1, nlohmann::json::object());
        };
        std::unique_ptr<liveqx::mqtt::BrokerLink> broker_link;
        std::unique_ptr<liveqx::players::BrokerTls> broker_tls;          // the broker's CA file and certificate (outlives the publisher)
        std::unique_ptr<liveqx::players::CrlPublisher> crl_publisher;   // the revocation list for the broker
        std::unique_ptr<liveqx::players::CommandService> command_service;
        std::jthread command_sweeper;       // settles commands nobody confirmed in time
        std::unique_ptr<liveqx::players::CertWatch> cert_watch;     // announces screens whose certificate nears its end
        std::jthread players_retention;     // removes old closed records once a day
        struct BrokerHookDetach {
            std::shared_ptr<BrokerHook> h;
            ~BrokerHookDetach() {
                if (!h) return;
                h->set(nullptr);
                std::lock_guard<std::mutex> lk(h->mu);
                h->on_revoked = nullptr;
            }
        } broker_hook_detach{broker_hook};
        std::function<std::shared_ptr<const liveqx::tls::DeviceCa>()> device_ca_provider;
        if (players_db.open()) {
            liveqx::players::PlayersServiceOptions po;
            po.ca_cert_path = paths.tlsDir() / "ca.crt";
            po.events       = &event_bus;
            // Revocation, layer 1: the in-process list is rebuilt from the
            // database (the source of truth) and kept current by this hook.
            // Layers 2 and 3 (broker, CRL file) join the same hook later.
            LOG_INFO("Players: {} revoked certificate(s) loaded",
                     cert_revocations.rebuildFrom(players_db));
            // The broker's certificate follows the host in mqtt_broker_url: look again when settings change.
            po.on_settings_changed = [broker_hook] { broker_hook->pokeRevoked(); };
            po.on_certs_revoked = [&cert_revocations, broker_hook](const std::vector<liveqx::players::CertRow>& c) {
                cert_revocations.add(c);              // the lock in the server: at once
                broker_hook->pokeRevoked();           // the lock at the broker: a new list, soon
            };
            po.on_player_created = [broker_hook](const std::string&) { broker_hook->poke(); };
            po.on_player_gone    = [broker_hook](const std::string&) { broker_hook->poke(); };
            // Bound again: the old device's connection at the broker ends, and the
            // server forgets that device's presence. The presence service is made
            // a little later, hence the check.
            po.on_player_reset   = [broker_hook, &presence_service](const std::string& id) {
                if (presence_service) presence_service->forget(id);
                broker_hook->rebuild(id);
            };
            po.ntp_hint     = [&time_repo]() -> std::vector<std::string> {
                auto cur = time_repo.load();
                if (cur && cur->ntp.enabled) return cur->ntp.servers;
                return {};
            };
            players_service = std::make_unique<liveqx::players::PlayersService>(
                players_db, std::move(po));
            LOG_INFO("Players: db={}", paths.playersDb().string());

            // Where an audit row happened (B13): the object and group of the screen,
            // group or object it is about, asked at the moment the row is written.
            // Things already deleted are placed by the handler that deleted them.
            audit_logger.setScopeResolver([&players_db](const liveqx::audit::AuditEvent& e) {
                return liveqx::players::auditScopeOf(players_db, e);
            });

            // The device CA exists only after the root-signing ceremony
            // (--device-ca-*): the server never creates one by itself, so
            // the key that signs everything is not generated on the server.
            // Until then enroll answers 503 ca_unavailable. The CA is picked
            // up lazily, so finishing the ceremony needs no restart; a
            // failed look is not repeated more often than every 10 s.
            struct DeviceCaHolder {
                std::mutex mu;
                std::shared_ptr<const liveqx::tls::DeviceCa> ca;
                std::int64_t last_try{0};
            };
            auto ca_holder = std::make_shared<DeviceCaHolder>();
            const auto ca_dir = paths.deviceCaDir();
            device_ca_provider = [ca_holder, ca_dir, &master_key]() -> std::shared_ptr<const liveqx::tls::DeviceCa> {
                std::lock_guard<std::mutex> lk(ca_holder->mu);
                if (ca_holder->ca) return ca_holder->ca;
                const auto now = static_cast<std::int64_t>(std::time(nullptr));
                if (now - ca_holder->last_try < 10) return nullptr;
                ca_holder->last_try = now;
                std::string err;
                auto ca = liveqx::tls::DeviceCa::open(ca_dir, master_key, err, /*allow_create=*/false);
                if (!ca) return nullptr;
                LOG_INFO("Device CA opened (generation {}, {})", ca->generation(),
                         ca->twoTier() ? "two-tier" : "single");
                ca_holder->ca = std::shared_ptr<const liveqx::tls::DeviceCa>(std::move(ca));
                return ca_holder->ca;
            };
            auto clock_offset_s = [&time_repo]() -> std::optional<std::int64_t> {
                auto cur = time_repo.load();
                if (cur && cur->ntp.enabled && cur->ntp.last_offset_ms)
                    return *cur->ntp.last_offset_ms / 1000;
                return std::nullopt;
            };
            liveqx::players::EnrollOptions eo;
            eo.ca             = device_ca_provider;
            eo.clock_offset_s = clock_offset_s;
            enroll_service = std::make_unique<liveqx::players::EnrollService>(players_db, std::move(eo));

            liveqx::players::RenewOptions ro;
            ro.ca             = device_ca_provider;
            ro.clock_offset_s = clock_offset_s;
            ro.ca_cert_pem    = [&players_service] { return players_service->caCertPem(); };
            renew_service = std::make_unique<liveqx::players::RenewService>(players_db, std::move(ro));

            // Presence: online while heartbeats keep coming, offline after the
            // configured number of missed intervals plus a grace (A8).
            // players.presence: heartbeat_interval_s (60), missed_intervals (3),
            // grace_s (15), flush_interval_s (5).
            json pcfg = json::object();
            if (cfg.contains("players") && cfg["players"].is_object() &&
                cfg["players"].contains("presence") && cfg["players"]["presence"].is_object())
                pcfg = cfg["players"]["presence"];
            liveqx::players::PresenceOptions pr;
            pr.offline_after_s  = std::clamp<std::int64_t>(pcfg.value("heartbeat_interval_s", 60), 5, 3600) *
                                  std::clamp<std::int64_t>(pcfg.value("missed_intervals", 3), 1, 20) +
                                  std::clamp<std::int64_t>(pcfg.value("grace_s", 15), 0, 600);
            pr.flush_interval_s = std::clamp<std::int64_t>(pcfg.value("flush_interval_s", 5), 0, 300);
            pr.mqtt_offline_grace_s = std::clamp<std::int64_t>(pcfg.value("mqtt_grace_s", 10), 0, 300);
            pr.events           = &event_bus;
            const auto pr_offline_after_s = pr.offline_after_s;
            presence_service = std::make_unique<liveqx::players::PresenceService>(players_db, std::move(pr));
            presence_service->start();
            LOG_INFO("Players: presence — offline after {} s of silence", pr_offline_after_s);

            // Commands to players: sent through the broker link, confirmed by the
            // player's heartbeat, settled by timeout when it never confirms.
            liveqx::players::CommandServiceOptions cso;
            cso.events  = &event_bus;
            cso.publish = [broker_hook](const liveqx::mqtt::Message& m) { return broker_hook->publish(m); };
            // Bulk commands: one audit row for every screen reached, in whatever batch,
            // and one when the job ends. The person who started it is on the job.
            cso.on_bulk_sent = [&audit_logger](const liveqx::players::CommandRow& c, const liveqx::players::JobRow& j) {
                sad::AuditEvent ev;
                ev.category       = sad::Category::Player;
                ev.action         = "player.command_sent";
                ev.actor_user_id  = j.created_by;
                ev.actor_username = j.created_by_name;
                ev.actor_role     = j.created_by_role;
                ev.target_type    = "player";
                ev.target_id      = c.player_id;
                ev.http_method    = "POST";
                ev.http_status    = 200;
                ev.summary        = "player.command_sent via " + j.target_kind;
                ev.details_json   = json{{"type", c.type}, {"command_id", c.id}, {"via", j.target_kind},
                                         {"target", j.target_name}, {"job_id", j.id}}.dump();
                audit_logger.log(std::move(ev));
            };
            cso.on_job_end = [&audit_logger](const liveqx::players::JobRow& j, const liveqx::players::JobSummary& s) {
                sad::AuditEvent ev;
                ev.category       = sad::Category::Player;
                ev.action         = "player.command_job_end";
                ev.actor_user_id  = j.created_by;
                ev.actor_username = j.created_by_name;
                ev.actor_role     = j.created_by_role;
                ev.target_type    = "player_" + j.target_kind;
                ev.target_id      = std::to_string(j.target_id);
                ev.summary        = "player.command_job_end " + j.state;
                ev.details_json   = json{{"job_id", j.id}, {"state", j.state}, {"reason", j.stop_reason},
                                         {"type", j.type}, {"target", j.target_name},
                                         {"total", s.total}, {"executed", s.executed},
                                         {"unconfirmed", s.unconfirmed + s.pending}, {"failed", s.failed},
                                         {"skipped_offline", s.skipped_offline}, {"cancelled", s.cancelled}}.dump();
                audit_logger.log(std::move(ev));
            };
            command_service = std::make_unique<liveqx::players::CommandService>(players_db, *players_service, std::move(cso));
            command_sweeper = std::jthread([svc = command_service.get()](std::stop_token st) {
                // Jobs move on every 2 s (the next batch as soon as the last is settled);
                // commands nobody confirmed are timed out every ~16 s.
                for (int round = 0; !st.stop_requested(); ++round) {
                    svc->tick();
                    if (round % 8 == 0) svc->sweep();
                    for (int i = 0; i < 2 && !st.stop_requested(); ++i)
                        std::this_thread::sleep_for(std::chrono::seconds(1));
                }
            });


            // Retention of the players' own journals: once a day, a minute after start
            // (so a long-neglected database is cleaned soon, but not during boot).
            players_retention = std::jthread([svc = players_service.get()](std::stop_token st) {
                auto nap = [&](int seconds) {
                    for (int i = 0; i < seconds && !st.stop_requested(); ++i)
                        std::this_thread::sleep_for(std::chrono::seconds(1));
                };
                nap(60);
                while (!st.stop_requested()) {
                    const auto r = svc->applyRetention();
                    if (r.total() > 0)
                        LOG_INFO("Players: retention removed {} event(s), {} command(s), {} job(s), {} expired certificate record(s), {} pairing token(s)",
                                 r.events, r.commands, r.jobs, r.certificates, r.tokens);
                    nap(86400);
                }
            });

            // The list of revoked certificates for the broker (players.broker.crl):
            // mode "off" (default) or "local" — write the file and tell a broker on this
            // machine to re-read it. A broker elsewhere cannot be signalled and is
            // protected by its records of players alone (the other locks).
            // Keys: path, pid_file, next_update_days (90), refresh_hours (24).
            {
                json ccfg = json::object();
                if (cfg.contains("players") && cfg["players"].is_object() &&
                    cfg["players"].contains("broker") && cfg["players"]["broker"].is_object() &&
                    cfg["players"]["broker"].contains("crl") && cfg["players"]["broker"]["crl"].is_object())
                    ccfg = cfg["players"]["broker"]["crl"];
                if (ccfg.value("mode", std::string("off")) == "local") {
                    liveqx::players::CrlPublisherOptions co;
                    co.path = ccfg.value("path", paths.brokerCrlFile().string());
                    co.on_change = announce_broker_change;
                    co.next_update_days   = std::clamp(ccfg.value("next_update_days", 90), 2, 365);
                    co.refresh_interval_s = std::clamp<std::int64_t>(ccfg.value("refresh_hours", 24), 1, 24 * 7) * 3600;
                    const auto ca_dir = paths.deviceCaDir();
                    co.generations = [ca_dir] { return liveqx::tls::DeviceCa::generations(ca_dir); };
                    co.open_generation = [ca_dir, &master_key](int g, std::string& err) {
                        return liveqx::tls::DeviceCa::openGeneration(ca_dir, master_key, g, err);
                    };
                    // The rest of what the broker needs to start: the CA file of the devices and its own
                    // certificate, kept beside the list; `ready` appears when all of it is in place.
                    {
                        liveqx::players::BrokerTlsOptions bt;
                        bt.dir = co.path.parent_path();
                        bt.generations = co.generations;
                        bt.open_generation = co.open_generation;
                        bt.server_ca_dir = paths.tlsDir();
                        bt.trust_root_pem = [&players_service] { return players_service->caCertPem(); };
                        bt.host = [&players_service] {
                            return liveqx::players::BrokerTls::hostOfBrokerUrl(players_service->settings().mqtt_broker_url);
                        };
                        // The server itself connects with its name checked, so the address it uses is covered too.
                        if (cfg.contains("tls") && cfg["tls"].is_object() && cfg["tls"].contains("san_extra") &&
                            cfg["tls"]["san_extra"].is_array())
                            for (const auto& v : cfg["tls"]["san_extra"])
                                if (v.is_string()) bt.san_extra.push_back(v.get<std::string>());
                        if (cfg.contains("players") && cfg["players"].is_object() &&
                            cfg["players"].contains("broker") && cfg["players"]["broker"].is_object())
                            if (const auto h = cfg["players"]["broker"].value("host", std::string()); !h.empty())
                                bt.san_extra.push_back(h);
                        co.ready_path = bt.dir / "ready";
                        broker_tls = std::make_unique<liveqx::players::BrokerTls>(std::move(bt));
                        co.prepare = [p = broker_tls.get()](bool& changed) { return p->ensure(changed); };
                    }
                    const std::string pid_file = ccfg.value("pid_file", std::string());
                    if (!pid_file.empty())
                        co.reload = [pid_file] { return liveqx::mqtt::reloadBrokerFromPidFile(pid_file); };
                    else {
                        LOG_WARN("Players: broker CRL is written but players.broker.crl.pid_file is not set — the broker is not told to re-read it");
                        co.reload = [] { return std::string("pid_file_not_configured"); };    // shown on the broker page
                    }
                    const std::string crl_file = co.path.string();
                    crl_publisher = std::make_unique<liveqx::players::CrlPublisher>(players_db, std::move(co));
                    {
                        std::lock_guard<std::mutex> lk(broker_hook->mu);
                        broker_hook->on_revoked = [p = crl_publisher.get()] { p->request(); };
                    }
                    crl_publisher->start();
                    LOG_INFO("Players: broker revocation list is kept in {}", crl_file);
                }
            }

            // The MQTT broker (players.broker): the server's standing connection
            // and the upkeep of the players' records on it. Off unless enabled;
            // a missing broker or CA only keeps it waiting, never stops the server.
            // players.broker: enabled (false), host ("127.0.0.1"), port (8883),
            // verify_hostname (true), reconcile_interval_s (300).
            json bcfg = json::object();
            if (cfg.contains("players") && cfg["players"].is_object() &&
                cfg["players"].contains("broker") && cfg["players"]["broker"].is_object())
                bcfg = cfg["players"]["broker"];
            if (bcfg.value("enabled", false)) {
                liveqx::mqtt::BrokerLinkOptions bo;
                bo.on_change = announce_broker_change;
                // The file gives the defaults; what an administrator set from the interface wins.
                liveqx::mqtt::BrokerConnection base;
                base.host                 = bcfg.value("host", std::string("127.0.0.1"));
                base.port                 = std::clamp(bcfg.value("port", 8883), 1, 65535);
                base.verify_hostname      = bcfg.value("verify_hostname", true);
                base.reconcile_interval_s = std::clamp(bcfg.value("reconcile_interval_s", 300), 10, 86400);
                const auto effective = players_service->brokerConnection(base);
                bo.host                 = effective.host;
                bo.port                 = effective.port;
                bo.verify_hostname      = effective.verify_hostname;
                bo.reconcile_interval_s = effective.reconcile_interval_s;
                const auto broker_ca_dir = paths.deviceCaDir();
                auto identity_mu = std::make_shared<std::mutex>();    // the link thread and a trial connection both ask
                bo.identity = [device_ca_provider, broker_ca_dir, &master_key, identity_mu](
                                  liveqx::mqtt::BrokerIdentityPem& out, std::string& why) {
                    std::lock_guard<std::mutex> lk(*identity_mu);
                    auto ca = device_ca_provider();
                    if (!ca) { why = "the device CA is not set up yet"; return false; }
                    liveqx::tls::BrokerIdentity id;
                    if (!liveqx::tls::DeviceCa::ensureBrokerIdentity(broker_ca_dir, master_key, *ca, id, why))
                        return false;
                    out.cert_chain_pem = id.cert_chain_pem;
                    out.key_pem        = id.key_pem;
                    out.serial         = id.serial_hex;
                    return true;
                };
                // Presence from the broker is the second source next to heartbeats.
                bo.subscriptions = {"liveqx/v1/players/+/presence"};
                bo.on_message = [&presence_service, &players_db, broker_hook](const liveqx::mqtt::Message& m) {
                    if (!presence_service) return;
                    auto p = liveqx::players::PresenceService::parsePresenceMessage(m.topic, m.payload);
                    if (!p) return;
                    // Presence of a player the database does not know: it was removed, and
                    // its retained message outlived it. Wipe it (the broker re-sends retained
                    // messages at every subscription, so older leftovers go too).
                    if (!players_db.findPlayer(p->first)) { broker_hook->clearRetained(m.topic); return; }
                    presence_service->onMqttPresence(p->first, p->second);
                };
                bo.trust_pem = [&players_service] { return players_service->caCertPem(); };
                bo.wanted = [&players_db]() {
                    std::set<std::string> ids;
                    for (const auto& p : players_db.listPlayers({}))
                        if (p.state != liveqx::players::PlayerState::Revoked) ids.insert(p.id);
                    return ids;
                };
#ifdef LIVEQX_HAVE_MOSQUITTO
                bo.make_client = [](const liveqx::mqtt::ClientOptions& co) -> std::unique_ptr<liveqx::mqtt::IMqttClient> {
                    return std::make_unique<liveqx::mqtt::MosquittoClient>(co);
                };
#else
                LOG_WARN("Players: players.broker is enabled but this build has no MQTT library");
#endif
                broker_link = std::make_unique<liveqx::mqtt::BrokerLink>(std::move(bo));
                broker_hook->set(broker_link.get());
                broker_link->start();
                LOG_INFO("Players: broker link — {}:{}", bcfg.value("host", std::string("127.0.0.1")),
                         bcfg.value("port", 8883));
            }
        } else {
            LOG_ERROR("Players: cannot open {} — player integration disabled",
                      paths.playersDb().string());
        }

        // Background pull of helper status. 30s cadence — same as the
        // helper's own watchdog touch; faster polling just thrashes
        // sqlite. jthread destructor request_stop+join's on scope exit.
        std::jthread mounts_status_thread(
            [&mounts_mgr](std::stop_token st) {
                // Small initial delay so reconcileOnBoot's apply settles
                // in mountd before we re-read its status table.
                for (int i = 0; i < 5 && !st.stop_requested(); ++i)
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                while (!st.stop_requested()) {
                    mounts_mgr.syncStatusFromHelper();
                    for (int i = 0; i < 30 && !st.stop_requested(); ++i)
                        std::this_thread::sleep_for(std::chrono::seconds(1));
                }
            });

        // ── fix26 c13/14 — Stress runner + scheduler ────────────────────
        //
        // Bootstrap order: persisted state (stress_state_path) wins over
        // the cfg["stress"] block, which wins over StressConfig defaults.
        // The persist callback writes back to stress_state_path on every
        // successful PUT /api/stress/config so the operator's last edit
        // survives restart.
        // fix35: stress.json sits under state_dir; Paths derives the
        // exact location. Pre-fix35 cfg key `stress_state_path` is
        // honoured at the Paths level via state_dir override.
        const std::filesystem::path stress_state_path =
            cfg.value("stress_state_path", paths.stressJson().string());
        liveqx::stress::StressConfig stress_initial;
        {
            namespace ssns = liveqx::stress;
            std::error_code ec;
            if (std::filesystem::exists(stress_state_path, ec)) {
                std::ifstream sf(stress_state_path);
                if (sf) {
                    try {
                        stress_initial = ssns::StressConfig::fromJson(json::parse(sf));
                        LOG_INFO("Stress: loaded persisted config from {}",
                                 stress_state_path.string());
                    } catch (const std::exception& e) {
                        LOG_WARN("Stress: cannot parse {} ({}); falling back to cfg",
                                 stress_state_path.string(), e.what());
                    }
                }
            } else if (cfg.contains("stress") && cfg["stress"].is_object()) {
                stress_initial = ssns::StressConfig::fromJson(cfg["stress"]);
            }
        }
        liveqx::stress::StressService stress_service(
            manager, stress_initial, /*builder=*/{}, &event_bus);
        stress_service.setPersistCallback(
            [stress_state_path](const liveqx::stress::StressConfig& c) {
                std::error_code ec;
                std::filesystem::create_directories(
                    stress_state_path.parent_path(), ec);
                const auto tmp = stress_state_path.string() + ".tmp";
                {
                    std::ofstream of(tmp, std::ios::trunc);
                    if (!of) {
                        LOG_ERROR("Stress: cannot open {} for write", tmp);
                        return;
                    }
                    of << c.toJson().dump(2);
                }
                std::filesystem::rename(tmp, stress_state_path, ec);
                if (ec)
                    LOG_ERROR("Stress: rename {}→{} failed: {}",
                              tmp, stress_state_path.string(), ec.message());
            });

        // fix19 c3: PluginManager owns the on-disk plugin tree (sibling of
        // channels/). scanAndLoad() is non-fatal — broken plugins surface
        // through /api/plugins as gone, the channel manager keeps booting.
        // Audit hook is wired in commit 5/15 once the AuthService surface
        // is settled.
        //
        // Constructed *before* MetricsCollector so /api/version can include
        // each loaded plugin's attribution text (fix19 c10).
        const std::string plugins_root_dir       = paths.pluginDir().string();
        const std::string plugins_allow_list_dir =
            paths.pluginsAllowListDir().string();
        liveqx::plugins::PluginManager plugins_mgr(
            plugins_root_dir, spdlog::default_logger());
        if (!plugins_allow_list_dir.empty())
            plugins_mgr.setAllowListDir(plugins_allow_list_dir);
        const auto plugins_loaded = plugins_mgr.scanAndLoad();
        LOG_INFO("Plugins: {} loaded from {} (allow-list={})",
                 plugins_loaded, plugins_root_dir,
                 plugins_allow_list_dir.empty()
                   ? "<disabled>" : plugins_allow_list_dir);

        MetricsCollector metrics(manager, process_metrics, build_info,
                                 &gateways, &stress_service, &plugins_mgr,
                                 &mounts_mgr);

        liveqx::preview::PreviewManager::Config preview_cfg;
        if (cfg.contains("preview") && cfg["preview"].is_object()) {
            const auto& pcfg = cfg["preview"];
            preview_cfg.max_clients_per_channel =
                pcfg.value("max_clients_per_channel",
                           preview_cfg.max_clients_per_channel);
            preview_cfg.max_active_channels =
                pcfg.value("max_active_channels",
                           preview_cfg.max_active_channels);
            preview_cfg.idle_shutdown_sec =
                pcfg.value("idle_shutdown_sec",
                           preview_cfg.idle_shutdown_sec);
            if (pcfg.contains("ice_servers") && pcfg["ice_servers"].is_array()) {
                preview_cfg.default_ice_servers.clear();
                for (const auto& s : pcfg["ice_servers"])
                    if (s.is_string())
                        preview_cfg.default_ice_servers.push_back(
                            s.get<std::string>());
            }
        }
        liveqx::preview::PreviewManager preview(preview_cfg);
        // fix34 D2.8 — wire the manager into every existing + future
        // ChannelInstance so the render loop's tap fans frames into preview
        // sessions. Detached on shutdown (see manager.setPreviewManager
        // below) so a late publish doesn't touch a destroyed manager.
        manager.setPreviewManager(&preview);

        // fix38 — TLS bootstrap. Schema is parsed once here; the binary
        // resolves cert/key paths according to mode and fans them into
        // ControlApi via TlsBindings. Failures are fatal: an operator who
        // explicitly opts into provided/auto deserves a hard error rather
        // than a silent fallback to plain HTTP.
        //
        // The bootstrap runs both at boot and on every TLS hot-reload
        // (commit 6/8) so cert files newly written by /api/tls/import or
        // /api/tls/regenerate-server are picked up by the next listener
        // incarnation. tls_failed signals the lambda's caller to abort.
        namespace sct = liveqx::tls;
        const std::string api_bind = cfg.value("api_bind", std::string{"0.0.0.0"});
        bool tls_failed = false;
        auto buildTlsBindings = [&]() -> TlsBindings {
            return resolveTlsBindings(cfg, paths, api_bind, tls_failed);
        };

        TlsBindings tls_bindings = buildTlsBindings();
        if (tls_failed) return 1;

        // fix21 — sd_notify(READY=1) lands here: REST is up, channels are
        // bootstrapped, gateways loaded. Outside systemd this is a no-op.
        liveqx::sysd::SystemdNotifier notifier;
        notifier.ready("started, channels=" + std::to_string(manager.size()));

        // fix21 — periodic STATUS+WATCHDOG keep-alive. Cadence comes from
        // $WATCHDOG_USEC (set by systemd from WatchdogSec=); outside systemd
        // SystemdNotifier::enabled() is false and startWatchdogLoop() is a
        // no-op. The provider walks ChannelManager under shared_lock — cheap.
        notifier.startWatchdogLoop([&manager] {
            return ChannelManager::formatStatusLine(manager.snapshotForStatus());
        });

        // fix38 — TLS hot-reload loop. /api/tls/regenerate-server and
        // /api/tls/import set tls_reload_pending and clear g_running so
        // the wait-for-shutdown loop falls through. We then stop() the
        // listener (joins handler threads cleanly), re-run buildTlsBindings
        // (which reads the just-written cert files from disk), and spin
        // up a fresh ControlApi over the same shared dependencies. Every
        // iteration after the first re-arms g_running because SIGINT/
        // SIGTERM use the same flag for actual shutdown.

        // Device port (A6): mTLS for heartbeat and renew, a separate listener
        // that must not sit behind a reverse proxy. It needs the server's TLS
        // certificate, so it starts only now that the TLS bindings are known.
        // The device CA may not exist yet (no root ceremony): the port then
        // refuses every certificate, and a small watcher adds the trust
        // anchors as soon as the CA appears — no restart.
        std::jthread device_trust_thread;
        if (players_service && renew_service) {
            json dev = json::object();
            if (cfg.contains("players") && cfg["players"].is_object() &&
                cfg["players"].contains("device_api") && cfg["players"]["device_api"].is_object())
                dev = cfg["players"]["device_api"];
            if (dev.value("enabled", true)) {
                if (tls_bindings.cert_path.empty() || tls_bindings.key_path.empty()) {
                    LOG_WARN("Players: device port not started — it needs the server's own TLS "
                             "certificate (tls.mode auto or provided); players cannot connect");
                } else {
                    liveqx::players::DeviceApiOptions dopt;
                    dopt.bind      = dev.value("bind", std::string{"0.0.0.0"});
                    dopt.port      = dev.value("port", 8444);
                    dopt.workers   = dev.value("workers", 16);
                    dopt.cert_path = tls_bindings.cert_path;
                    dopt.key_path  = tls_bindings.key_path;
                    dopt.events    = &event_bus;
                    dopt.renew     = renew_service.get();
                    dopt.presence  = presence_service.get();
                    if (command_service)
                        dopt.on_command_result = [svc = command_service.get()](
                            const std::string& pid, const std::string& cid, const std::string& status,
                            std::optional<std::int64_t> uptime) { svc->onResult(pid, cid, status, uptime); };
                    // Refusals and renewals go into the same HMAC-chained trail
                    // as the admin API; the UI is nudged to refetch over SSE.
                    dopt.audit     = [&audit_logger, &event_bus](const liveqx::players::DeviceAuditEvent& e) {
                        sad::AuditEvent ev;
                        ev.category    = e.access_denial ? sad::Category::Access : sad::Category::Player;
                        ev.action      = e.action;
                        ev.actor_ip    = e.client_ip;
                        ev.target_type = e.player_id.empty() ? "" : "player";
                        ev.target_id   = e.player_id;
                        ev.http_method = e.http_method;
                        ev.http_path   = e.http_path;
                        ev.http_status = e.http_status;
                        ev.summary     = e.action + " -> " + std::to_string(e.http_status);
                        json details = {{"reason", e.reason}, {"port", "device"}};
                        if (e.suppressed > 0) details["suppressed"] = e.suppressed;
                        ev.details_json = details.dump();
                        const std::string cat = sad::categoryName(ev.category);
                        audit_logger.log(std::move(ev));
                        event_bus.publish(liveqx::events::EventType::AuditEvent, -1,
                                          {{"category", cat}, {"action", e.action},
                                           {"target_type", e.player_id.empty() ? "" : "player"},
                                           {"target_id", e.player_id}, {"http_method", e.http_method},
                                           {"http_path", e.http_path}, {"http_status", e.http_status},
                                           {"request_id", ""}, {"actor_username", ""}});
                    };
                    device_api = std::make_unique<liveqx::players::DeviceApi>(
                        players_db, cert_revocations, std::move(dopt));
                    if (!device_api->start()) {
                        LOG_ERROR("Players: device port failed to start — players cannot connect");
                        device_api.reset();
                    }
                }
            }
            if (device_api) {
                auto* api_ptr = device_api.get();
                auto provider = device_ca_provider;
                const auto cert_file = tls_bindings.cert_path;
                const auto key_file  = tls_bindings.key_path;
                device_trust_thread = std::jthread([api_ptr, provider, cert_file, key_file](std::stop_token st) {
                    std::string loaded_for;
                    // Re-issuing the server certificate (UI: regenerate / import)
                    // changes these files; the device port follows by itself.
                    auto stamp = [&] {
                        std::error_code ec;
                        const auto c = std::filesystem::last_write_time(cert_file, ec);
                        const auto k = std::filesystem::last_write_time(key_file, ec);
                        return std::make_pair(c.time_since_epoch().count(), k.time_since_epoch().count());
                    };
                    auto seen = stamp();
                    while (!st.stop_requested()) {
                        if (const auto now_stamp = stamp(); now_stamp != seen) {
                            // Give a writer that updates cert and key one after the other time to finish.
                            std::this_thread::sleep_for(std::chrono::seconds(2));
                            seen = stamp();
                            if (!api_ptr->reloadServerCertificate())
                                LOG_ERROR("Players: device port kept its old certificate");
                        }
                        if (auto ca = provider()) {
                            const std::string pem = ca->certPem() + ca->chainPem() + ca->rootPem();
                            if (pem != loaded_for) {
                                LOG_INFO("Players: device CA trust anchors loaded ({} certificate(s))",
                                         api_ptr->addTrustAnchors(pem));
                                loaded_for = pem;
                            }
                        }
                        for (int i = 0; i < 100 && !st.stop_requested(); ++i)
                            std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    }
                });
            }
        }

        std::atomic<bool> tls_reload_pending{false};

        // The end dates of the server's own certificates: the one that ends takes every screen with it.
        // Read afresh on each ask (a file and a few fields), so the page, the scrape and the watch see what
        // is now. It runs on threads that outlive a reload of the TLS bindings, so it keeps its own copies.
        const auto api_tls_cert = tls_bindings.cert_path;
        auto server_certs = [api_tls_cert, device_ca_provider, &broker_tls, &crl_publisher]() {
            std::vector<liveqx::players::ServerCertEntry> v;
            auto push = [&v](const char* name, std::int64_t not_after, std::string error) {
                v.push_back({name, not_after > 0 && error.empty() ? not_after : 0, std::move(error)});
            };
            if (!api_tls_cert.empty()) {
                const auto ci = liveqx::tls::readCertInfo(api_tls_cert);
                push("api_tls", ci.not_after_unix, ci.subject.empty() ? "unreadable" : "");
            }
            if (device_ca_provider) {
                if (auto ca = device_ca_provider()) {
                    const auto ci = liveqx::tls::readCertInfoFromPem(ca->certPem());
                    push("device_ca", ci.not_after_unix, ci.subject.empty() ? "unreadable" : "");
                    if (ca->twoTier()) {
                        const auto root = liveqx::tls::readCertInfoFromPem(ca->rootPem());
                        push("device_ca_root", root.not_after_unix, root.subject.empty() ? "unreadable" : "");
                    }
                }
            }
            if (broker_tls) {
                const auto t = broker_tls->state();
                push("broker_tls", t.not_after, t.error);
            }
            if (crl_publisher) {
                const auto c = crl_publisher->status();
                if (c.valid_until > 0) push("crl", c.valid_until, "");
            }
            return v;
        };

        // Announces screens whose certificate nears its end, a renewal that fails across the fleet, and the
        // steps of the server's own certificates (including the revocation list's end).
        if (players_service) {
            liveqx::players::CertWatchOptions cw;
            cw.events = &event_bus;
            // A fault of the whole fleet belongs to no place, so only administrators see its row.
            cw.on_renew_alarm = [&audit_logger](bool active, const liveqx::players::ScreenCertSummary& s) {
                sad::AuditEvent ev;
                ev.category       = sad::Category::Player;
                ev.action         = active ? "player.renew_alarm" : "player.renew_alarm_cleared";
                ev.actor_username = "system";
                ev.summary        = active ? "certificate renewal fails across the fleet"
                                           : "certificate renewal no longer fails across the fleet";
                ev.details_json   = json{{"failing", s.failing}, {"online", s.renewing_online},
                                         {"renewing", s.renewing}}.dump();
                audit_logger.log(std::move(ev));
            };
            cw.server_certs = server_certs;
            cw.on_server_cert = [&audit_logger](const liveqx::players::ServerCertEntry& c, liveqx::players::CertStage stage,
                                                liveqx::players::CertStage previous, std::int64_t days_left) {
                sad::AuditEvent ev;
                ev.category       = sad::Category::System;
                ev.action         = "tls.cert_stage";
                ev.actor_username = "system";
                ev.target_type    = "tls";
                ev.target_id      = c.name;
                ev.summary        = "server certificate " + c.name + ": " + liveqx::players::certStageName(stage);
                ev.details_json   = json{{"name", c.name}, {"stage", liveqx::players::certStageName(stage)},
                                         {"previous", liveqx::players::certStageName(previous)},
                                         {"not_after", c.not_after}, {"days_left", days_left}}.dump();
                audit_logger.log(std::move(ev));
            };
            cert_watch = std::make_unique<liveqx::players::CertWatch>(players_db, std::move(cw));
            cert_watch->start();
        }

        // The server's own certificate (tls.mode auto) is renewed while the server runs, not only at start:
        // a server that is not restarted for a year would otherwise be left with one that has ended. When a
        // new one has been issued the listener is rebuilt exactly as after "regenerate" in the interface, and
        // the device port follows the files by itself. A failed attempt changes nothing: the old certificate
        // keeps serving and the next look tries again. The first look is an hour after start (the start has
        // just checked), then every six hours.
        std::jthread server_cert_renewal;
        if (tls_bindings.mode == "auto" && !tls_bindings.tls_dir.empty()) {
            server_cert_renewal = std::jthread(
                [&, tls_dir = tls_bindings.tls_dir, san_extra = tls_bindings.san_extra](std::stop_token st) {
                    auto nap = [&](std::chrono::seconds total) {
                        for (std::int64_t i = 0; i < total.count() && !st.stop_requested(); ++i)
                            std::this_thread::sleep_for(std::chrono::seconds(1));
                    };
                    nap(std::chrono::hours(1));
                    while (!st.stop_requested()) {
                        std::string err;
                        const auto r = renewServerCertIfDue(tls_dir, san_extra, err);
                        if (r == ServerCertRenewal::Renewed) {
                            LOG_INFO("tls: the server certificate was close to its end and has been renewed");
                            sad::AuditEvent ev;
                            ev.category       = sad::Category::System;
                            ev.action         = "tls.auto_renew";
                            ev.actor_username = "system";
                            ev.target_type    = "tls";
                            ev.summary        = "server certificate renewed on its own";
                            audit_logger.log(std::move(ev));
                            if (g_running.load(std::memory_order_acquire)) {
                                tls_reload_pending.store(true, std::memory_order_release);
                                g_running.store(false, std::memory_order_release);
                            }
                        } else if (r == ServerCertRenewal::Failed) {
                            LOG_ERROR("tls: the server certificate is close to its end and could not be renewed: {}", err);
                        }
                        nap(r == ServerCertRenewal::Failed ? std::chrono::hours(1) : std::chrono::hours(6));
                    }
                });
        }

        do {
            ControlApi api(api_port, manager, &metrics, livez_opts, &gateways,
                           &auth_svc, &ldap_repo, &smtp_repo, &rbac,
                           &event_bus, &preview, &stress_service, &plugins_mgr,
                           &master_key, &mounts_mgr, tls_bindings,
                           &time_repo, &time_src, sntp_client.get(),
                           &audit_logger, &audit_rate, players_service.get(),
                           enroll_service.get(), command_service.get());
            api.setOnTlsReload([&]() {
                tls_reload_pending.store(true, std::memory_order_release);
                g_running.store(false, std::memory_order_release);
            });
            if (players_service) {
                api.setServerCertsProvider(server_certs);
                metrics.setExtraPrometheus([&players_db, server_certs]() {
                    const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
                    return liveqx::players::certMetricsText(
                        liveqx::players::summarizeScreens(players_db, now), server_certs(), now);
                });
            }
            // fix35 A3.5–A3.9 — mount static UI (or 503-stub if headless).
            api.setBrokerHooks(
                [&]() {
                    // Everything the interface needs to say why the broker is (not) working.
                    json j = broker_link ? liveqx::mqtt::brokerStatusJson(broker_link->status())
                                         : json{{"state", "disabled"}, {"detail", ""}};
                    json cfg_b = json::object();
                    if (cfg.contains("players") && cfg["players"].is_object() &&
                        cfg["players"].contains("broker") && cfg["players"]["broker"].is_object())
                        cfg_b = cfg["players"]["broker"];
                    liveqx::mqtt::BrokerConnection conn;
                    if (broker_link) {
                        conn = broker_link->connection();
                    } else {
                        conn.host                 = cfg_b.value("host", std::string("127.0.0.1"));
                        conn.port                 = cfg_b.value("port", 8883);
                        conn.verify_hostname      = cfg_b.value("verify_hostname", true);
                        conn.reconcile_interval_s = cfg_b.value("reconcile_interval_s", 300);
                        if (players_service) conn = players_service->brokerConnection(conn);
                    }
                    j["enabled"]         = cfg_b.value("enabled", false);
                    j["host"]            = conn.host;
                    j["port"]            = conn.port;
                    j["verify_hostname"] = conn.verify_hostname;
                    j["reconcile_interval_s"] = conn.reconcile_interval_s;
#ifdef LIVEQX_HAVE_MOSQUITTO
                    constexpr bool kBuilt = true;
#else
                    constexpr bool kBuilt = false;
#endif
                    {
                        json crl = {{"mode", crl_publisher ? "local" : "off"}};
                        if (crl_publisher) {
                            const auto c = crl_publisher->status();
                            crl["last_publish_at"] = c.last_publish_at;
                            crl["last_ok"]         = c.last_ok;
                            crl["waiting"]         = c.waiting;
                            crl["error"]           = c.error;
                            crl["reload_error"]    = c.reload_error;
                            crl["revoked"]         = c.revoked;
                            crl["generations"]     = c.generations;
                            crl["valid_until"]     = c.valid_until;
                            crl["prepare_error"]   = c.prepare_error;
                            if (broker_tls) {
                                const auto t = broker_tls->state();
                                crl["tls"] = {{"certificate", t.certificate}, {"not_after", t.not_after}, {"error", t.error}};
                            }
                        }
                        j["crl"] = std::move(crl);
                    }
                    j["readiness"] = {
                        {"mosquitto_in_build", kBuilt},
                        {"device_ca", device_ca_provider ? device_ca_provider() != nullptr : false},
                        {"trust_root", players_service ? !players_service->caCertPem().empty() : false},
                        {"broker_url_configured",
                         players_service ? !players_service->settings().mqtt_broker_url.empty() : false}};
                    return j.dump();
                },
                (broker_link || crl_publisher)
                    ? std::function<void()>([&]() {
                          if (broker_link)   broker_link->requestSync();
                          if (crl_publisher) crl_publisher->request();
                      })
                    : std::function<void()>{},
                [&](const std::string& body) {
                    ControlApi::BrokerUpdate out;
                    auto j = json::parse(body, nullptr, false);
                    if (!players_service || j.is_discarded()) {
                        out.status = 400;
                        out.body = json{{"error", "invalid_body"}}.dump();
                        return out;
                    }
                    auto r = liveqx::players::updateBrokerConnection(j, *players_service, broker_link.get());
                    out.status = r.status;
                    out.body   = r.body.dump();
                    out.audit  = r.audit.dump();
                    return out;
                });
            // The pool of workers of the HTTP server and the cap on open streams (config: api_worker_threads,
            // api_max_streams, api_max_streams_per_user). A stream holds a worker for as long as it is open.
            {
                auto num = [&](const char* key, int dflt) {
                    return cfg.contains(key) && cfg[key].is_number_integer() ? cfg[key].get<int>() : dflt;
                };
                ControlApi::Limits lim;
                lim.worker_threads        = num("api_worker_threads", lim.worker_threads);
                lim.max_streams           = num("api_max_streams", lim.max_streams);
                lim.max_streams_per_user  = num("api_max_streams_per_user", lim.max_streams_per_user);
                api.setLimits(lim);
            }
            api.mountUi(paths.uiDir());
            api.start();
            const bool tls_on = !tls_bindings.cert_path.empty() &&
                                !tls_bindings.key_path.empty();
            LOG_INFO("Control API on {}://{}:{}/api/channels",
                     tls_on ? "https" : "http", api_bind, api_port);

            LOG_INFO("Running — send SIGINT or SIGTERM to stop");
            while (g_running.load(std::memory_order_relaxed))
                std::this_thread::sleep_for(std::chrono::milliseconds(100));

            api.stop();

            if (tls_reload_pending.exchange(false, std::memory_order_acq_rel)) {
                LOG_INFO("TLS reload — rebuilding listener with rotated cert");
                tls_bindings = buildTlsBindings();
                if (tls_failed) {
                    LOG_ERROR("TLS reload bootstrap failed; staying down");
                    break;
                }
                g_running.store(true, std::memory_order_release);
                continue;
            }
            break;
        } while (true);

        LOG_INFO("Shutting down…");
        notifier.stopWatchdogLoop();
        notifier.stopping("draining");
        // event_bus + preview die at scope-exit. Unhook the bus + preview
        // from the manager first so no late publish or render-tap racing
        // past stopAll() touches a dead bus / manager.
        manager.setEventBus(nullptr);
        manager.setPreviewManager(nullptr);
        auth_svc.setEventBus(nullptr);
        // Flush + join the audit writer thread before audit_db/master_key
        // go out of scope. stop() is idempotent; ~AuditLogger would call
        // it anyway, but doing it here keeps shutdown ordering explicit.
        audit_retention.stop();
        audit_logger.stop();
    }

    gateways.stopAll();
    watchdog.stop();
    manager.stopAll();
    LOG_INFO("Done");
    spdlog::shutdown();
    return 0;
}
