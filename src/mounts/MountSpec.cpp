#include "mounts/MountSpec.h"

#include <algorithm>
#include <array>
#include <filesystem>

#include <nlohmann/json.hpp>

namespace liveqx::mounts {

namespace {

// Запрещённые байты во всех «текстовых» полях, попадающих в systemd
// unit-файл или в /proc/mounts. NUL обрывает C-string'и, '\n' и '\r'
// дают unit-injection, ';|`' дают command-injection если строка
// каким-то путём попадёт в shell. Кавычки оставляем — credentials
// файл их экранирует сам.
constexpr std::array<char, 6> kForbiddenBytes{
    '\0', '\r', '\n', ';', '|', '`'
};

bool hasForbidden(std::string_view s) noexcept {
    for (char c : s) {
        for (char fb : kForbiddenBytes) if (c == fb) return true;
    }
    return false;
}

bool nonEmpty(const std::string& s, std::size_t max_len) noexcept {
    return !s.empty() && s.size() <= max_len && !hasForbidden(s);
}

bool isAbsoluteSafe(const std::string& path) noexcept {
    if (path.empty() || path[0] != '/') return false;
    if (hasForbidden(path)) return false;
    // Рантайм-проверка `realpath ⊂ kDefaultMountRoot` живёт в mountd
    // (после чтения с диска). Здесь ловим только синтаксис: запретим
    // `..` и double-slash на этапе парсинга.
    if (path.find("..") != std::string::npos) return false;
    if (path.find("//") != std::string::npos) return false;
    return true;
}

}  // namespace

const char* toString(FsType fs) noexcept {
    switch (fs) {
        case FsType::Cifs: return "cifs";
        case FsType::Nfs:  return "nfs";
    }
    return "cifs";  // unreachable; std::variant был бы нагляднее, но
                    // enum + switch матчится с REST schema проще.
}

std::optional<FsType> fsTypeFromString(std::string_view s) noexcept {
    if (s == "cifs") return FsType::Cifs;
    if (s == "nfs")  return FsType::Nfs;
    return std::nullopt;
}

bool MountSpec::validate(std::string& out_error) const {
    if (!nonEmpty(source, 512)) {
        out_error = "source must be non-empty, ≤512 chars, no control bytes";
        return false;
    }
    if (!isAbsoluteSafe(target)) {
        out_error = "target must be absolute, no '..' / '//' / control bytes";
        return false;
    }
    // options может быть пустой (mountd подставит дефолты). Только
    // санити-проверка длины и запрещённых символов.
    if (options.size() > 2048 || hasForbidden(options)) {
        out_error = "options too long or contains forbidden bytes";
        return false;
    }

    switch (fs_type) {
        case FsType::Cifs: {
            // "//host/share" — минимум: "//a/b" (5 символов).
            if (source.size() < 5 || source[0] != '/' || source[1] != '/') {
                out_error = "cifs source must start with // and include /share";
                return false;
            }
            // Если креды заданы — username обязателен; пустой password
            // допустим (mountd закодирует guest=). Если креды не заданы
            // вовсе — это guest-mount.
            if (cifs.has_value()) {
                if (!nonEmpty(cifs->username, 256)) {
                    out_error = "cifs.username required when creds set";
                    return false;
                }
                if (cifs->password.size() > 256 || hasForbidden(cifs->password)) {
                    out_error = "cifs.password too long or contains forbidden bytes";
                    return false;
                }
                if (cifs->domain.size() > 64 || hasForbidden(cifs->domain)) {
                    out_error = "cifs.domain too long or contains forbidden bytes";
                    return false;
                }
            }
            break;
        }
        case FsType::Nfs: {
            // "host:/path" — двоеточие обязательно.
            const auto colon = source.find(':');
            if (colon == std::string::npos || colon == 0
                || colon + 1 >= source.size() || source[colon + 1] != '/') {
                out_error = "nfs source must look like host:/exported/path";
                return false;
            }
            if (cifs.has_value()) {
                out_error = "nfs mount cannot carry cifs credentials";
                return false;
            }
            break;
        }
    }
    return true;
}

nlohmann::json MountSpec::toJson(bool include_password) const {
    nlohmann::json j = {
        {"id",      id},
        {"fs_type", toString(fs_type)},
        {"source",  source},
        {"target",  target},
        {"options", options},
        {"ro",      ro},
    };
    if (cifs.has_value()) {
        nlohmann::json c = {
            {"username", cifs->username},
        };
        if (!cifs->domain.empty()) c["domain"] = cifs->domain;
        if (include_password)      c["password"] = cifs->password;
        j["cifs"] = std::move(c);
    }
    return j;
}

std::optional<MountSpec> MountSpec::fromJson(const nlohmann::json& j,
                                             std::string& out_error) {
    if (!j.is_object()) {
        out_error = "spec must be a JSON object";
        return std::nullopt;
    }

    MountSpec s;
    if (j.contains("id") && j["id"].is_number_integer()) {
        s.id = j["id"].get<std::int64_t>();
    }

    auto fs_str = j.value("fs_type", std::string{});
    auto fs     = fsTypeFromString(fs_str);
    if (!fs) {
        out_error = "fs_type must be 'cifs' or 'nfs'";
        return std::nullopt;
    }
    s.fs_type = *fs;
    s.source  = j.value("source",  std::string{});
    s.target  = j.value("target",  std::string{});
    s.options = j.value("options", std::string{});
    s.ro      = j.value("ro",      true);

    if (j.contains("cifs")) {
        const auto& c = j["cifs"];
        if (!c.is_object()) {
            out_error = "cifs block must be an object";
            return std::nullopt;
        }
        CifsCreds cc;
        cc.username = c.value("username", std::string{});
        cc.password = c.value("password", std::string{});
        cc.domain   = c.value("domain",   std::string{});
        s.cifs = std::move(cc);
    }

    if (!s.validate(out_error)) return std::nullopt;
    return s;
}

}  // namespace liveqx::mounts
