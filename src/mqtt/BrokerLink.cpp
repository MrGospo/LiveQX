#include "mqtt/BrokerLink.h"

#include "utils/Log.h"

#include <algorithm>
#include <chrono>
#include <ctime>

namespace liveqx::mqtt {
namespace {

std::int64_t nowSeconds() { return static_cast<std::int64_t>(std::time(nullptr)); }

}  // namespace

BrokerLink::BrokerLink(BrokerLinkOptions opts) : opts_(std::move(opts)) {
    opts_.retry_s = std::max(1, opts_.retry_s);
    opts_.reconcile_interval_s = std::max(opts_.retry_s, opts_.reconcile_interval_s);
}

BrokerLink::~BrokerLink() { stop(); }

void BrokerLink::start() {
    if (thread_.joinable()) return;
    stopping_ = false;
    thread_ = std::thread([this] { run(); });
}

void BrokerLink::stop() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        stopping_ = true;
        poke_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    tearDown();
    setStatus("stopped", "");
}

void BrokerLink::requestSync() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        dirty_ = true;
        poke_ = true;
    }
    cv_.notify_all();
}

BrokerLinkStatus BrokerLink::status() const {
    std::lock_guard<std::mutex> lk(mu_);
    return status_;
}

void BrokerLink::setStatus(const std::string& state, const std::string& detail) {
    std::lock_guard<std::mutex> lk(mu_);
    status_.state  = state;
    status_.detail = detail;
}

void BrokerLink::wait(int seconds) {
    std::unique_lock<std::mutex> lk(mu_);
    cv_.wait_for(lk, std::chrono::seconds(std::max(1, seconds)), [&] { return poke_ || stopping_.load(); });
    poke_ = false;
}

bool BrokerLink::bringUp(std::string& why) {
    BrokerIdentityPem id;
    if (!opts_.identity || !opts_.identity(id, why)) {
        if (why.empty()) why = "the server's identity for the broker is not available yet";
        return false;
    }
    const std::string trust = opts_.trust_pem ? opts_.trust_pem() : std::string();
    if (trust.empty()) { why = "no root to check the broker's certificate against yet"; return false; }

    ClientOptions co;
    co.host            = opts_.host;
    co.port            = opts_.port;
    co.client_id       = BrokerAdmin::kServerClient;
    co.ca_pem          = trust;
    co.cert_chain_pem  = id.cert_chain_pem;
    co.key_pem         = id.key_pem;
    co.verify_hostname = opts_.verify_hostname;

    auto client = opts_.make_client ? opts_.make_client(co) : nullptr;
    if (!client) { why = "the MQTT client is not available in this build"; return false; }
    auto admin = std::make_unique<BrokerAdmin>(*client, opts_.admin);    // subscribes to replies
    BrokerAdmin* raw = admin.get();
    client->setMessageHandler([raw](const Message& m) { raw->handleMessage(m); });
    client->setStateHandler([this](bool, const std::string&) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            poke_ = true;
        }
        cv_.notify_all();
    });
    if (!client->start()) { why = client->lastError(); return false; }

    client_ = std::move(client);
    admin_  = std::move(admin);
    serial_ = id.serial;
    was_up_ = false;
    return true;
}

void BrokerLink::tearDown() {
    if (client_) client_->stop();          // joins its threads before the administrator goes
    admin_.reset();
    client_.reset();
    was_up_ = false;
    serial_.clear();
}

void BrokerLink::run() {
    while (!stopping_.load()) {
        if (!client_) {
            std::string why;
            if (!bringUp(why)) {
                setStatus("waiting", why);
                wait(opts_.retry_s);
                continue;
            }
            setStatus("connecting", "");
            next_tick_ = nowSeconds() + opts_.reconcile_interval_s;
        }

        const std::int64_t now = nowSeconds();
        if (now >= next_tick_) {
            next_tick_ = now + opts_.reconcile_interval_s;
            BrokerIdentityPem id;
            std::string why;
            if (opts_.identity && opts_.identity(id, why) && id.serial != serial_) {
                LOG_INFO("Broker: the server's identity was re-issued, reconnecting");
                tearDown();
                continue;
            }
            std::lock_guard<std::mutex> lk(mu_);
            dirty_ = true;                 // the timer
        }

        const bool up = client_->connected();
        if (up && !was_up_) {
            LOG_INFO("Broker: connected to {}:{}", opts_.host, opts_.port);
            setStatus("connected", "");
            std::lock_guard<std::mutex> lk(mu_);
            dirty_ = true;
        } else if (!up && was_up_) {
            LOG_WARN("Broker: connection lost");
            setStatus("connecting", client_->lastError());
        } else if (!up) {
            setStatus("connecting", client_->lastError());
        }
        was_up_ = up;

        bool due = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            due = up && dirty_;
            if (due) dirty_ = false;
        }
        if (due) {
            const ReconcileReport rep = admin_->reconcile(opts_.wanted ? opts_.wanted() : std::set<std::string>{});
            {
                std::lock_guard<std::mutex> lk(mu_);
                status_.last_reconcile_at = nowSeconds();
                status_.last_reconcile_ok = rep.ok;
                status_.last_report       = rep;
                if (!rep.ok) {
                    dirty_ = true;                // try again soon
                    status_.detail = rep.error;
                }
            }
            if (rep.issued || rep.removed || !rep.ok)
                LOG_INFO("Broker: reconciled — {} issued, {} removed, {} failed{}{}", rep.issued, rep.removed,
                         rep.failed, rep.error.empty() ? "" : ": ", rep.error);
        }

        bool again = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            again = dirty_ && up;
        }
        if (again) { wait(opts_.retry_s); continue; }
        const std::int64_t until_tick = std::max<std::int64_t>(1, next_tick_ - nowSeconds());
        wait(up ? static_cast<int>(std::min<std::int64_t>(until_tick, opts_.reconcile_interval_s)) : opts_.retry_s);
    }
}

}  // namespace liveqx::mqtt
