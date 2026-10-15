// Device CA ceremony commands — see utils/DeviceCaCli.h.

#include "utils/DeviceCaCli.h"

#include "auth/MasterKey.h"
#include "utils/DeviceCa.h"
#include "utils/DeviceRootCa.h"

#include <sodium.h>
#include <sys/stat.h>

#include <fstream>
#include <iostream>
#include <iterator>

namespace fs = std::filesystem;
namespace sa = liveqx::auth;

namespace liveqx::tls {
namespace {

constexpr std::size_t kMaxFileBytes = 64 * 1024;

int fail(const std::string& msg) {
    std::cerr << "ERROR: " << msg << "\n";
    return 1;
}

// Short stable code -> a sentence an operator can act on.
std::string explain(const std::string& code) {
    if (code == "wrong_passphrase")        return "wrong passphrase";
    if (code == "root_already_exists")     return "a root already exists in that directory; it is never overwritten";
    if (code == "root_unreadable")         return "root.crt / root.key.sealed are missing or damaged";
    if (code == "no_pending_request")      return "there is no pending request; run --device-ca-request first";
    if (code == "certificate_does_not_match_request")
        return "this certificate belongs to a different request (the key does not match)";
    if (code == "certificate_not_signed_by_root")
        return "the certificate was not signed by the given root";
    if (code == "certificate_not_currently_valid") return "the certificate is expired or not valid yet";
    if (code == "csr_bad_subject")         return "the request is not an intermediate CA request of this server";
    return code;
}

bool readSmallFile(const fs::path& p, std::string& out, std::string& err) {
    std::ifstream f(p, std::ios::binary);
    if (!f) { err = "cannot read " + p.string(); return false; }
    out.assign(kMaxFileBytes + 1, '\0');
    f.read(out.data(), static_cast<std::streamsize>(out.size()));
    out.resize(static_cast<std::size_t>(f.gcount()));
    if (out.size() > kMaxFileBytes) { err = p.string() + " is too large"; return false; }
    return true;
}

// Opens the master key the way the service would, for a file key only:
// the CLI cannot see keys delivered through the service's environment.
bool openMasterKey(const fs::path& key_file, const std::string& key_store,
                   const sa::PassphraseSource& source, sa::MasterKey& key,
                   std::string& err) {
    if (key_store != "file") {
        err = "master_key_store is \"" + key_store + "\": the key is delivered from "
              "outside and cannot be opened here";
        return false;
    }
    if (std::getenv("LIVEQX_MASTER_KEY") != nullptr) {
        err = "LIVEQX_MASTER_KEY is set: the key comes from the environment and "
              "cannot be opened here";
        return false;
    }
    if (!key.load()) {
        if (!key.sealed()) { err = "cannot load the master key from " + key_file.string(); return false; }
        auto p = source("Master key passphrase");
        if (!p) { err = "the master key is passphrase protected and no passphrase was supplied"; return false; }
        std::string uerr;
        const bool ok = key.unlock(*p, uerr);
        sodium_memzero(p->data(), p->size());
        if (!ok) { err = uerr; return false; }
    }
    return true;
}

}  // namespace

int deviceCaRootInit(const fs::path& out_dir, const sa::PassphraseSource& passphrase,
                     const sa::keywrap::KdfParams& kdf) {
    std::error_code ec;
    if (fs::exists(out_dir / "root.crt", ec) || fs::exists(out_dir / "root.key.sealed", ec))
        return fail(explain("root_already_exists"));

    std::string err;
    auto pass = sa::promptNewPassphrase(passphrase, err);
    if (!pass) return fail(err);
    const bool ok = createDeviceRoot(out_dir, *pass, err, kdf);
    sodium_memzero(pass->data(), pass->size());
    if (!ok) return fail(explain(err));

    std::cout << "device_ca_root_created: true\n"
              << "root_cert: " << (out_dir / "root.crt") << "\n"
              << "root_key: " << (out_dir / "root.key.sealed") << "\n"
              << "\n"
              << "Keep root.key.sealed OFF the LiveQX server (an offline machine or removable\n"
              << "media) and remember the passphrase: it is needed every time an intermediate\n"
              << "is signed, and nothing can recover it. Copy only root.crt to the server.\n";
    return 0;
}

int deviceCaRequest(const fs::path& master_key_file, const fs::path& device_ca_dir,
                    const std::string& key_store, const sa::PassphraseSource& passphrase) {
    std::string err;
    sa::MasterKey key(master_key_file.string());
    if (!openMasterKey(master_key_file, key_store, passphrase, key, err)) return fail(err);

    std::string csr;
    if (!DeviceCa::createIntermediateRequest(device_ca_dir, key, csr, err))
        return fail(explain(err));

    std::cout << "device_ca_request: pending\n"
              << "Carry the request below to the machine that holds the root key and run:\n"
              << "  liveqx --device-ca-root-sign --root-dir <root dir> --csr <file with the request> "
                 "--out intermediate.crt\n"
              << "then, on this server:\n"
              << "  liveqx --device-ca-import --cert intermediate.crt --root <root.crt>\n"
              << "\n"
              << csr;
    return 0;
}

int deviceCaRootSign(const fs::path& root_dir, const fs::path& csr_file, const fs::path& out_file,
                     int validity_days, const sa::PassphraseSource& passphrase) {
    std::error_code ec;
    if (fs::exists(out_file, ec)) return fail(out_file.string() + " already exists; not overwritten");

    std::string csr, err;
    if (!readSmallFile(csr_file, csr, err)) return fail(err);
    auto pass = passphrase("Root key passphrase");
    if (!pass) return fail("no passphrase available");

    std::string cert;
    const bool ok = signDeviceIntermediate(root_dir, *pass, csr, validity_days, cert, err);
    sodium_memzero(pass->data(), pass->size());
    if (!ok) return fail(explain(err));

    {
        std::ofstream o(out_file, std::ios::binary | std::ios::trunc);
        o << cert;
        if (!o) return fail("cannot write " + out_file.string());
    }
    ::chmod(out_file.c_str(), 0644);
    std::cout << "device_ca_intermediate_signed: true\n"
              << "certificate: " << out_file << "\n"
              << "valid_days: " << validity_days << "\n"
              << "Copy it (and root.crt) to the server and run --device-ca-import.\n";
    return 0;
}

int deviceCaImport(const fs::path& master_key_file, const fs::path& device_ca_dir,
                   const std::string& key_store, const fs::path& cert_file,
                   const fs::path& root_file, const sa::PassphraseSource& passphrase) {
    std::string cert, root, err;
    if (!readSmallFile(cert_file, cert, err)) return fail(err);
    if (!readSmallFile(root_file, root, err)) return fail(err);

    sa::MasterKey key(master_key_file.string());
    if (!openMasterKey(master_key_file, key_store, passphrase, key, err)) return fail(err);

    if (!DeviceCa::importIntermediate(device_ca_dir, key, cert, root, err))
        return fail(explain(err));

    std::cout << "device_ca_imported: true\n"
              << "The new generation takes effect after you restart the LiveQX service; "
                 "certificates already issued under earlier generations stay valid.\n";
    return 0;
}

}  // namespace liveqx::tls
