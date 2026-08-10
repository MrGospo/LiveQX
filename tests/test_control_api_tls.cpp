// fix38 — ControlApi TLS handshake + REST admin integration test.
//
// Generates a fresh CA + server cert via utils/Tls into a tmp dir,
// boots ControlApi with TlsBindings pointing at those PEMs, and
// performs:
//   * GET /healthz over httplib::SSLClient with the CA in the trust
//     store. Expects 200 (handshake succeeds, route works).
//   * GET /healthz over plain httplib::Client on the same port.
//     Expects connection failure or empty result (TLS-only listener).
//   * GET /api/tls/info returns mode/cert/ca metadata.
//   * GET /api/tls/ca-bundle returns the CA cert as PEM bytes.
//   * POST /api/tls/regenerate-server reissues server.crt with a fresh
//     fingerprint and triggers the on_tls_reload callback.
//   * POST /api/tls/import accepts a re-issued cert+key+ca trio.
//   * /api/tls/* on an instance launched without tls_dir returns 503.

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include "api/ChannelManager.h"
#include "api/ControlApi.h"
#include "utils/Tls.h"

using nlohmann::json;

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

fs::path makeTlsTmp(const std::string& tag) {
    auto base = fs::temp_directory_path() / "sc-control-tls";
    fs::create_directories(base);
    static std::atomic<uint64_t> seq{0};
    auto stamp = std::to_string(
        std::chrono::system_clock::now().time_since_epoch().count());
    auto p = base / (tag + "_" + stamp + "_" + std::to_string(seq++));
    fs::create_directories(p);
    return p;
}

constexpr int kBasePort = 18620;

bool waitTlsReady(const std::string& host, int port, const fs::path& ca_path) {
    httplib::SSLClient cli(host, port);
    cli.set_ca_cert_path(ca_path.string().c_str());
    cli.enable_server_certificate_verification(true);
    cli.set_connection_timeout(0, 100'000);
    for (int i = 0; i < 100; ++i) {
        auto r = cli.Get("/healthz");
        if (r && r->status == 200) return true;
        std::this_thread::sleep_for(20ms);
    }
    return false;
}

}  // namespace

TEST(ControlApiTls, ServesHttpsWhenCertProvided) {
    auto dir = makeTlsTmp("https-ok");
    auto ca  = liveqx::tls::ensureCa(dir);
    ASSERT_TRUE(ca.ok()) << ca.error;

    liveqx::tls::SanList sans;
    sans.dns_names = {"localhost"};
    sans.ip_v4     = {"127.0.0.1"};
    auto srv = liveqx::tls::issueServerCert(dir, "localhost", sans);
    ASSERT_TRUE(srv.ok()) << srv.error;

    ChannelManager mgr(nullptr, fs::path{});
    TlsBindings tls;
    tls.cert_path = srv.cert_path;
    tls.key_path  = srv.key_path;
    tls.bind      = "127.0.0.1";

    int port = kBasePort + 0;
    ControlApi api(port, mgr, /*metrics=*/nullptr, {}, /*gateways=*/nullptr,
                   /*auth=*/nullptr, /*ldap_repo=*/nullptr,
                   /*smtp_repo=*/nullptr, /*rbac=*/nullptr,
                   /*events=*/nullptr, /*preview=*/nullptr,
                   /*stress=*/nullptr, /*plugins=*/nullptr,
                   /*master_key=*/nullptr, /*mounts=*/nullptr, tls);
    api.start();

    ASSERT_TRUE(waitTlsReady("127.0.0.1", port, ca.cert_path))
        << "TLS handshake never succeeded on port " << port;

    // Healthz roundtrip — verifies routes are live on the SSL listener.
    httplib::SSLClient cli("127.0.0.1", port);
    cli.set_ca_cert_path(ca.cert_path.string().c_str());
    cli.enable_server_certificate_verification(true);
    auto r = cli.Get("/healthz");
    ASSERT_TRUE(r) << "no response from /healthz over HTTPS";
    EXPECT_EQ(r->status, 200);

    api.stop();
    fs::remove_all(dir);
}

TEST(ControlApiTls, RejectsPlainHttpOnTlsListener) {
    auto dir = makeTlsTmp("http-on-https");
    auto ca  = liveqx::tls::ensureCa(dir);
    ASSERT_TRUE(ca.ok()) << ca.error;

    liveqx::tls::SanList sans;
    sans.dns_names = {"localhost"};
    sans.ip_v4     = {"127.0.0.1"};
    auto srv = liveqx::tls::issueServerCert(dir, "localhost", sans);
    ASSERT_TRUE(srv.ok()) << srv.error;

    ChannelManager mgr(nullptr, fs::path{});
    TlsBindings tls;
    tls.cert_path = srv.cert_path;
    tls.key_path  = srv.key_path;
    tls.bind      = "127.0.0.1";

    int port = kBasePort + 1;
    ControlApi api(port, mgr, /*metrics=*/nullptr, {}, /*gateways=*/nullptr,
                   /*auth=*/nullptr, /*ldap_repo=*/nullptr,
                   /*smtp_repo=*/nullptr, /*rbac=*/nullptr,
                   /*events=*/nullptr, /*preview=*/nullptr,
                   /*stress=*/nullptr, /*plugins=*/nullptr,
                   /*master_key=*/nullptr, /*mounts=*/nullptr, tls);
    api.start();
    ASSERT_TRUE(waitTlsReady("127.0.0.1", port, ca.cert_path));

    // Plain HTTP must NOT get a parseable HTTP response — the SSL
    // listener will reject the bytes outright. We accept either a null
    // result (connection error / read error) or a non-2xx status.
    httplib::Client http("127.0.0.1", port);
    http.set_connection_timeout(0, 200'000);
    auto r = http.Get("/healthz");
    if (r) {
        EXPECT_NE(r->status / 100, 2)
            << "plain HTTP unexpectedly succeeded against TLS listener";
    }
    // r==nullptr is the expected case — no assertion needed.

    api.stop();
    fs::remove_all(dir);
}

TEST(ControlApiTls, FallsBackToHttpWhenNoCert) {
    // No TlsBindings → ControlApi must serve plain HTTP exactly as
    // before fix38 so existing deployments keep working until the
    // operator opts into auto/provided mode.
    ChannelManager mgr(nullptr, fs::path{});
    int port = kBasePort + 2;
    ControlApi api(port, mgr);
    api.start();

    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(0, 100'000);
    bool ready = false;
    for (int i = 0; i < 100; ++i) {
        auto r = cli.Get("/healthz");
        if (r && r->status == 200) { ready = true; break; }
        std::this_thread::sleep_for(20ms);
    }
    EXPECT_TRUE(ready);
    api.stop();
}

namespace {

// Helper for the /api/tls/* tests: spin up ControlApi over HTTPS with a
// freshly minted CA + server cert, then wait until the listener answers.
struct TlsHarness {
    fs::path                        dir;
    liveqx::tls::CaResult   ca;
    liveqx::tls::ServerCertResult srv;
    std::unique_ptr<ChannelManager> mgr;
    std::unique_ptr<ControlApi>     api;
    int                             port{0};

    explicit TlsHarness(const std::string& tag, int p) {
        dir  = makeTlsTmp(tag);
        ca   = liveqx::tls::ensureCa(dir);
        liveqx::tls::SanList sans;
        sans.dns_names = {"localhost"};
        sans.ip_v4     = {"127.0.0.1"};
        srv  = liveqx::tls::issueServerCert(dir, "localhost", sans);
        mgr  = std::make_unique<ChannelManager>(nullptr, fs::path{});
        TlsBindings tls;
        tls.cert_path = srv.cert_path;
        tls.key_path  = srv.key_path;
        tls.tls_dir   = dir;
        tls.mode      = "auto";
        tls.bind      = "127.0.0.1";
        port = p;
        api = std::make_unique<ControlApi>(
            port, *mgr, /*metrics=*/nullptr, LivezOptions{},
            /*gateways=*/nullptr, /*auth=*/nullptr,
            /*ldap_repo=*/nullptr, /*smtp_repo=*/nullptr,
            /*rbac=*/nullptr, /*events=*/nullptr,
            /*preview=*/nullptr, /*stress=*/nullptr,
            /*plugins=*/nullptr, /*master_key=*/nullptr, /*mounts=*/nullptr, tls);
    }

    ~TlsHarness() {
        if (api) api->stop();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    void start() { api->start(); ASSERT_TRUE(waitTlsReady("127.0.0.1", port, ca.cert_path)); }

    std::unique_ptr<httplib::SSLClient> client() const {
        auto cli = std::make_unique<httplib::SSLClient>("127.0.0.1", port);
        cli->set_ca_cert_path(ca.cert_path.string().c_str());
        cli->enable_server_certificate_verification(true);
        cli->set_connection_timeout(0, 200'000);
        return cli;
    }
};

}  // namespace

TEST(ControlApiTls, InfoEndpointReturnsMetadata) {
    TlsHarness h("info", kBasePort + 10);
    h.start();
    auto cli = h.client();
    auto r = cli->Get("/api/tls/info");
    ASSERT_TRUE(r);
    ASSERT_EQ(r->status, 200);
    auto body = json::parse(r->body);
    EXPECT_EQ(body.value("mode", ""), "auto");
    EXPECT_EQ(body.value("tls_enabled", false), true);
    ASSERT_TRUE(body["server"].is_object()) << body.dump();
    ASSERT_TRUE(body["ca"].is_object()) << body.dump();
    EXPECT_EQ(body["server"].value("self_signed", true), false);
    EXPECT_EQ(body["ca"].value("self_signed", false), true);
    EXPECT_FALSE(body["server"].value("fingerprint_sha256", "").empty());
}

TEST(ControlApiTls, CaBundleReturnsPem) {
    TlsHarness h("cabundle", kBasePort + 11);
    h.start();
    auto cli = h.client();
    auto r = cli->Get("/api/tls/ca-bundle");
    ASSERT_TRUE(r);
    ASSERT_EQ(r->status, 200);
    EXPECT_NE(r->body.find("-----BEGIN CERTIFICATE-----"), std::string::npos);
    EXPECT_NE(r->body.find("-----END CERTIFICATE-----"),   std::string::npos);
}

TEST(ControlApiTls, RegenerateServerRotatesFingerprintAndFiresReload) {
    TlsHarness h("regen", kBasePort + 12);
    std::atomic<int> reload_count{0};
    h.api->setOnTlsReload([&]() { reload_count.fetch_add(1); });
    h.start();
    auto cli = h.client();

    auto info_before = cli->Get("/api/tls/info");
    ASSERT_TRUE(info_before);
    auto body_before = json::parse(info_before->body);
    auto fp_before = body_before["server"].value("fingerprint_sha256", "");
    ASSERT_FALSE(fp_before.empty());

    json req_body = {{"san_extra", {"example.lan"}}};
    auto r = cli->Post("/api/tls/regenerate-server",
                       req_body.dump(), "application/json");
    ASSERT_TRUE(r);
    ASSERT_EQ(r->status, 200) << r->body;
    auto out = json::parse(r->body);
    EXPECT_TRUE(out.value("ok", false));
    EXPECT_TRUE(out.value("restart_required", false));
    auto fp_after = out.value("fingerprint_sha256", "");
    EXPECT_FALSE(fp_after.empty());
    EXPECT_NE(fp_before, fp_after);

    // on_tls_reload runs after writeJson, but on the same thread — give it
    // a small grace then check.
    for (int i = 0; i < 50 && reload_count.load() == 0; ++i) {
        std::this_thread::sleep_for(10ms);
    }
    EXPECT_GE(reload_count.load(), 1);
}

TEST(ControlApiTls, ImportAcceptsValidCertKeyCa) {
    TlsHarness h("import", kBasePort + 13);
    h.start();
    auto cli = h.client();

    // Re-mint a cert with a different SAN to import. Use a separate dir
    // so we don't clobber the live one before we POST.
    auto donor_dir = makeTlsTmp("import-donor");
    auto donor_ca  = liveqx::tls::ensureCa(donor_dir);
    liveqx::tls::SanList sans;
    sans.dns_names = {"imported.local"};
    sans.ip_v4     = {"127.0.0.1"};
    auto donor_srv = liveqx::tls::issueServerCert(
        donor_dir, "imported", sans);
    ASSERT_TRUE(donor_srv.ok()) << donor_srv.error;

    auto slurp = [](const fs::path& p) {
        std::ifstream f(p, std::ios::binary);
        std::stringstream ss; ss << f.rdbuf();
        return ss.str();
    };
    json body = {
        {"cert", slurp(donor_srv.cert_path)},
        {"key",  slurp(donor_srv.key_path)},
        {"ca",   slurp(donor_ca.cert_path)},
    };
    auto r = cli->Post("/api/tls/import", body.dump(), "application/json");
    ASSERT_TRUE(r);
    ASSERT_EQ(r->status, 200) << r->body;
    auto out = json::parse(r->body);
    EXPECT_TRUE(out.value("ok", false));
    EXPECT_TRUE(out.value("ca_imported", false));
    EXPECT_EQ(out["server"].value("fingerprint_sha256", ""),
              donor_srv.fingerprint_sha256);

    fs::remove_all(donor_dir);
}

TEST(ControlApiTls, ImportRejectsMismatchedKeypair) {
    TlsHarness h("import-bad", kBasePort + 14);
    h.start();
    auto cli = h.client();

    auto donor_dir = makeTlsTmp("import-bad-donor");
    auto donor_ca  = liveqx::tls::ensureCa(donor_dir);
    liveqx::tls::SanList sans;
    sans.dns_names = {"a"};
    sans.ip_v4     = {"127.0.0.1"};
    auto srv1 = liveqx::tls::issueServerCert(donor_dir, "a", sans);
    auto srv2 = liveqx::tls::issueServerCert(donor_dir, "a", sans);
    ASSERT_TRUE(srv1.ok());
    ASSERT_TRUE(srv2.ok());

    auto slurp = [](const fs::path& p) {
        std::ifstream f(p, std::ios::binary);
        std::stringstream ss; ss << f.rdbuf();
        return ss.str();
    };
    // After issuing srv2 the on-disk server.{crt,key} are srv2's; but we
    // can read srv1's cert from before reissue isn't possible. Instead,
    // mismatch by sending donor_ca's cert with srv2's key (unrelated keys).
    json body = {
        {"cert", slurp(donor_ca.cert_path)},
        {"key",  slurp(srv2.key_path)},
    };
    auto r = cli->Post("/api/tls/import", body.dump(), "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400) << r->body;
    fs::remove_all(donor_dir);
}

TEST(ControlApiTls, ListenerSeesNewCertAfterReissueAndRestart) {
    // Mimics main.cpp's hot-reload loop: regenerate-server writes a new
    // cert pair to disk, the on_tls_reload callback fires, the operator
    // (here: this test) tears down ControlApi and constructs a fresh one
    // pointing at the same paths. The new listener must present the
    // newly-issued cert (different fingerprint) on the next handshake.
    auto dir = makeTlsTmp("reload");
    auto ca  = liveqx::tls::ensureCa(dir);
    liveqx::tls::SanList sans;
    sans.dns_names = {"localhost"};
    sans.ip_v4     = {"127.0.0.1"};
    auto srv = liveqx::tls::issueServerCert(dir, "localhost", sans);
    ASSERT_TRUE(srv.ok()) << srv.error;
    const auto fp1 = srv.fingerprint_sha256;

    ChannelManager mgr(nullptr, fs::path{});
    TlsBindings tls;
    tls.cert_path = srv.cert_path;
    tls.key_path  = srv.key_path;
    tls.tls_dir   = dir;
    tls.mode      = "auto";
    tls.bind      = "127.0.0.1";

    int port = kBasePort + 16;

    // Listener #1 — request /healthz to confirm it's serving the first cert.
    {
        ControlApi api(port, mgr, /*metrics=*/nullptr, LivezOptions{},
                       /*gateways=*/nullptr, /*auth=*/nullptr,
                       /*ldap_repo=*/nullptr, /*smtp_repo=*/nullptr,
                       /*rbac=*/nullptr, /*events=*/nullptr,
                       /*preview=*/nullptr, /*stress=*/nullptr,
                       /*plugins=*/nullptr, /*master_key=*/nullptr, /*mounts=*/nullptr, tls);
        api.start();
        ASSERT_TRUE(waitTlsReady("127.0.0.1", port, ca.cert_path));
        api.stop();
    }

    // Re-issue the cert at the same paths — overwrites server.{crt,key}.
    auto srv2 = liveqx::tls::issueServerCert(dir, "localhost", sans);
    ASSERT_TRUE(srv2.ok()) << srv2.error;
    EXPECT_NE(fp1, srv2.fingerprint_sha256);

    // Listener #2 — same TlsBindings, fresh ControlApi: the SSLServer ctor
    // re-reads the cert file off disk and presents the rotated fingerprint.
    {
        ControlApi api(port, mgr, /*metrics=*/nullptr, LivezOptions{},
                       /*gateways=*/nullptr, /*auth=*/nullptr,
                       /*ldap_repo=*/nullptr, /*smtp_repo=*/nullptr,
                       /*rbac=*/nullptr, /*events=*/nullptr,
                       /*preview=*/nullptr, /*stress=*/nullptr,
                       /*plugins=*/nullptr, /*master_key=*/nullptr, /*mounts=*/nullptr, tls);
        api.start();
        ASSERT_TRUE(waitTlsReady("127.0.0.1", port, ca.cert_path));

        httplib::SSLClient cli("127.0.0.1", port);
        cli.set_ca_cert_path(ca.cert_path.string().c_str());
        cli.enable_server_certificate_verification(true);
        auto r = cli.Get("/api/tls/info");
        ASSERT_TRUE(r);
        ASSERT_EQ(r->status, 200);
        auto body = json::parse(r->body);
        ASSERT_TRUE(body["server"].is_object());
        EXPECT_EQ(body["server"].value("fingerprint_sha256", ""),
                  srv2.fingerprint_sha256);
        api.stop();
    }
    fs::remove_all(dir);
}

TEST(ControlApiTls, AdminEndpointsReturn503WhenTlsDirEmpty) {
    // Plain HTTP instance — tls_dir is empty, so /api/tls/* should be 503.
    ChannelManager mgr(nullptr, fs::path{});
    int port = kBasePort + 15;
    ControlApi api(port, mgr);
    api.start();

    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(0, 200'000);
    bool ready = false;
    for (int i = 0; i < 100; ++i) {
        auto r = cli.Get("/healthz");
        if (r && r->status == 200) { ready = true; break; }
        std::this_thread::sleep_for(20ms);
    }
    ASSERT_TRUE(ready);

    auto info = cli.Get("/api/tls/info");
    ASSERT_TRUE(info);
    EXPECT_EQ(info->status, 200);  // info works even when disabled
    auto body = json::parse(info->body);
    EXPECT_EQ(body.value("tls_enabled", true), false);

    auto bundle = cli.Get("/api/tls/ca-bundle");
    ASSERT_TRUE(bundle);
    EXPECT_EQ(bundle->status, 503);

    auto regen = cli.Post("/api/tls/regenerate-server", "{}", "application/json");
    ASSERT_TRUE(regen);
    EXPECT_EQ(regen->status, 503);

    api.stop();
}
