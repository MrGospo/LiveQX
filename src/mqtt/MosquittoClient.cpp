#include "mqtt/MosquittoClient.h"

#include "utils/Log.h"

#include <mosquitto.h>
#include <mqtt_protocol.h>

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <algorithm>
#include <chrono>
#include <cstring>

namespace liveqx::mqtt {
namespace {

// libmosquitto's global state is initialised once for all clients.
std::mutex g_lib_mu;
int        g_lib_users = 0;

void libAcquire() {
    std::lock_guard<std::mutex> lk(g_lib_mu);
    if (g_lib_users++ == 0) mosquitto_lib_init();
}
void libRelease() {
    std::lock_guard<std::mutex> lk(g_lib_mu);
    if (--g_lib_users == 0) mosquitto_lib_cleanup();
}

struct SslCtxDel { void operator()(SSL_CTX* c) const noexcept { SSL_CTX_free(c); } };
using SslCtx = std::unique_ptr<SSL_CTX, SslCtxDel>;

struct BioDel { void operator()(BIO* b) const noexcept { BIO_free_all(b); } };
using BioPtr = std::unique_ptr<BIO, BioDel>;

BioPtr memBio(const std::string& s) {
    return BioPtr(BIO_new_mem_buf(s.data(), static_cast<int>(s.size())));
}

// The TLS context, built from PEM held in memory: the roots we trust for the
// broker, our own certificate chain and our private key.
SslCtx buildContext(const ClientOptions& o, std::string& err) {
    SslCtx ctx(SSL_CTX_new(TLS_client_method()));
    if (!ctx) { err = "cannot create a TLS context"; return nullptr; }
    SSL_CTX_set_min_proto_version(ctx.get(), TLS1_2_VERSION);
    SSL_CTX_set_verify(ctx.get(), SSL_VERIFY_PEER, nullptr);

    int roots = 0;
    {
        BioPtr b = memBio(o.ca_pem);
        X509_STORE* store = SSL_CTX_get_cert_store(ctx.get());
        while (X509* c = PEM_read_bio_X509(b.get(), nullptr, nullptr, nullptr)) {
            X509_STORE_add_cert(store, c);
            X509_free(c);
            ++roots;
        }
        ERR_clear_error();                       // the end of the bundle raises a benign error
    }
    if (roots == 0) { err = "no usable CA certificate to verify the broker with"; return nullptr; }

    {
        BioPtr b = memBio(o.cert_chain_pem);
        X509* leaf = PEM_read_bio_X509(b.get(), nullptr, nullptr, nullptr);
        if (!leaf) { ERR_clear_error(); err = "the client certificate is not readable"; return nullptr; }
        const int ok = SSL_CTX_use_certificate(ctx.get(), leaf);
        X509_free(leaf);
        if (ok != 1) { ERR_clear_error(); err = "the client certificate is not usable"; return nullptr; }
        while (X509* extra = PEM_read_bio_X509(b.get(), nullptr, nullptr, nullptr)) {
            if (SSL_CTX_add_extra_chain_cert(ctx.get(), extra) != 1) {     // takes ownership on success
                X509_free(extra);
                ERR_clear_error();
                err = "an intermediate certificate is not usable";
                return nullptr;
            }
        }
        ERR_clear_error();
    }
    {
        BioPtr b = memBio(o.key_pem);
        EVP_PKEY* key = PEM_read_bio_PrivateKey(b.get(), nullptr, nullptr, nullptr);
        if (!key) { ERR_clear_error(); err = "the client private key is not readable"; return nullptr; }
        const int ok = SSL_CTX_use_PrivateKey(ctx.get(), key);
        EVP_PKEY_free(key);
        if (ok != 1 || SSL_CTX_check_private_key(ctx.get()) != 1) {
            ERR_clear_error();
            err = "the client private key does not match its certificate";
            return nullptr;
        }
    }
    return ctx;
}

PublishResult mapPublishError(int rc) {
    switch (rc) {
        case MOSQ_ERR_SUCCESS:        return PublishResult::Ok;
        case MOSQ_ERR_NO_CONN:        return PublishResult::NotConnected;
        case MOSQ_ERR_INVAL:
        case MOSQ_ERR_PAYLOAD_SIZE:
        case MOSQ_ERR_MALFORMED_UTF8:
        case MOSQ_ERR_QOS_NOT_SUPPORTED:
        case MOSQ_ERR_OVERSIZE_PACKET:
        case MOSQ_ERR_NOT_SUPPORTED:  return PublishResult::Rejected;
        default:                      return PublishResult::Failed;
    }
}

}  // namespace

// libmosquitto calls plain C functions; they find us through the user pointer.
struct MosquittoClient::Callbacks {
    static void connect(mosquitto*, void* self, int reason, int, const mosquitto_property*) {
        static_cast<MosquittoClient*>(self)->onConnect(reason);
    }
    static void disconnect(mosquitto*, void* self, int reason, const mosquitto_property*) {
        static_cast<MosquittoClient*>(self)->onDisconnect(reason);
    }
    static void message(mosquitto*, void* self, const mosquitto_message* m, const mosquitto_property*) {
        auto* c = static_cast<MosquittoClient*>(self);
        MessageHandler h;
        {
            std::lock_guard<std::mutex> lk(c->mu_);
            h = c->on_message_;
        }
        if (!h || !m || !m->topic) return;
        Message msg;
        msg.topic   = m->topic;
        msg.payload.assign(static_cast<const char*>(m->payload), static_cast<std::size_t>(m->payloadlen > 0 ? m->payloadlen : 0));
        msg.qos     = m->qos;
        msg.retain  = m->retain;
        h(msg);
    }
    static void log(mosquitto*, void* self, int level, const char* text) {
        if (!text) return;
        if (level == MOSQ_LOG_ERR || level == MOSQ_LOG_WARNING)
            static_cast<MosquittoClient*>(self)->setError(text);
    }
};

MosquittoClient::MosquittoClient(ClientOptions opts) : opts_(std::move(opts)) { libAcquire(); }

MosquittoClient::~MosquittoClient() {
    stop();
    libRelease();
}

void MosquittoClient::setMessageHandler(MessageHandler h) {
    std::lock_guard<std::mutex> lk(mu_);
    on_message_ = std::move(h);
}

void MosquittoClient::setStateHandler(StateHandler h) {
    std::lock_guard<std::mutex> lk(mu_);
    on_state_ = std::move(h);
}

std::string MosquittoClient::lastError() const {
    std::lock_guard<std::mutex> lk(mu_);
    return last_error_;
}

void MosquittoClient::setError(std::string e) {
    std::lock_guard<std::mutex> lk(mu_);
    last_error_ = std::move(e);
}

void MosquittoClient::notifyState(bool up, const std::string& detail) {
    StateHandler h;
    {
        std::lock_guard<std::mutex> lk(mu_);
        h = on_state_;
    }
    if (h) h(up, detail);
}

bool MosquittoClient::start() {
    if (started_.load()) return true;
    if (opts_.client_id.empty() || opts_.host.empty() || opts_.port <= 0 || opts_.port > 65535) {
        setError("the broker address or client id is missing");
        return false;
    }
    std::string err;
    SslCtx ctx = buildContext(opts_, err);
    if (!ctx) { setError(err); return false; }

    mosq_ = mosquitto_new(opts_.client_id.c_str(), opts_.clean_start, this);
    if (!mosq_) { setError("cannot create the MQTT client"); return false; }
    mosquitto_int_option(mosq_, MOSQ_OPT_PROTOCOL_VERSION, MQTT_PROTOCOL_V5);
    // The library keeps its own reference to the context; we drop ours below.
    SSL_CTX_up_ref(ctx.get());
    mosquitto_void_option(mosq_, MOSQ_OPT_SSL_CTX, ctx.get());
    mosquitto_int_option(mosq_, MOSQ_OPT_SSL_CTX_WITH_DEFAULTS, 0);   // our settings, as built
    mosquitto_tls_insecure_set(mosq_, !opts_.verify_hostname);
    mosquitto_reconnect_delay_set(mosq_, std::max(1, opts_.reconnect_min_s),
                                  std::max(opts_.reconnect_min_s, opts_.reconnect_max_s), true);
    mosquitto_connect_v5_callback_set(mosq_, &Callbacks::connect);
    mosquitto_disconnect_v5_callback_set(mosq_, &Callbacks::disconnect);
    mosquitto_message_v5_callback_set(mosq_, &Callbacks::message);
    mosquitto_log_callback_set(mosq_, &Callbacks::log);
    if (opts_.will) {
        const auto& w = *opts_.will;
        mosquitto_will_set_v5(mosq_, w.topic.c_str(), static_cast<int>(w.payload.size()), w.payload.data(),
                              w.qos, w.retain, nullptr);
    }

    stopping_ = false;
    started_  = true;
    connector_ = std::thread([this] { connectLoop(); });
    return true;
}

void MosquittoClient::connectLoop() {
    mosquitto_property* props = nullptr;
    mosquitto_property_add_int32(&props, MQTT_PROP_SESSION_EXPIRY_INTERVAL, opts_.session_expiry_s);

    int pause = std::max(1, opts_.reconnect_min_s);
    while (!stopping_.load()) {
        const int rc = mosquitto_connect_bind_v5(mosq_, opts_.host.c_str(), opts_.port, opts_.keepalive_s,
                                                 nullptr, props);
        if (rc == MOSQ_ERR_SUCCESS) {
            mosquitto_loop_start(mosq_);         // from here the library keeps the session alive
            mosquitto_property_free_all(&props);
            return;
        }
        std::string why = rc == MOSQ_ERR_TLS     ? "TLS handshake failed (certificate rejected or the name does not match)"
                        : rc == MOSQ_ERR_ERRNO   ? std::string("cannot connect: ") + std::strerror(errno)
                                                 : std::string("cannot connect: ") + mosquitto_strerror(rc);
        setError(why);
        notifyState(false, why);
        for (int waited = 0; waited < pause * 10 && !stopping_.load(); ++waited)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        pause = std::min(pause * 2, std::max(opts_.reconnect_min_s, opts_.reconnect_max_s));
    }
    mosquitto_property_free_all(&props);
}

void MosquittoClient::onConnect(int reason) {
    if (reason != 0) {
        const std::string why = std::string("the broker refused the connection: ") +
                                (reason <= 5 ? mosquitto_connack_string(reason) : mosquitto_reason_string(reason));
        connected_ = false;
        setError(why);
        notifyState(false, why);
        return;
    }
    connected_ = true;
    std::vector<std::pair<std::string, int>> subs;
    {
        std::lock_guard<std::mutex> lk(mu_);
        subs = subscriptions_;
        last_error_.clear();
    }
    for (const auto& [filter, qos] : subs)
        mosquitto_subscribe_v5(mosq_, nullptr, filter.c_str(), qos, 0, nullptr);
    notifyState(true, "");
}

void MosquittoClient::onDisconnect(int reason) {
    connected_ = false;
    if (stopping_.load()) return;
    const std::string why = reason == 0 ? "disconnected"
                                        : std::string("connection lost: ") + mosquitto_strerror(reason);
    notifyState(false, why);
}

void MosquittoClient::stop() {
    if (!started_.exchange(false)) return;
    stopping_ = true;
    if (connector_.joinable()) connector_.join();
    if (mosq_) {
        mosquitto_disconnect(mosq_);
        mosquitto_loop_stop(mosq_, true);
        mosquitto_destroy(mosq_);
        mosq_ = nullptr;
    }
    connected_ = false;
}

PublishResult MosquittoClient::publish(const Message& m) {
    if (m.topic.empty() || m.topic.find_first_of("+#") != std::string::npos ||
        m.topic.find('\0') != std::string::npos || m.qos < 0 || m.qos > 2)
        return PublishResult::Rejected;
    if (!connected_.load() || !mosq_) return PublishResult::NotConnected;

    mosquitto_property* props = nullptr;
    if (m.expiry_s) mosquitto_property_add_int32(&props, MQTT_PROP_MESSAGE_EXPIRY_INTERVAL, *m.expiry_s);
    const int rc = mosquitto_publish_v5(mosq_, nullptr, m.topic.c_str(), static_cast<int>(m.payload.size()),
                                        m.payload.data(), m.qos, m.retain, props);
    mosquitto_property_free_all(&props);
    return mapPublishError(rc);
}

void MosquittoClient::subscribe(const std::string& filter, int qos) {
    if (mosquitto_sub_topic_check(filter.c_str()) != MOSQ_ERR_SUCCESS) return;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = std::find_if(subscriptions_.begin(), subscriptions_.end(),
                               [&](const auto& s) { return s.first == filter; });
        if (it == subscriptions_.end()) subscriptions_.emplace_back(filter, qos);
        else it->second = qos;
    }
    if (connected_.load() && mosq_) mosquitto_subscribe_v5(mosq_, nullptr, filter.c_str(), qos, 0, nullptr);
}

}  // namespace liveqx::mqtt
