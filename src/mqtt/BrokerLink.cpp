#include "mqtt/BrokerLink.h"

#include "utils/Log.h"

#include <algorithm>
#include <chrono>
#include <ctime>

namespace liveqx::mqtt {
namespace {

std::int64_t nowSeconds() { return static_cast<std::int64_t>(std::time(nullptr)); }

}  // namespace

nlohmann::json brokerStatusJson(const BrokerLinkStatus& st) {
    nlohmann::json j = {{"state", st.state}, {"detail", st.detail},
                        {"last_reconcile_at", st.last_reconcile_at},
                        {"last_reconcile_ok", st.last_reconcile_ok}};
    if (st.last_reconcile_at > 0)
        j["last_report"] = {{"issued", st.last_report.issued}, {"removed", st.last_report.removed},
                            {"failed", st.last_report.failed}, {"error", st.last_report.error}};
    return j;
}

BrokerLink::BrokerLink(BrokerLinkOptions opts) : opts_(std::move(opts)) {
    opts_.retry_s = std::max(1, opts_.retry_s);
    opts_.reconcile_interval_s = std::max(opts_.retry_s, opts_.reconcile_interval_s);
    conn_.host = opts_.host;
    conn_.port = opts_.port;
    conn_.verify_hostname = opts_.verify_hostname;
    conn_.reconcile_interval_s = opts_.reconcile_interval_s;
}

BrokerConnection BrokerLink::connection() const {
    std::lock_guard<std::mutex> lk(mu_);
    return conn_;
}

void BrokerLink::reconfigure(const BrokerConnection& c) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        conn_ = c;
        pending_conn_ = c;
        dirty_ = true;
        poke_ = true;
    }
    cv_.notify_all();
}

BrokerLink::ProbeResult BrokerLink::probe(const BrokerConnection& c) {
    ProbeResult r;
    BrokerIdentityPem id;
    std::string why;
    if (!opts_.identity || !opts_.identity(id, why)) {
        r.error = "not_ready: " + (why.empty() ? std::string("no identity for the broker yet") : why);
        return r;
    }
    const std::string trust = opts_.trust_pem ? opts_.trust_pem() : std::string();
    if (trust.empty()) { r.error = "not_ready: no root to check the broker's certificate against yet"; return r; }

    ClientOptions co;
    co.host            = c.host;
    co.port            = c.port;
    // Another client id: the broker would otherwise drop the running connection
    // in favour of this one. The identity (and so the rights) is the same.
    co.client_id       = std::string(BrokerAdmin::kServerClient) + "-probe";
    co.ca_pem          = trust;
    co.cert_chain_pem  = id.cert_chain_pem;
    co.key_pem         = id.key_pem;
    co.verify_hostname = c.verify_hostname;
    co.clean_start     = true;
    co.session_expiry_s = 0;
    auto client = opts_.make_client ? opts_.make_client(co) : nullptr;
    if (!client) { r.error = "the MQTT client is not available in this build"; return r; }
    if (!client->start()) { r.error = client->lastError(); return r; }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(std::max(1, opts_.probe_timeout_s));
    while (std::chrono::steady_clock::now() < deadline && !client->connected())
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    r.ok = client->connected();
    if (!r.ok) {
        r.error = client->lastError();
        if (r.error.empty()) r.error = "no answer from the broker in time";
    }
    client->stop();
    return r;
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

void BrokerLink::clearRetained(const std::string& topic) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (clear_retained_.size() < 100000) clear_retained_.insert(topic);
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
    for (const auto& f : opts_.subscriptions) client->subscribe(f, 1);
    client->setMessageHandler([raw, sink = opts_.on_message](const Message& m) {
        if (raw->handleMessage(m)) return;                  // a reply to our own administrative call
        if (sink) sink(m);
    });
    client->setStateHandler([this](bool, const std::string&) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            poke_ = true;
        }
        cv_.notify_all();
    });
    if (!client->start()) { why = client->lastError(); return false; }

    {
        std::lock_guard<std::mutex> lk(client_mu_);
        client_ = std::move(client);
        admin_  = std::move(admin);
    }
    serial_ = id.serial;
    was_up_ = false;
    return true;
}

PublishResult BrokerLink::publish(const Message& m) {
    std::lock_guard<std::mutex> lk(client_mu_);
    return client_ ? client_->publish(m) : PublishResult::NotConnected;
}

void BrokerLink::tearDown() {
    std::lock_guard<std::mutex> lk(client_mu_);
    if (client_) client_->stop();          // joins its threads before the administrator goes
    admin_.reset();
    client_.reset();
    was_up_ = false;
    serial_.clear();
}

void BrokerLink::run() {
    while (!stopping_.load()) {
        // A new address / setting was asked for: close the old connection and open the new.
        {
            std::optional<BrokerConnection> fresh;
            {
                std::lock_guard<std::mutex> lk(mu_);
                fresh.swap(pending_conn_);
            }
            if (fresh) {
                opts_.host = fresh->host;
                opts_.port = fresh->port;
                opts_.verify_hostname = fresh->verify_hostname;
                opts_.reconcile_interval_s = std::max(opts_.retry_s, fresh->reconcile_interval_s);
                LOG_INFO("Broker: now connecting to {}:{}", opts_.host, opts_.port);
                tearDown();
            }
        }
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

        if (up) {
            std::set<std::string> wipe;
            {
                std::lock_guard<std::mutex> lk(mu_);
                wipe.swap(clear_retained_);
            }
            for (const auto& topic : wipe) {
                Message m;
                m.topic  = topic;
                m.retain = true;
                m.qos    = 1;
                if (client_->publish(m) != PublishResult::Ok) {      // try again later
                    std::lock_guard<std::mutex> lk(mu_);
                    clear_retained_.insert(topic);
                }
            }
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
