// Sealed start server — see auth/SealedServer.h.

#include "auth/SealedServer.h"

#include "auth/MasterKey.h"
#include "utils/Log.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <sodium.h>

#include <algorithm>
#include <chrono>

namespace liveqx::auth {
namespace {

using nlohmann::json;

constexpr std::size_t kMaxBodyBytes = 4096;
constexpr std::size_t kMaxTrackedAddresses = 1024;

// Self-contained page: no external resources, language by the browser.
const char* const kPage = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>LiveQX</title>
<style>
 :root{color-scheme:light dark}
 body{font:16px system-ui,sans-serif;margin:0;min-height:100vh;display:flex;align-items:center;justify-content:center;background:#0f1115;color:#e6e8ec}
 main{width:min(26rem,calc(100vw - 2rem));padding:2rem;border-radius:12px;background:#1a1d24}
 h1{font-size:1.25rem;margin:0 0 .5rem}
 p{margin:.25rem 0 1rem;color:#9aa3b2;line-height:1.4}
 label{display:block;margin-bottom:.25rem;font-size:.85rem;color:#9aa3b2}
 input{width:100%;box-sizing:border-box;padding:.6rem .7rem;border-radius:8px;border:1px solid #333a47;background:#0f1115;color:inherit;font:inherit}
 button{margin-top:1rem;width:100%;padding:.65rem;border:0;border-radius:8px;background:#3b82f6;color:#fff;font:inherit;cursor:pointer}
 button:disabled{opacity:.6;cursor:default}
 #msg{min-height:1.4rem;margin-top:.75rem;font-size:.9rem}
 .err{color:#f87171}.ok{color:#4ade80}
</style></head><body><main>
<h1 id="t"></h1><p id="h"></p>
<form id="f" autocomplete="off">
 <label for="p" id="l"></label>
 <input id="p" type="password" autocomplete="current-password" autofocus required>
 <button id="b" type="submit"></button>
</form><div id="msg" role="status"></div>
</main>
<script>
const T={
 ru:{t:"Служба запечатана",h:"Мастер-ключ защищён парольной фразой. Введите её, чтобы запустить LiveQX.",l:"Парольная фраза",b:"Запустить",wrong:"Неверная фраза.",wait:"Слишком много попыток. Повторите через {n} с.",busy:"Идёт проверка, повторите чуть позже.",start:"Запуск… страница обновится сама.",net:"Нет связи с сервером."},
 en:{t:"The service is sealed",h:"The master key is protected by a passphrase. Enter it to start LiveQX.",l:"Passphrase",b:"Start",wrong:"Wrong passphrase.",wait:"Too many attempts. Try again in {n} s.",busy:"A check is in progress, try again shortly.",start:"Starting… this page will refresh by itself.",net:"Cannot reach the server."}
};
const L=(navigator.language||"en").toLowerCase().startsWith("ru")?"ru":"en", S=T[L];
const $=id=>document.getElementById(id);
document.documentElement.lang=L;
$("t").textContent=S.t;$("h").textContent=S.h;$("l").textContent=S.l;$("b").textContent=S.b;
const say=(s,c)=>{const m=$("msg");m.textContent=s;m.className=c||""};
function poll(){
 fetch("/api/seal/status",{cache:"no-store"}).then(r=>{
  if(r.ok) return r.json().then(()=>setTimeout(poll,1500));
  if(r.status===503) return setTimeout(poll,1500);
  location.reload();
 }).catch(()=>setTimeout(poll,1500));
}
$("f").addEventListener("submit",e=>{
 e.preventDefault();$("b").disabled=true;say("");
 fetch("/api/seal/unlock",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify({passphrase:$("p").value})})
 .then(r=>r.json().catch(()=>({})).then(j=>({s:r.status,j})))
 .then(({s,j})=>{
  if(s===200){$("p").value="";say(S.start,"ok");poll();return}
  $("b").disabled=false;
  if(s===429)say(j.error==="busy"?S.busy:S.wait.replace("{n}",j.retry_after||"?"),"err");
  else if(s===401)say(S.wrong,"err");
  else say(S.net,"err");
 }).catch(()=>{$("b").disabled=false;say(S.net,"err")});
});
</script></body></html>)HTML";

void setCommonHeaders(httplib::Response& res) {
    res.set_header("Cache-Control", "no-store");
    res.set_header("X-Content-Type-Options", "nosniff");
    res.set_header("X-Frame-Options", "DENY");
    res.set_header("Referrer-Policy", "no-referrer");
}

void sendJson(httplib::Response& res, int status, const json& body) {
    setCommonHeaders(res);
    res.status = status;
    res.set_content(body.dump(), "application/json");
}

}  // namespace

// ─── UnlockLimiter ──────────────────────────────────────────────────────────
UnlockLimiter::UnlockLimiter(Clock now, int free_attempts, int max_delay_s,
                             int forget_after_s)
    : now_(std::move(now)),
      free_attempts_(std::max(1, free_attempts)),
      max_delay_s_(max_delay_s),
      forget_after_s_(forget_after_s) {}

std::int64_t UnlockLimiter::nowSec() const {
    if (now_) return now_();
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

void UnlockLimiter::forgetIdle(std::int64_t now) {
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (now - it->second.last_failure > forget_after_s_ &&
            it->second.locked_until <= now)
            it = entries_.erase(it);
        else
            ++it;
    }
    while (entries_.size() > kMaxTrackedAddresses) {
        auto oldest = std::min_element(
            entries_.begin(), entries_.end(),
            [](const auto& a, const auto& b) {
                return a.second.last_failure < b.second.last_failure;
            });
        entries_.erase(oldest);
    }
}

int UnlockLimiter::retryAfter(const std::string& address) {
    std::lock_guard<std::mutex> lk(mu_);
    const auto now = nowSec();
    forgetIdle(now);
    auto it = entries_.find(address);
    if (it == entries_.end()) return 0;
    return it->second.locked_until > now
               ? static_cast<int>(it->second.locked_until - now) : 0;
}

void UnlockLimiter::recordFailure(const std::string& address) {
    std::lock_guard<std::mutex> lk(mu_);
    const auto now = nowSec();
    forgetIdle(now);
    auto& e = entries_[address];
    ++e.failures;
    e.last_failure = now;
    if (e.failures >= free_attempts_) {
        const int shift = std::min(e.failures - free_attempts_ + 1, 30);
        const std::int64_t delay =
            std::min<std::int64_t>(max_delay_s_, std::int64_t{1} << shift);
        e.locked_until = now + delay;
    }
}

void UnlockLimiter::recordSuccess(const std::string& address) {
    std::lock_guard<std::mutex> lk(mu_);
    entries_.erase(address);
}

// ─── SealedServer ───────────────────────────────────────────────────────────
SealedServer::SealedServer(MasterKey& key, Options options)
    : key_(key),
      opts_(std::move(options)),
      limiter_(nullptr, opts_.free_attempts) {}

SealedServer::~SealedServer() { stop(); }

bool SealedServer::start(std::string& err) {
    err.clear();
    if (!opts_.cert_path.empty() && !opts_.key_path.empty()) {
        auto* ssl = new httplib::SSLServer(opts_.cert_path.string().c_str(),
                                           opts_.key_path.string().c_str());
        if (!ssl->is_valid()) {
            delete ssl;
            err = "cannot load the TLS certificate " + opts_.cert_path.string();
            return false;
        }
        server_.reset(ssl);
    } else {
        server_ = std::make_unique<httplib::Server>();
    }
    server_->set_payload_max_length(kMaxBodyBytes);
    // The real control API (httplib defaults) binds this same port right after
    // we release it, and httplib's defaults set SO_REUSEPORT. Sockets left in
    // TIME_WAIT by this server must therefore carry SO_REUSEPORT too, or the
    // real server could not bind for about a minute. SO_REUSEADDR is added for
    // a quick restart. A listener of some other program (without SO_REUSEPORT)
    // still makes bind fail, which is what we want.
    server_->set_socket_options([](socket_t sock) {
        int yes = 1;
        ::setsockopt(sock, SOL_SOCKET, SO_REUSEADDR,
                     reinterpret_cast<const char*>(&yes), sizeof(yes));
#ifdef SO_REUSEPORT
        ::setsockopt(sock, SOL_SOCKET, SO_REUSEPORT,
                     reinterpret_cast<const char*>(&yes), sizeof(yes));
#endif
    });
    installHandlers();

    port_ = opts_.port == 0
                ? server_->bind_to_any_port(opts_.bind)
                : (server_->bind_to_port(opts_.bind, opts_.port) ? opts_.port : -1);
    if (port_ <= 0) {
        err = "cannot bind " + opts_.bind + ":" + std::to_string(opts_.port);
        server_.reset();
        port_ = 0;
        return false;
    }
    thread_ = std::thread([this] { server_->listen_after_bind(); });
    server_->wait_until_ready();
    LOG_INFO("sealed: waiting for the master-key passphrase on {}:{} ({})",
             opts_.bind, port_, opts_.cert_path.empty() ? "http" : "https");
    return true;
}

void SealedServer::installHandlers() {
    // httplib reads a POST body only after the pre-routing handler has run,
    // so the two real endpoints are ordinary routes; the pre-routing handler
    // only answers everything else.
    server_->Get("/api/seal/status",
                 [this](const httplib::Request&, httplib::Response& res) {
        sendJson(res, 200, {{"sealed", !key_.loaded()}});
    });

    server_->Post("/api/seal/unlock",
                  [this](const httplib::Request& req, httplib::Response& res) {
        const std::string ip = req.remote_addr;

        if (const int wait = limiter_.retryAfter(ip); wait > 0) {
            res.set_header("Retry-After", std::to_string(wait));
            sendJson(res, 429, {{"error", "too_many_attempts"},
                                {"retry_after", wait}});
            return;
        }

        json body = json::parse(req.body, nullptr, false);
        if (body.is_discarded() || !body.is_object() ||
            !body.contains("passphrase") || !body["passphrase"].is_string()) {
            sendJson(res, 400, {{"error", "bad_request"}});
            return;
        }
        std::string pass = body["passphrase"].get<std::string>();

        std::unique_lock<std::mutex> lk(unlock_mu_, std::try_to_lock);
        if (!lk.owns_lock()) {
            sodium_memzero(pass.data(), pass.size());
            res.set_header("Retry-After", "1");
            sendJson(res, 429, {{"error", "busy"}, {"retry_after", 1}});
            return;
        }

        if (key_.loaded()) {   // someone else got there first
            sodium_memzero(pass.data(), pass.size());
            sendJson(res, 200, {{"ok", true}});
            return;
        }

        std::string err;
        const bool ok = key_.unlock(pass, err);
        sodium_memzero(pass.data(), pass.size());
        if (ok) {
            limiter_.recordSuccess(ip);
            LOG_INFO("sealed: unlocked by {}", ip);
            {
                std::lock_guard<std::mutex> wl(wait_mu_);
                unlocked_ = true;
            }
            wait_cv_.notify_all();
            sendJson(res, 200, {{"ok", true}});
        } else {
            limiter_.recordFailure(ip);
            LOG_WARN("sealed: failed unlock attempt from {}: {}", ip, err);
            const int wait = limiter_.retryAfter(ip);
            json out = {{"error", "wrong_passphrase"}};
            if (wait > 0) out["retry_after"] = wait;
            sendJson(res, 401, out);
        }
    });

    server_->set_pre_routing_handler(
        [](const httplib::Request& req, httplib::Response& res) {
            using R = httplib::Server::HandlerResponse;

            if ((req.path == "/api/seal/status" && req.method == "GET") ||
                (req.path == "/api/seal/unlock" && req.method == "POST"))
                return R::Unhandled;

            // The page for browsers; JSON "sealed" for anything API-like.
            if (req.method == "GET" && req.path.rfind("/api/", 0) != 0 &&
                req.path != "/healthz") {
                setCommonHeaders(res);
                res.set_header("Content-Security-Policy",
                               "default-src 'none'; style-src 'unsafe-inline'; "
                               "script-src 'unsafe-inline'; connect-src 'self'; "
                               "form-action 'none'; base-uri 'none'");
                res.status = 200;
                res.set_content(kPage, "text/html; charset=utf-8");
                return R::Handled;
            }
            sendJson(res, 503, {{"error", "sealed"}});
            return R::Handled;
        });
}

bool SealedServer::waitUnlocked(const std::function<bool()>& should_abort) {
    std::unique_lock<std::mutex> lk(wait_mu_);
    while (!unlocked_) {
        if (should_abort && should_abort()) return false;
        wait_cv_.wait_for(lk, std::chrono::milliseconds(100));
    }
    return true;
}

void SealedServer::stop() {
    if (server_) server_->stop();
    if (thread_.joinable()) thread_.join();
    server_.reset();
}

}  // namespace liveqx::auth
