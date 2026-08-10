#include "mounts/RpcProtocol.h"

#include <cerrno>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>

#include <nlohmann/json.hpp>

namespace liveqx::mounts {

const char* toString(RpcOp op) noexcept {
    switch (op) {
        case RpcOp::ApplyMount:  return "ApplyMount";
        case RpcOp::RemoveMount: return "RemoveMount";
        case RpcOp::TestMount:   return "TestMount";
        case RpcOp::Status:      return "Status";
    }
    return "Status";
}

bool rpcOpFromString(std::string_view s, RpcOp& out) noexcept {
    if (s == "ApplyMount")  { out = RpcOp::ApplyMount;  return true; }
    if (s == "RemoveMount") { out = RpcOp::RemoveMount; return true; }
    if (s == "TestMount")   { out = RpcOp::TestMount;   return true; }
    if (s == "Status")      { out = RpcOp::Status;      return true; }
    return false;
}

RpcResponse RpcResponse::okWith(std::string status, nlohmann::json extra) {
    RpcResponse r;
    r.ok     = true;
    r.status = std::move(status);
    r.extra  = std::move(extra);
    return r;
}

RpcResponse RpcResponse::fail(std::string error, std::string status) {
    RpcResponse r;
    r.ok     = false;
    r.status = std::move(status);
    r.error  = std::move(error);
    return r;
}

nlohmann::json RpcResponse::toJson() const {
    nlohmann::json j = {
        {"ok",     ok},
        {"status", status},
    };
    if (!error.empty()) j["error"] = error;
    if (extra.is_object() && !extra.empty()) {
        for (auto& [k, v] : extra.items()) j[k] = v;
    }
    return j;
}

RpcResponse RpcResponse::fromJson(const nlohmann::json& j) {
    RpcResponse r;
    r.ok     = j.value("ok",     false);
    r.status = j.value("status", std::string{});
    r.error  = j.value("error",  std::string{});
    // Перекидываем все «лишние» поля в extra, чтобы вызывающий мог
    // прочитать file_count / units / etc.
    if (j.is_object()) {
        nlohmann::json e = nlohmann::json::object();
        for (auto& [k, v] : j.items()) {
            if (k == "ok" || k == "status" || k == "error") continue;
            e[k] = v;
        }
        r.extra = std::move(e);
    }
    return r;
}

std::vector<std::uint8_t> encodeFrame(const nlohmann::json& body) {
    const std::string s = body.dump();
    const auto n = s.size();
    std::vector<std::uint8_t> buf(4 + n);
    const std::uint32_t be = htonl(static_cast<std::uint32_t>(n));
    std::memcpy(buf.data(),     &be, 4);
    std::memcpy(buf.data() + 4, s.data(), n);
    return buf;
}

namespace {

// Полное чтение N байт из blocking-сокета. EINTR — повтор. Возвращает
// false при EOF или ошибке. Контракт: out_error не очищаем, только
// дописываем при провале.
bool readExactly(int fd, void* dst, std::size_t n, std::string& out_error) {
    auto* p = static_cast<std::uint8_t*>(dst);
    while (n > 0) {
        ssize_t r = ::read(fd, p, n);
        if (r > 0) { p += r; n -= static_cast<std::size_t>(r); continue; }
        if (r == 0) {
            out_error = "peer closed connection mid-frame";
            return false;
        }
        if (errno == EINTR) continue;
        out_error = std::string("read: ") + std::strerror(errno);
        return false;
    }
    return true;
}

bool writeExactly(int fd, const void* src, std::size_t n, std::string& out_error) {
    auto* p = static_cast<const std::uint8_t*>(src);
    while (n > 0) {
        ssize_t w = ::write(fd, p, n);
        if (w > 0) { p += w; n -= static_cast<std::size_t>(w); continue; }
        if (w < 0 && errno == EINTR) continue;
        out_error = std::string("write: ") + std::strerror(errno);
        return false;
    }
    return true;
}

}  // namespace

bool readFrame(int fd, nlohmann::json& out_body, std::string& out_error) {
    std::uint32_t be = 0;
    if (!readExactly(fd, &be, 4, out_error)) return false;
    const std::uint32_t len = ntohl(be);
    if (len == 0 || len > kMaxFrameBytes) {
        out_error = "frame length out of bounds: " + std::to_string(len);
        return false;
    }
    std::string body;
    body.resize(len);
    if (!readExactly(fd, body.data(), len, out_error)) return false;
    try {
        out_body = nlohmann::json::parse(body);
    } catch (const std::exception& e) {
        out_error = std::string("json parse: ") + e.what();
        return false;
    }
    return true;
}

bool writeFrame(int fd, const nlohmann::json& body, std::string& out_error) {
    const auto frame = encodeFrame(body);
    if (frame.size() > 4 + kMaxFrameBytes) {
        out_error = "frame too large to send";
        return false;
    }
    return writeExactly(fd, frame.data(), frame.size(), out_error);
}

}  // namespace liveqx::mounts
