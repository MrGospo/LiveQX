// Writes a complete test PKI, produced by LiveQX's own code, to the directory
// named in LQX_EXPORT_PKI_DIR. Skipped when the variable is not set.
//
// It exists so a real broker (Mosquitto) can be pointed at exactly what the
// server issues: two-tier device CA (offline root + intermediate), player
// certificates with their chain, CRLs, and a TLS certificate for the broker
// from the separate server CA. Used for the revocation experiments of stage 8
// (CRL over the chain, disconnecting clients).
//
//   LQX_EXPORT_PKI_DIR=/some/dir ./liveqx-tests --gtest_filter='PkiExport.*'

#include "auth/KeyWrap.h"
#include "auth/MasterKey.h"
#include "utils/DeviceCa.h"
#include "utils/DeviceRootCa.h"
#include "utils/Tls.h"

#include <gtest/gtest.h>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>

namespace fs  = std::filesystem;
namespace tls = liveqx::tls;
namespace kw  = liveqx::auth::keywrap;

namespace {

void write(const fs::path& p, const std::string& s) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << s;
}

std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

// Player key + CSR; the key goes to `key_path` in PEM.
std::string makePlayerCsr(const fs::path& key_path) {
    EVP_PKEY* key = EVP_EC_gen("P-256");
    BIO* kb = BIO_new(BIO_s_mem());
    PEM_write_bio_PrivateKey(kb, key, nullptr, nullptr, 0, nullptr, nullptr);
    char* kd = nullptr;
    long kn = BIO_get_mem_data(kb, &kd);
    write(key_path, std::string(kd, static_cast<size_t>(kn)));
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

}  // namespace

TEST(PkiExport, WritesATestPkiWhenAsked) {
    const char* env = std::getenv("LQX_EXPORT_PKI_DIR");
    if (!env || !*env) GTEST_SKIP() << "set LQX_EXPORT_PKI_DIR to export a test PKI";
    const fs::path out = env;
    fs::create_directories(out);
    const fs::path work = out / "work";
    fs::create_directories(work);

    // ── the device CA: offline root + intermediate on the "server" ──
    liveqx::auth::MasterKey mk((work / "master.key").string());
    ASSERT_TRUE(mk.load());
    const fs::path root_dir = work / "offline-root";
    std::string err;
    ASSERT_TRUE(tls::createDeviceRoot(root_dir, "export-root-passphrase", err,
                                      kw::KdfParams::minimal())) << err;
    std::string csr;
    ASSERT_TRUE(tls::DeviceCa::createIntermediateRequest(work / "device_ca", mk, csr, err)) << err;
    std::string inter;
    ASSERT_TRUE(tls::signDeviceIntermediate(root_dir, "export-root-passphrase", csr, 1825,
                                            inter, err)) << err;
    ASSERT_TRUE(tls::DeviceCa::importIntermediate(work / "device_ca", mk, inter,
                                                  slurp(root_dir / "root.crt"), err)) << err;
    auto ca = tls::DeviceCa::open(work / "device_ca", mk, err, /*allow_create=*/false);
    ASSERT_TRUE(ca) << err;

    write(out / "device_root.crt", ca->rootPem());
    write(out / "device_intermediate.crt", ca->chainPem());

    // ── two players ──
    const char* ids[] = {"11111111-2222-4333-8444-555555555555",
                         "66666666-7777-4888-9999-000000000000"};
    std::string serials[2];
    for (int i = 0; i < 2; ++i) {
        const std::string tag = "client" + std::to_string(i + 1);
        const std::string player_csr = makePlayerCsr(out / (tag + ".key"));
        tls::IssuedClientCert c;
        ASSERT_TRUE(ca->signClientCsr(player_csr, ids[i], 365, c, err)) << err;
        write(out / (tag + ".crt"), c.cert_pem);
        write(out / (tag + "_fullchain.pem"), c.cert_pem + c.chain_pem);
        write(out / (tag + ".serial"), c.serial_hex + "\n");
        write(out / (tag + ".id"), std::string(ids[i]) + "\n");
        serials[i] = c.serial_hex;
    }

    // ── CRLs of the intermediate: nothing revoked, and client1 revoked ──
    std::string crl;
    ASSERT_TRUE(ca->generateCrl({}, 1, 30, crl, err)) << err;
    write(out / "device_crl_empty.pem", crl);
    ASSERT_TRUE(ca->generateCrl({{serials[0], static_cast<std::int64_t>(std::time(nullptr))}},
                                2, 30, crl, err)) << err;
    write(out / "device_crl_client1_revoked.pem", crl);

    // ── the broker's TLS certificate, from the separate server CA ──
    const fs::path serverca = out / "server_ca";
    auto sca = tls::ensureCa(serverca);
    ASSERT_TRUE(sca.ok()) << sca.error;
    tls::SanList sans;
    sans.dns_names = {"localhost"};
    sans.ip_v4 = {"127.0.0.1"};
    auto srv = tls::issueServerCert(serverca, "localhost", sans);
    ASSERT_TRUE(srv.ok()) << srv.error;
    write(out / "server_ca.crt", slurp(serverca / "ca.crt"));
    write(out / "broker.crt", slurp(srv.cert_path));
    write(out / "broker.key", slurp(srv.key_path));
}
