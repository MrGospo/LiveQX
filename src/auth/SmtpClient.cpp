#include "auth/SmtpClient.h"

#include <chrono>
#include <cstring>
#include <sstream>

#include <curl/curl.h>

#include "utils/Log.h"

namespace liveqx::auth {

namespace {

struct Reader {
    const std::string* payload;
    std::size_t        offset;
};

std::size_t readCallback(char* dst, std::size_t size, std::size_t nmemb,
                         void* userdata) {
    auto* r = static_cast<Reader*>(userdata);
    const std::size_t cap   = size * nmemb;
    const std::size_t avail = r->payload->size() - r->offset;
    const std::size_t take  = (avail < cap) ? avail : cap;
    if (take == 0) return 0;
    std::memcpy(dst, r->payload->data() + r->offset, take);
    r->offset += take;
    return take;
}

std::int64_t monotonicMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(
        steady_clock::now().time_since_epoch()).count();
}

const char* securityName(SmtpSecurity s) {
    switch (s) {
        case SmtpSecurity::None:     return "none";
        case SmtpSecurity::StartTls: return "starttls";
        case SmtpSecurity::Tls:      return "tls";
    }
    return "?";
}

}  // namespace

SmtpClient::SmtpClient(SmtpConfig cfg) : cfg_(std::move(cfg)) {}

std::string SmtpClient::buildUri(const SmtpConfig& cfg) {
    std::uint16_t port = cfg.port;
    if (port == 0) {
        switch (cfg.security) {
            case SmtpSecurity::None:     port = 25;  break;
            case SmtpSecurity::StartTls: port = 587; break;
            case SmtpSecurity::Tls:      port = 465; break;
        }
    }
    const char* scheme =
        (cfg.security == SmtpSecurity::Tls) ? "smtps" : "smtp";
    return std::string(scheme) + "://" + cfg.server + ":" +
           std::to_string(port);
}

std::string SmtpClient::formatFromHeader(const SmtpConfig& cfg) {
    if (cfg.from_name.empty()) return cfg.from_email;
    // Нужны кавычки если в имени есть спец-символы RFC 5322 §3.4 (",", ":",
    // "<", ">", "@", "(", ")", ".", quotes). Жадно ставим кавычки всегда —
    // это валидный RFC-вариант и проще, чем определять corner-case'ы.
    std::string esc;
    esc.reserve(cfg.from_name.size() + 2);
    for (char c : cfg.from_name) {
        if (c == '"' || c == '\\') esc.push_back('\\');
        esc.push_back(c);
    }
    return "\"" + esc + "\" <" + cfg.from_email + ">";
}

bool SmtpClient::isPlausibleEmail(std::string_view s) {
    if (s.empty() || s.size() > 254) return false;
    int at = 0;
    for (char c : s) {
        if (c == '\r' || c == '\n' || c == ' ') return false;
        if (c == '@') ++at;
    }
    if (at != 1) return false;
    if (s.front() == '@' || s.back() == '@') return false;
    return true;
}

std::string SmtpClient::validate(const SmtpConfig& cfg) {
    if (cfg.server.empty()) return "server is empty";
    if (cfg.timeout_sec < 1 || cfg.timeout_sec > 300)
        return "timeout_sec out of range [1..300]";
    if (cfg.from_email.empty()) return "from_email is empty";
    if (!isPlausibleEmail(cfg.from_email)) return "from_email is malformed";
    // username и password должны быть оба заданы или оба пустые. Один
    // без другого — конфигурационная ошибка.
    const bool has_user = !cfg.username.empty();
    const bool has_pw   = !cfg.password.empty();
    if (has_user != has_pw) return "username and password must be both set or both empty";
    return {};
}

std::string SmtpClient::buildPayload(const SmtpConfig& cfg,
                                     std::string_view  to,
                                     std::string_view  subject,
                                     std::string_view  body) {
    std::ostringstream os;
    os << "To: "      << to               << "\r\n"
       << "From: "    << formatFromHeader(cfg) << "\r\n"
       << "Subject: " << subject          << "\r\n"
       << "MIME-Version: 1.0\r\n"
       << "Content-Type: text/plain; charset=UTF-8\r\n"
       << "\r\n"
       << body;
    return os.str();
}

SmtpClient::SendResult SmtpClient::send(std::string_view to,
                                        std::string_view subject,
                                        std::string_view body) {
    SendResult out;
    if (auto err = validate(cfg_); !err.empty()) {
        out.error = "invalid_config: " + err;
        return out;
    }
    if (!isPlausibleEmail(to)) {
        out.error = "invalid recipient: " + std::string(to);
        return out;
    }
    // CR/LF в subject ломает структуру письма + позволяет header
    // injection. Запрещаем.
    for (char c : subject) {
        if (c == '\r' || c == '\n') {
            out.error = "subject contains CR/LF";
            return out;
        }
    }

    const auto t0 = monotonicMs();

    CURL* curl = curl_easy_init();
    if (!curl) {
        out.error = "curl_easy_init failed";
        return out;
    }

    const auto uri = buildUri(cfg_);
    curl_easy_setopt(curl, CURLOPT_URL, uri.c_str());
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, long(cfg_.timeout_sec));
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        long(cfg_.timeout_sec));

    if (cfg_.security == SmtpSecurity::StartTls) {
        curl_easy_setopt(curl, CURLOPT_USE_SSL, long(CURLUSESSL_ALL));
    } else if (cfg_.security == SmtpSecurity::Tls) {
        curl_easy_setopt(curl, CURLOPT_USE_SSL, long(CURLUSESSL_ALL));
    }
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

    if (!cfg_.username.empty()) {
        curl_easy_setopt(curl, CURLOPT_USERNAME, cfg_.username.c_str());
        curl_easy_setopt(curl, CURLOPT_PASSWORD, cfg_.password.c_str());
    }

    curl_easy_setopt(curl, CURLOPT_MAIL_FROM, cfg_.from_email.c_str());

    struct curl_slist* rcpt = nullptr;
    const auto to_str = std::string(to);
    rcpt = curl_slist_append(rcpt, to_str.c_str());
    curl_easy_setopt(curl, CURLOPT_MAIL_RCPT, rcpt);

    const std::string payload = buildPayload(cfg_, to, subject, body);
    Reader reader{&payload, 0};
    curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(curl, CURLOPT_READFUNCTION, readCallback);
    curl_easy_setopt(curl, CURLOPT_READDATA,     &reader);

    char errbuf[CURL_ERROR_SIZE]{};
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);

    const CURLcode rc = curl_easy_perform(curl);
    out.latency_ms = monotonicMs() - t0;
    if (rc == CURLE_OK) {
        out.ok = true;
    } else {
        const char* e = errbuf[0] ? errbuf : curl_easy_strerror(rc);
        out.error = "smtp send failed: ";
        out.error += e;
        LOG_WARN("SmtpClient: send to {} via {} ({}) failed: {}",
                 std::string(to), uri, securityName(cfg_.security), out.error);
    }

    curl_slist_free_all(rcpt);
    curl_easy_cleanup(curl);
    return out;
}

SmtpClient::PingResult SmtpClient::ping() {
    PingResult out;
    if (auto err = validate(cfg_); !err.empty()) {
        out.error = "invalid_config: " + err;
        return out;
    }

    const auto t0 = monotonicMs();
    CURL* curl = curl_easy_init();
    if (!curl) {
        out.error = "curl_easy_init failed";
        return out;
    }

    const auto uri = buildUri(cfg_);
    curl_easy_setopt(curl, CURLOPT_URL, uri.c_str());
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, long(cfg_.timeout_sec));
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        long(cfg_.timeout_sec));
    if (cfg_.security != SmtpSecurity::None) {
        curl_easy_setopt(curl, CURLOPT_USE_SSL, long(CURLUSESSL_ALL));
    }
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    if (!cfg_.username.empty()) {
        curl_easy_setopt(curl, CURLOPT_USERNAME, cfg_.username.c_str());
        curl_easy_setopt(curl, CURLOPT_PASSWORD, cfg_.password.c_str());
    }
    // CONNECT_ONLY=1 → libcurl делает TCP+TLS+EHLO (если SMTPS) и
    // возвращает управление. Этого достаточно как «жив или нет».
    curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 1L);

    char errbuf[CURL_ERROR_SIZE]{};
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);

    const CURLcode rc = curl_easy_perform(curl);
    out.latency_ms = monotonicMs() - t0;
    if (rc == CURLE_OK) {
        out.ok = true;
    } else {
        const char* e = errbuf[0] ? errbuf : curl_easy_strerror(rc);
        out.error = "smtp ping failed: ";
        out.error += e;
    }
    curl_easy_cleanup(curl);
    return out;
}

}  // namespace liveqx::auth
