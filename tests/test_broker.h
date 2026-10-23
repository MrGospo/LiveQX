#pragma once
// A real Mosquitto broker for tests, started and stopped by the test itself.
//
// It repeats the stand used to verify the broker design (stage 7a): mutual TLS
// with the identity taken from the certificate, Dynamic Security for rights.
// But it is built from the server's OWN code, so a test exercises what
// production will really issue:
//   * the device CA (offline root + intermediate) signs the players' certificates;
//   * the server's identity is DeviceCa::ensureBrokerIdentity (CN liveqx-server);
//   * the broker's own certificate comes from the separate server CA.
//
// Tests skip (they do not fail) when mosquitto, mosquitto_ctrl or the Dynamic
// Security plugin are not installed.
//
// The broker runs in its own process and is ALWAYS taken down with the fixture:
// SIGTERM, then SIGKILL, then reaped. It listens on 127.0.0.1 only.

#include "auth/KeyWrap.h"
#include "auth/MasterKey.h"
#include "mqtt/MqttClient.h"
#include "utils/DeviceCa.h"
#include "utils/DeviceRootCa.h"
#include "utils/Tls.h"

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/x509.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

extern char** environ;

namespace liveqx::testing {

namespace fs = std::filesystem;

inline std::string readFileText(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}
inline void writeFileText(const fs::path& p, const std::string& s, mode_t mode = 0600) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << s;
    f.close();
    ::chmod(p.c_str(), mode);
}

inline std::optional<fs::path> findExecutable(const std::string& name) {
    const char* path = std::getenv("PATH");
    std::string dirs = path ? path : "/usr/bin:/bin";
    dirs += ":/usr/sbin:/usr/local/sbin";
    std::stringstream ss(dirs);
    std::string d;
    while (std::getline(ss, d, ':')) {
        if (d.empty()) continue;
        fs::path p = fs::path(d) / name;
        std::error_code ec;
        if (fs::exists(p, ec) && ::access(p.c_str(), X_OK) == 0) return p;
    }
    return std::nullopt;
}

// The broker the tests run. The stock Mosquitto 2.0.18 can corrupt its own session list when it
// rereads its settings (see packaging/mosquitto/README.md), so the patched build is used when it
// has been made (packaging/mosquitto/build.sh); otherwise whatever `mosquitto` is on the PATH.
inline std::optional<fs::path> brokerBinary() {
#ifdef LIVEQX_MOSQUITTO_PREFIX
    {
        const fs::path patched = fs::path(LIVEQX_MOSQUITTO_PREFIX) / "sbin" / "mosquitto";
        std::error_code ec;
        if (fs::exists(patched, ec) && ::access(patched.c_str(), X_OK) == 0) return patched;
    }
#endif
    return findExecutable("mosquitto");
}
inline bool brokerIsPatched() {
#ifdef LIVEQX_MOSQUITTO_PREFIX
    const auto b = brokerBinary();
    return b && b->parent_path().parent_path() == fs::path(LIVEQX_MOSQUITTO_PREFIX);
#else
    return false;
#endif
}

inline std::optional<fs::path> findDynsecPlugin() {
    for (const char* r : {"/usr/lib", "/usr/local/lib", "/usr/lib64"}) {
        std::error_code ec;
        const fs::path root(r);
        if (!fs::exists(root, ec)) continue;
        if (fs::exists(root / "mosquitto_dynamic_security.so", ec)) return root / "mosquitto_dynamic_security.so";
        for (const auto& sub : fs::directory_iterator(root, ec))
            if (sub.is_directory(ec) && fs::exists(sub.path() / "mosquitto_dynamic_security.so", ec))
                return sub.path() / "mosquitto_dynamic_security.so";
    }
    return std::nullopt;
}

struct ProcResult {
    int         code{-1};
    std::string output;
};

// Runs a program to completion (no shell), stdin closed, stdout+stderr captured.
inline ProcResult runProcess(const std::vector<std::string>& argv, const fs::path& scratch,
                             int timeout_ms = 15000) {
    ProcResult r;
    static std::atomic<int> seq{0};
    const fs::path out = scratch / ("proc-" + std::to_string(seq.fetch_add(1)) + ".out");
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, out.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    posix_spawn_file_actions_adddup2(&fa, 1, 2);
    std::vector<char*> args;
    for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    pid_t pid = 0;
    if (posix_spawnp(&pid, argv[0].c_str(), &fa, nullptr, args.data(), environ) != 0) {
        posix_spawn_file_actions_destroy(&fa);
        return r;
    }
    posix_spawn_file_actions_destroy(&fa);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    int status = 0;
    while (true) {
        const pid_t w = ::waitpid(pid, &status, WNOHANG);
        if (w == pid) { r.code = WIFEXITED(status) ? WEXITSTATUS(status) : -1; break; }
        if (std::chrono::steady_clock::now() > deadline) {
            ::kill(pid, SIGKILL);
            ::waitpid(pid, &status, 0);
            r.code = -2;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    r.output = readFileText(out);
    std::error_code ec;
    fs::remove(out, ec);
    return r;
}

inline int freeLoopbackPort() {
    int s = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a);
    socklen_t len = sizeof a;
    ::getsockname(s, reinterpret_cast<sockaddr*>(&a), &len);
    const int port = ntohs(a.sin_port);
    ::close(s);
    return port;
}

inline bool portAccepts(int port) {
    int s = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(static_cast<uint16_t>(port));
    const bool ok = ::connect(s, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0;
    ::close(s);
    return ok;
}

inline std::string randomUuidV4() {
    unsigned char b[16];
    RAND_bytes(b, sizeof b);
    b[6] = static_cast<unsigned char>((b[6] & 0x0f) | 0x40);
    b[8] = static_cast<unsigned char>((b[8] & 0x3f) | 0x80);
    static const char* d = "0123456789abcdef";
    std::string s;
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) s.push_back('-');
        s.push_back(d[b[i] >> 4]);
        s.push_back(d[b[i] & 15]);
    }
    return s;
}

// Key + CSR as a player would make them.
inline std::string makeCsrAndKey(std::string& key_pem) {
    EVP_PKEY* key = EVP_EC_gen("P-256");
    BIO* kb = BIO_new(BIO_s_mem());
    PEM_write_bio_PrivateKey(kb, key, nullptr, nullptr, 0, nullptr, nullptr);
    char* kd = nullptr;
    long kn = BIO_get_mem_data(kb, &kd);
    key_pem.assign(kd, static_cast<size_t>(kn));
    BIO_free_all(kb);
    X509_REQ* req = X509_REQ_new();
    X509_REQ_set_pubkey(req, key);
    X509_REQ_sign(req, key, EVP_sha256());
    BIO* b = BIO_new(BIO_s_mem());
    PEM_write_bio_X509_REQ(b, req);
    char* d = nullptr;
    long n = BIO_get_mem_data(b, &d);
    std::string pem(d, static_cast<size_t>(n));
    BIO_free_all(b);
    X509_REQ_free(req);
    EVP_PKEY_free(key);
    return pem;
}

inline bool waitFor(const std::function<bool()>& pred, int timeout_ms = 10000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return pred();
}

class TestBroker {
public:
    struct Player {
        std::string id;
        std::string cert_chain_pem;
        std::string key_pem;
        std::string serial_hex;
        int         generation{0};
        std::int64_t not_before{0}, not_after{0};
    };

    // nullopt when a broker can be started here; otherwise why not.
    static std::optional<std::string> unavailable() {
        if (!findExecutable("mosquitto")) return "mosquitto is not installed";
        if (!findExecutable("mosquitto_ctrl")) return "mosquitto_ctrl is not installed";
        if (!findDynsecPlugin()) return "the Dynamic Security plugin is not installed";
        return std::nullopt;
    }

    // `broker_dns` / `broker_ips`: names and addresses the broker's own certificate is valid for.
    // `with_crl`: the broker checks a revocation list (it starts with an empty one for
    // every generation) and writes a pid file, so it can be told to re-read the list.
    explicit TestBroker(std::vector<std::string> broker_dns = {"localhost"},
                        std::vector<std::string> broker_ips = {"127.0.0.1"},
                        bool with_crl = false) : with_crl_(with_crl) {
        static std::atomic<int> seq{0};
        dir_ = fs::temp_directory_path() /
               ("lqx-broker-" + std::to_string(::getpid()) + "-" + std::to_string(seq.fetch_add(1)));
        fs::create_directories(dir_);
        ok_ = build(broker_dns, broker_ips);
    }
    ~TestBroker() {
        stop();
        ca_.reset();
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }
    TestBroker(const TestBroker&)            = delete;
    TestBroker& operator=(const TestBroker&) = delete;

    bool ok() const { return ok_; }
    const std::string& problem() const { return problem_; }
    int port() const { return port_; }

    // What clients trust for the broker's certificate.
    std::string brokerCaPem() const { return readFileText(dir_ / "server_ca" / "ca.crt"); }

    mqtt::ClientOptions serverOptions() const {
        mqtt::ClientOptions o;
        o.host = "127.0.0.1";
        o.port = port_;
        o.client_id = kBrokerName;
        o.ca_pem = brokerCaPem();
        o.cert_chain_pem = identity_.cert_chain_pem;
        o.key_pem = identity_.key_pem;
        o.reconnect_min_s = 1;
        o.reconnect_max_s = 2;
        return o;
    }
    mqtt::ClientOptions playerOptions(const Player& p) const {
        mqtt::ClientOptions o;
        o.host = "127.0.0.1";
        o.port = port_;
        o.client_id = p.id;
        o.ca_pem = brokerCaPem();
        o.cert_chain_pem = p.cert_chain_pem;
        o.key_pem = p.key_pem;
        o.reconnect_min_s = 1;
        o.reconnect_max_s = 2;
        return o;
    }

    // A player with a certificate from the device CA; `provision` registers it
    // (client + personal role + common role) on the broker, as pairing will.
    Player enrollPlayer(bool provision = true) { return enrollPlayerAs(randomUuidV4(), provision); }

    // The same, for a player id that already exists elsewhere (in a database).
    Player enrollPlayerAs(const std::string& id, bool provision = true) {
        Player p;
        p.id = id;
        std::string key;
        const std::string csr = makeCsrAndKey(key);
        tls::IssuedClientCert cert;
        std::string err;
        if (!ca_->signClientCsr(csr, p.id, 365, cert, err)) { problem_ = "sign: " + err; return p; }
        p.cert_chain_pem = cert.cert_pem + cert.chain_pem;
        p.key_pem = key;
        p.serial_hex = cert.serial_hex;
        p.generation = cert.generation;
        p.not_before = cert.not_before_unix;
        p.not_after = cert.not_after_unix;
        if (provision) provision_(p.id);
        return p;
    }

    // The administrative tool, speaking as the server (certificate identity).
    std::string ctrl(const std::vector<std::string>& args) {
        std::vector<std::string> argv = {"mosquitto_ctrl", "--cafile", (dir_ / "ctrl-ca.crt").string(),
                                         "--cert", (dir_ / "ctrl.crt").string(),
                                         "--key", (dir_ / "ctrl.key").string(), "--insecure",
                                         "-h", "127.0.0.1", "-p", std::to_string(port_), "dynsec"};
        argv.insert(argv.end(), args.begin(), args.end());
        return runProcess(argv, dir_).output;
    }

    fs::path crlFile() const { return dir_ / "crl.pem"; }
    fs::path pidFile() const { return dir_ / "broker.pid"; }
    fs::path caDir() const { return dir_ / "ca"; }
    const auth::MasterKey& masterKey() const { return *mk_; }

    void stop() {
        if (pid_ <= 0) return;
        ::kill(pid_, SIGTERM);
        for (int i = 0; i < 300; ++i) {
            int status = 0;
            if (::waitpid(pid_, &status, WNOHANG) == pid_) { pid_ = -1; return; }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        ::kill(pid_, SIGKILL);
        int status = 0;
        ::waitpid(pid_, &status, 0);
        pid_ = -1;
    }
    bool start() {
        if (pid_ > 0) return true;
        std::vector<std::string> argv = {brokerBinary()->string(), "-c", (dir_ / "mosquitto.conf").string()};
        std::vector<char*> args;
        for (auto& a : argv) args.push_back(a.data());
        args.push_back(nullptr);
        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
        posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
        posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
        pid_t pid = 0;
        const int rc = posix_spawnp(&pid, argv[0].c_str(), &fa, nullptr, args.data(), environ);
        posix_spawn_file_actions_destroy(&fa);
        if (rc != 0) { problem_ = "cannot start mosquitto"; return false; }
        pid_ = pid;
        if (!waitFor([&] { return portAccepts(port_); }, 8000)) {
            problem_ = "the broker did not come up: " + readFileText(dir_ / "broker.log");
            stop();
            return false;
        }
        return true;
    }
    void restart() { stop(); start(); }
    // The broker rereads its settings and the revocation list, as when the server publishes a new one.
    void reload() { if (pid_ > 0) ::kill(pid_, SIGHUP); }
    // The broker dies with no goodbye (power cut, OOM kill): open connections are cut, not closed.
    void crash() {
        if (pid_ <= 0) return;
        ::kill(pid_, SIGKILL);
        int status = 0;
        ::waitpid(pid_, &status, 0);
        pid_ = -1;
    }
    std::string log() const { return readFileText(dir_ / "broker.log"); }

private:
    bool build(const std::vector<std::string>& broker_dns, const std::vector<std::string>& broker_ips) {
        if (auto why = unavailable()) { problem_ = *why; return false; }
        port_ = freeLoopbackPort();

        mk_ = std::make_unique<auth::MasterKey>((dir_ / "master.key").string());
        if (!mk_->load()) { problem_ = "master key"; return false; }

        // The device CA: offline root, intermediate on the "server".
        std::string err, csr, inter;
        const fs::path root = dir_ / "root";
        if (!tls::createDeviceRoot(root, "pass", err, auth::keywrap::KdfParams::minimal())) { problem_ = err; return false; }
        if (!tls::DeviceCa::createIntermediateRequest(dir_ / "ca", *mk_, csr, err)) { problem_ = err; return false; }
        if (!tls::signDeviceIntermediate(root, "pass", csr, 1825, inter, err)) { problem_ = err; return false; }
        if (!tls::DeviceCa::importIntermediate(dir_ / "ca", *mk_, inter, readFileText(root / "root.crt"), err)) { problem_ = err; return false; }
        ca_ = tls::DeviceCa::open(dir_ / "ca", *mk_, err, false);
        if (!ca_) { problem_ = err; return false; }

        // The broker's own certificate, from the separate server CA.
        const fs::path sca = dir_ / "server_ca";
        if (!tls::ensureCa(sca).ok()) { problem_ = "server CA"; return false; }
        tls::SanList sans;
        sans.dns_names = broker_dns;
        sans.ip_v4 = broker_ips;
        const std::string cn = broker_dns.empty() ? "broker" : broker_dns.front();
        if (!tls::issueServerCert(sca, cn, sans).ok()) { problem_ = "broker certificate"; return false; }

        // The server's identity for the broker: the real thing.
        if (!tls::DeviceCa::ensureBrokerIdentity(dir_ / "ca", *mk_, *ca_, identity_, err)) { problem_ = err; return false; }

        // Files for the broker and for mosquitto_ctrl (tests only: production keeps the key in memory).
        writeFileText(dir_ / "cafile.pem", ca_->rootPem() + ca_->chainPem());
        writeFileText(dir_ / "ctrl-ca.crt", brokerCaPem());
        writeFileText(dir_ / "ctrl.crt", identity_.cert_chain_pem);
        writeFileText(dir_ / "ctrl.key", identity_.key_pem);

        if (with_crl_) {
            // An empty list for every generation, as the server writes it.
            std::string all;
            for (int g : tls::DeviceCa::generations(dir_ / "ca")) {
                auto gen = tls::DeviceCa::openGeneration(dir_ / "ca", *mk_, g, err);
                std::string pem;
                if (!gen || !gen->generateCrl({}, 1, 30, pem, err)) { problem_ = "initial CRL: " + err; return false; }
                all += pem;
            }
            writeFileText(crlFile(), all, 0644);
        }

        const auto plugin = *findDynsecPlugin();
        fs::create_directories(dir_ / "data");
        std::ostringstream conf;
        if (with_crl_) conf << "pid_file " << pidFile().string() << "\n";
        conf << "listener " << port_ << " 127.0.0.1\n"
             << "cafile " << (dir_ / "cafile.pem").string() << "\n"
             << "certfile " << (sca / "server.crt").string() << "\n"
             << "keyfile " << (sca / "server.key").string() << "\n"
             << "require_certificate true\n"
             << (with_crl_ ? "crlfile " + crlFile().string() + "\n" : std::string())
             << "use_identity_as_username true\n"
             << "allow_anonymous false\n"
             << "persistence true\n"
             << "persistence_location " << (dir_ / "data").string() << "/\n"
             << "log_dest file " << (dir_ / "broker.log").string() << "\n"
             << "plugin " << plugin.string() << "\n"
             << "plugin_opt_config_file " << (dir_ / "dynamic-security.json").string() << "\n";
        writeFileText(dir_ / "mosquitto.conf", conf.str());

        // Dynamic Security: the server's own identity is the administrator.
        auto init = runProcess({"mosquitto_ctrl", "dynsec", "init", (dir_ / "dynamic-security.json").string(),
                                kBrokerName, "unused-password"}, dir_);
        if (init.code != 0 || !fs::exists(dir_ / "dynamic-security.json")) { problem_ = "dynsec init: " + init.output; return false; }
        if (!start()) return false;

        // Nothing is allowed unless a role says so.
        for (const char* t : {"publishClientSend", "publishClientReceive", "subscribe", "unsubscribe"})
            ctrl({"setDefaultACLAccess", t, "deny"});
        // Everyone gets the broadcast; the server may command and listen to presence.
        ctrl({"createRole", "player-common"});
        ctrl({"addRoleACL", "player-common", "subscribeLiteral", "liveqx/v1/broadcast/commands", "allow"});
        ctrl({"addRoleACL", "player-common", "publishClientReceive", "liveqx/v1/broadcast/commands", "allow"});
        ctrl({"createRole", "server-ops"});
        ctrl({"addRoleACL", "server-ops", "publishClientSend", "liveqx/v1/players/+/commands", "allow"});
        ctrl({"addRoleACL", "server-ops", "publishClientSend", "liveqx/v1/broadcast/commands", "allow"});
        ctrl({"addRoleACL", "server-ops", "subscribePattern", "liveqx/v1/players/+/presence", "allow"});
        ctrl({"addRoleACL", "server-ops", "publishClientReceive", "liveqx/v1/players/+/presence", "allow"});
        ctrl({"addRoleACL", "server-ops", "publishClientSend", "liveqx/v1/players/+/presence", "allow"});
        ctrl({"addClientRole", kBrokerName, "server-ops", "5"});
        return true;
    }

    void provision_(const std::string& id) {
        ctrl({"createClient", id});
        ctrl({"createRole", "player-" + id});
        ctrl({"addRoleACL", "player-" + id, "publishClientSend", "liveqx/v1/players/" + id + "/presence", "allow"});
        ctrl({"addRoleACL", "player-" + id, "subscribeLiteral", "liveqx/v1/players/" + id + "/commands", "allow"});
        ctrl({"addRoleACL", "player-" + id, "publishClientReceive", "liveqx/v1/players/" + id + "/commands", "allow"});
        ctrl({"addClientRole", id, "player-common", "1"});
        ctrl({"addClientRole", id, "player-" + id, "2"});
    }

    static constexpr const char* kBrokerName = "liveqx-server";

    bool        with_crl_{false};
    fs::path    dir_;
    int         port_{0};
    pid_t       pid_{-1};
    bool        ok_{false};
    std::string problem_;
    std::unique_ptr<auth::MasterKey> mk_;
    std::unique_ptr<tls::DeviceCa>   ca_;
    tls::BrokerIdentity              identity_;
};

}  // namespace liveqx::testing
