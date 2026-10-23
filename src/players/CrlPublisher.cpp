#include "players/CrlPublisher.h"

#include "utils/Log.h"

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <sys/stat.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iterator>

namespace liveqx::players {

namespace fs = std::filesystem;

namespace {

// When the lists in an existing file run out (the earliest of them); 0 if there is no usable file.
std::int64_t validUntilOfFile(const fs::path& path) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) return 0;
    std::ifstream in(path, std::ios::binary);
    if (!in) return 0;
    const std::string pem((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    if (!bio) return 0;
    std::int64_t earliest = 0;
    while (X509_CRL* crl = PEM_read_bio_X509_CRL(bio, nullptr, nullptr, nullptr)) {
        const ASN1_TIME* next = X509_CRL_get0_nextUpdate(crl);
        std::tm tm{};
        if (next && ASN1_TIME_to_tm(next, &tm) == 1) {
            const std::int64_t t = static_cast<std::int64_t>(timegm(&tm));
            if (earliest == 0 || t < earliest) earliest = t;
        }
        X509_CRL_free(crl);
    }
    BIO_free(bio);
    ERR_clear_error();
    return earliest;
}

}  // namespace

CrlPublisher::CrlPublisher(PlayersDb& db, CrlPublisherOptions opts) : db_(db), opts_(std::move(opts)) {
    opts_.next_update_days = std::max(2, opts_.next_update_days);
    opts_.refresh_interval_s = std::max<std::int64_t>(60, opts_.refresh_interval_s);
    opts_.retry_s = std::max<std::int64_t>(1, opts_.retry_s);
    opts_.waiting_retry_s = std::max<std::int64_t>(1, opts_.waiting_retry_s);
    // A list that is refreshed less often than it runs out would shut every screen out between refreshes.
    const std::int64_t most = std::int64_t{opts_.next_update_days} * 86400 / 3;
    if (opts_.refresh_interval_s > most) {
        LOG_WARN("Broker CRL: refresh every {} h is too rare for lists valid {} days — refreshing every {} h",
                 opts_.refresh_interval_s / 3600, opts_.next_update_days, most / 3600);
        opts_.refresh_interval_s = most;
    }
    // Until the first publication the page should still know how long the file on disk is good for.
    status_.valid_until = validUntilOfFile(opts_.path);
}

CrlPublisher::~CrlPublisher() { stop(); }

CrlStatus CrlPublisher::status() const {
    std::lock_guard<std::mutex> lk(mu_);
    return status_;
}

CrlStatus CrlPublisher::publishNow() {
    std::lock_guard<std::mutex> one(publish_mu_);
    const std::int64_t now = opts_.now ? opts_.now() : static_cast<std::int64_t>(std::time(nullptr));
    CrlStatus st = status();
    st.last_attempt_at = now;
    st.last_ok = false;
    st.waiting = false;
    st.error.clear();
    st.reload_error.clear();
    auto finish = [&]() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            status_ = st;
        }
        if (opts_.on_change) opts_.on_change();
        return st;
    };

    const std::vector<int> gens = opts_.generations ? opts_.generations() : std::vector<int>{};
    if (gens.empty()) { st.waiting = true; return finish(); }      // before the device CA ceremony: normal, not a fault

    // Every generation, in one file. If any cannot be signed, nothing is written.
    std::string all;
    std::size_t revoked = 0;
    for (int g : gens) {
        std::string err;
        auto ca = opts_.open_generation ? opts_.open_generation(g, err) : nullptr;
        if (!ca) {
            st.error = "generation_" + std::to_string(g) + ": " + (err.empty() ? "cannot be opened" : err);
            return finish();
        }
        std::vector<tls::RevokedCert> list;
        for (const auto& row : db_.revokedSerials(g, now))
            list.push_back({row.serial_hex, row.revoked_at.value_or(now)});
        const std::int64_t number = db_.nextCrlNumber(g);
        if (number <= 0) { st.error = "crl_number_unavailable"; return finish(); }
        std::string pem;
        if (!ca->generateCrl(list, number, opts_.next_update_days, pem, err, now)) {
            st.error = "generation_" + std::to_string(g) + ": " + err;
            return finish();
        }
        all += pem;
        revoked += list.size();
    }

    // Replace the file in one step: the broker never sees half of it.
    std::error_code ec;
    if (opts_.path.has_parent_path()) fs::create_directories(opts_.path.parent_path(), ec);
    const fs::path tmp = opts_.path.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << all;
        out.flush();
        if (!out) {
            st.error = "cannot_write_file";
            fs::remove(tmp, ec);
            return finish();
        }
    }
    ::chmod(tmp.c_str(), 0644);                               // a revocation list is public
    fs::rename(tmp, opts_.path, ec);
    if (ec) {
        st.error = "cannot_replace_file";
        fs::remove(tmp, ec);
        return finish();
    }

    st.last_publish_at = now;
    st.last_ok = true;
    st.generations = gens.size();
    st.revoked = revoked;
    st.valid_until = now + std::int64_t{opts_.next_update_days} * 86400;
    if (opts_.reload) st.reload_error = opts_.reload();       // the file stands even if the broker could not be told
    return finish();
}

void CrlPublisher::start() {
    if (thread_.joinable()) return;
    {
        std::lock_guard<std::mutex> lk(mu_);
        stopping_ = false;
        wanted_ = true;                                       // publish as soon as it runs
    }
    thread_ = std::thread([this] { run(); });
}

void CrlPublisher::stop() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        stopping_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void CrlPublisher::request() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        wanted_ = true;
    }
    cv_.notify_all();
}

void CrlPublisher::run() {
    std::unique_lock<std::mutex> lk(mu_);
    std::int64_t wait_s = opts_.refresh_interval_s;
    bool was_waiting = false;
    while (!stopping_) {
        cv_.wait_for(lk, std::chrono::seconds(wait_s), [&] { return stopping_ || wanted_; });
        if (stopping_) break;
        wanted_ = false;
        lk.unlock();
        const CrlStatus st = publishNow();
        if (st.waiting) {
            if (!was_waiting) LOG_INFO("Broker CRL: waiting for the device CA (nothing to publish yet)");
        } else if (!st.last_ok) {
            LOG_ERROR("Broker CRL: could not publish — {}", st.error);
        } else {
            if (was_waiting) LOG_INFO("Broker CRL: the device CA is there, the list is published");
            if (!st.reload_error.empty())
                LOG_WARN("Broker CRL: written ({} revoked, {} generation(s)) but the broker was not told: {}",
                         st.revoked, st.generations, st.reload_error);
        }
        was_waiting = st.waiting;
        lk.lock();
        // Soon again while waiting for the CA or after a failure; otherwise the usual refresh, long before any list runs out.
        wait_s = st.waiting ? opts_.waiting_retry_s
               : (!st.last_ok || !st.reload_error.empty()) ? opts_.retry_s : opts_.refresh_interval_s;
    }
}

}  // namespace liveqx::players
