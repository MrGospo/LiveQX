// fix41 — entry point для liveqx-mountd.
//
// Минималистичный privileged helper. Принимает RPC от liveqx,
// материализует systemd .mount/.automount юниты, отвечает.
//
// Этот коммит подвязывает реальные handlers (SystemctlClient +
// PlaintextCredsHelper + MountTester) поверх RpcServer. CLI-флаги
// разрешают подменить пути для интеграционных тестов (--unit-dir,
// --cred-dir, --mount-bin, --systemctl-bin).
//
// fix43: helper больше не использует systemd-creds(1) — cred-файлы
// пишутся в plaintext на tmpfs (/run/liveqx/creds). См. CredsHelper.h.

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

#include <grp.h>
#include <pwd.h>

#include <spdlog/spdlog.h>

#include "mounts/CredsHelper.h"
#include "mounts/MountTester.h"
#include "mounts/MountdHandlers.h"
#include "mounts/RpcServer.h"
#include "mounts/SystemctlClient.h"

namespace {

liveqx::mounts::RpcServer* g_server = nullptr;

void handleSignal(int /*sig*/) {
    if (g_server) g_server->stop();
}

void installSignalHandlers() {
    struct sigaction sa {};
    sa.sa_handler = &handleSignal;
    ::sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    ::sigaction(SIGTERM, &sa, nullptr);
    ::sigaction(SIGINT,  &sa, nullptr);
    // SIGPIPE на write() в закрытый сокет — игнор, errno=EPIPE
    // обработает RpcProtocol.
    ::signal(SIGPIPE, SIG_IGN);
}

void printUsage(const char* prog) {
    std::fprintf(stderr,
        "Usage: %s [OPTIONS]\n"
        "\n"
        "  --socket PATH           Unix socket to listen on\n"
        "                          (default: %s)\n"
        "  --client-group NAME     POSIX group allowed to connect\n"
        "                          (default: %s)\n"
        "  --skip-chown            Don't chown the socket to client-group\n"
        "                          (test-only; client uid is allowed in)\n"
        "  --unit-dir PATH         Where to write generated systemd units\n"
        "                          (default: /etc/systemd/system)\n"
        "  --cred-dir PATH         Where to write plaintext CIFS credentials\n"
        "                          (default: /run/liveqx/creds, tmpfs 0700)\n"
        "  --cifs-uid N            Owner uid injected into CIFS mount Options=\n"
        "                          (default: resolved from user 'liveqx')\n"
        "  --cifs-gid N            Owner gid injected into CIFS mount Options=\n"
        "  --cifs-file-mode MODE   file_mode= for CIFS Options= (default: 0644)\n"
        "  --cifs-dir-mode  MODE   dir_mode=  for CIFS Options= (default: 0755)\n"
        "  --systemctl-bin PATH    Override systemctl binary path\n"
        "  --mount-bin PATH        Override mount binary path (for --test)\n"
        "  --umount-bin PATH       Override umount binary path\n"
        "  --no-creds              Use a no-op creds helper (test-only;\n"
        "                          treats apply for CIFS-with-password as\n"
        "                          if creds were written)\n"
        "  --no-tester             Use a no-op mount tester (test-only)\n"
        "  --help                  This message\n"
        "\n"
        "liveqx-mountd is a root-privileged helper. It listens for\n"
        "RPC from liveqx (running as service-user) and applies\n"
        "systemd .mount/.automount units for CIFS/NFS shares.\n"
        "\n"
        "See docs/MOUNTS.md for protocol and security details.\n",
        prog,
        liveqx::mounts::kDefaultSocketPath,
        liveqx::mounts::kClientGroupName);
}

// Заглушки для тестов: --no-creds / --no-tester позволяют гонять mountd
// в интеграционных условиях без записи реальных cred-файлов / mount(8).
class NoopCreds final : public liveqx::mounts::ICredsHelper {
public:
    bool writeFile(const std::string&,
                   const std::filesystem::path& out, std::string&) override {
        // Пишем «marker» вместо реального cred-файла, чтобы files were touched.
        std::error_code ec;
        std::filesystem::create_directories(out.parent_path(), ec);
        std::ofstream(out.string()) << "noop";
        return true;
    }
    bool removeFile(const std::filesystem::path& p, std::string&) override {
        std::error_code ec;
        std::filesystem::remove(p, ec);
        return true;
    }
};

class NoopTester final : public liveqx::mounts::IMountTester {
public:
    liveqx::mounts::TestMountResult test(
        const liveqx::mounts::MountSpec& spec) override {
        liveqx::mounts::TestMountResult r;
        std::string verr;
        if (!spec.validate(verr)) { r.error = verr; return r; }
        r.ok = true;
        r.files = 0;
        r.duration_ms = 0;
        return r;
    }
};

}  // namespace

int main(int argc, char** argv) {
    using liveqx::mounts::RpcServer;
    using liveqx::mounts::MountdHandlers;
    using liveqx::mounts::MountdHandlersConfig;
    using liveqx::mounts::SystemctlClient;
    using liveqx::mounts::PlaintextCredsHelper;
    using liveqx::mounts::MountTester;
    using liveqx::mounts::ISystemctl;
    using liveqx::mounts::ICredsHelper;
    using liveqx::mounts::IMountTester;

    RpcServer::Config rpc_cfg;
    MountdHandlersConfig handlers_cfg;
    auto sysctl_cfg  = SystemctlClient::defaultConfig();
    auto tester_cfg  = MountTester::defaultConfig();

    bool use_noop_creds  = false;
    bool use_noop_tester = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto need = [&](const char* opt) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s requires an argument\n", opt);
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--help" || a == "-h") {
            printUsage(argv[0]);
            return 0;
        } else if (a == "--socket")              rpc_cfg.socket_path  = need("--socket");
        else if   (a == "--client-group")        rpc_cfg.client_group = need("--client-group");
        else if   (a == "--skip-chown")          rpc_cfg.skip_chown   = true;
        else if   (a == "--unit-dir")            handlers_cfg.unit_ctx.systemd_unit_dir = need("--unit-dir");
        else if   (a == "--cred-dir")            handlers_cfg.unit_ctx.cred_dir         = need("--cred-dir");
        else if   (a == "--cifs-uid")            handlers_cfg.unit_ctx.cifs_uid =
            static_cast<std::uint32_t>(std::stoul(need("--cifs-uid")));
        else if   (a == "--cifs-gid")            handlers_cfg.unit_ctx.cifs_gid =
            static_cast<std::uint32_t>(std::stoul(need("--cifs-gid")));
        else if   (a == "--cifs-file-mode")      handlers_cfg.unit_ctx.cifs_file_mode = need("--cifs-file-mode");
        else if   (a == "--cifs-dir-mode")       handlers_cfg.unit_ctx.cifs_dir_mode  = need("--cifs-dir-mode");
        else if   (a == "--systemctl-bin")       sysctl_cfg.bin_path  = need("--systemctl-bin");
        else if   (a == "--mount-bin")           tester_cfg.mount_bin = need("--mount-bin");
        else if   (a == "--umount-bin")          tester_cfg.umount_bin = need("--umount-bin");
        else if   (a == "--no-creds")            use_noop_creds = true;
        else if   (a == "--no-tester")           use_noop_tester = true;
        else {
            std::fprintf(stderr, "Unknown arg: %s\n", a.c_str());
            printUsage(argv[0]);
            return 2;
        }
    }

    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [mountd] [%l] %v");

    // fix44: резолвим uid/gid пользователя liveqx один раз при старте.
    // mount.cifs без uid=/gid=/file_mode=/dir_mode= в Options= мапит все файлы
    // на root, и Watcher liveqx'а видит пустой каталог. CLI-флаги
    // --cifs-uid/--cifs-gid имеют приоритет: если оператор задал руками,
    // getpwnam не вызываем.
    if (handlers_cfg.unit_ctx.cifs_uid == 0) {
        if (auto* pw = ::getpwnam(liveqx::mounts::kClientGroupName)) {
            handlers_cfg.unit_ctx.cifs_uid = static_cast<std::uint32_t>(pw->pw_uid);
            if (handlers_cfg.unit_ctx.cifs_gid == 0) {
                handlers_cfg.unit_ctx.cifs_gid = static_cast<std::uint32_t>(pw->pw_gid);
            }
            spdlog::info("CIFS mounts will use uid={} gid={} (resolved from user '{}')",
                         handlers_cfg.unit_ctx.cifs_uid,
                         handlers_cfg.unit_ctx.cifs_gid,
                         liveqx::mounts::kClientGroupName);
        } else {
            spdlog::warn("user '{}' not found; CIFS mounts will keep uid=0/gid=0 "
                         "and non-root processes won't see files",
                         liveqx::mounts::kClientGroupName);
        }
    }
    // Если задан только uid, но gid не задан — попробуем дотянуть gid из той
    // же группы (имя группы совпадает с именем пользователя).
    if (handlers_cfg.unit_ctx.cifs_uid != 0
        && handlers_cfg.unit_ctx.cifs_gid == 0) {
        if (auto* gr = ::getgrnam(liveqx::mounts::kClientGroupName)) {
            handlers_cfg.unit_ctx.cifs_gid = static_cast<std::uint32_t>(gr->gr_gid);
        }
    }

    spdlog::info("liveqx-mountd starting, socket={} group={} unit_dir={} cred_dir={}",
                 rpc_cfg.socket_path.string(),
                 rpc_cfg.client_group,
                 handlers_cfg.unit_ctx.systemd_unit_dir.string(),
                 handlers_cfg.unit_ctx.cred_dir.string());

    if (sysctl_cfg.bin_path.empty()) {
        spdlog::warn("systemctl binary not found; daemon-reload/enable will fail");
    }

    // fix43: гарантируем существование cred_dir с правами 0700 root:root.
    // systemd создаёт parent /run/liveqx через RuntimeDirectory=,
    // но дочерний каталог creds/ — наш. На tmpfs это безопасно: файлы с
    // паролями исчезают при reboot и не доступны не-root пользователям.
    {
        std::error_code ec;
        std::filesystem::create_directories(handlers_cfg.unit_ctx.cred_dir, ec);
        if (ec) {
            spdlog::warn("mkdir {}: {}",
                         handlers_cfg.unit_ctx.cred_dir.string(), ec.message());
        }
        std::filesystem::permissions(handlers_cfg.unit_ctx.cred_dir,
                                     std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::replace, ec);
        if (ec) {
            spdlog::warn("chmod 0700 {}: {}",
                         handlers_cfg.unit_ctx.cred_dir.string(), ec.message());
        }
    }

    std::shared_ptr<ISystemctl>  systemctl =
        std::make_shared<SystemctlClient>(std::move(sysctl_cfg));

    std::shared_ptr<ICredsHelper> creds = use_noop_creds
        ? std::shared_ptr<ICredsHelper>(std::make_shared<NoopCreds>())
        : std::shared_ptr<ICredsHelper>(std::make_shared<PlaintextCredsHelper>());

    std::shared_ptr<IMountTester> tester = use_noop_tester
        ? std::shared_ptr<IMountTester>(std::make_shared<NoopTester>())
        : std::shared_ptr<IMountTester>(std::make_shared<MountTester>(std::move(tester_cfg)));

    MountdHandlers handlers(std::move(handlers_cfg),
                            std::move(systemctl),
                            std::move(creds),
                            std::move(tester));

    RpcServer server(std::move(rpc_cfg), handlers.asRpcHandler());
    g_server = &server;

    installSignalHandlers();

    std::string err;
    if (!server.start(err)) {
        spdlog::error("startup failed: {}", err);
        return 1;
    }
    server.runForever();
    spdlog::info("liveqx-mountd shutting down");
    return 0;
}
