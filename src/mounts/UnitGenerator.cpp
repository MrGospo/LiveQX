#include "mounts/UnitGenerator.h"

#include <array>
#include <cstdint>
#include <sstream>

namespace liveqx::mounts {

namespace {

// Strip a single trailing '/' (but not the only '/' of "/"). Это
// нормализует пользовательский ввод "/mnt/foo/" к "/mnt/foo" перед
// escape'ом — иначе imeни unit'а получают пустой "сегмент" после
// последнего '/'.
std::string stripTrailingSlash(std::string_view s) {
    if (s.size() > 1 && s.back() == '/') {
        return std::string(s.substr(0, s.size() - 1));
    }
    return std::string(s);
}

// Канонизация target'а: гарантируем ведущий '/', снимаем trailing '/'.
// Это то значение, что попадает в Where= в .mount/.automount юнитах —
// systemd валидирует Where= против basename'а, поэтому путь и basename
// должны нормализоваться одним и тем же способом.
std::string normalizeTargetImpl(std::string_view target_in) {
    std::string t = stripTrailingSlash(target_in);
    if (t.empty()) return "/";
    if (t.front() != '/') t.insert(t.begin(), '/');
    return t;
}

bool isUnreservedSystemdChar(unsigned char c) noexcept {
    // systemd-escape оставляет неэскейпленными: [A-Za-z0-9_.]. ВАЖНО:
    // '-' зарезервирован как сепаратор сегментов после escape'а
    // ('/' → '-'), поэтому литеральный '-' в исходном пути обязан стать
    // '\x2d' — иначе systemd-escape --path X != нашему результату, и
    // юниты с Where= не пройдут валидацию.
    return (c >= 'a' && c <= 'z')
        || (c >= 'A' && c <= 'Z')
        || (c >= '0' && c <= '9')
        || c == '_' || c == '.';
}

void appendEscapedByte(std::string& out, unsigned char c) {
    static const char hex[] = "0123456789abcdef";
    out.push_back('\\');
    out.push_back('x');
    out.push_back(hex[(c >> 4) & 0x0f]);
    out.push_back(hex[c & 0x0f]);
}

}  // namespace

std::string systemdEscapePath(std::string_view target_in) {
    // Шаг 1: нормализуем — снимаем trailing '/' (но не превращаем "/" в "").
    std::string target = stripTrailingSlash(target_in);

    // Шаг 2: edge case'ы. systemd-escape(--path "/") = "-",
    // systemd-escape(--path "") = "-".
    if (target.empty() || target == "/") {
        return "-";
    }

    // Шаг 3: leading slash убираем — для абсолютного пути systemd-escape
    // не префиксует '-', он его подразумевает по конвенции (path-only
    // unit names всегда относительные к /).
    std::string_view body{target};
    if (body.front() == '/') body.remove_prefix(1);

    std::string out;
    out.reserve(body.size() * 2);

    // Шаг 4: ведущая точка после стрипа '/' эскейпится как \x2e.
    // Иначе systemd считал бы юнит «hidden» (на самом деле systemd
    // просто требует escape для . в начале, иначе load fails).
    bool first_char = true;

    for (std::size_t i = 0; i < body.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(body[i]);
        if (c == '/') {
            out.push_back('-');
            first_char = false;
            continue;
        }
        if (first_char && c == '.') {
            appendEscapedByte(out, c);
            first_char = false;
            continue;
        }
        if (isUnreservedSystemdChar(c)) {
            out.push_back(static_cast<char>(c));
        } else {
            appendEscapedByte(out, c);
        }
        first_char = false;
    }
    return out;
}

std::string unitBasenameByTarget(std::string_view target) {
    return systemdEscapePath(target);
}

std::string credFilename(std::int64_t id) {
    return "liveqx-mnt-" + std::to_string(id);
}

std::string legacyUnitBasename(std::int64_t id) {
    return "liveqx-mnt-" + std::to_string(id);
}

namespace {

// Comma-склейка двух наборов опций без пустых элементов и без
// дублирования через простой substring-match. Для мердж-логики хватит:
// мы не парсим options как KV.
std::string joinOpts(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    std::string out = a;
    if (out.back() != ',') out.push_back(',');
    out += b;
    return out;
}

bool containsToken(const std::string& opts, const std::string& token) {
    // Грубая проверка: ищем "token" между запятыми/началом/концом.
    auto pos = opts.find(token);
    while (pos != std::string::npos) {
        const bool left_ok  = (pos == 0 || opts[pos - 1] == ',');
        const auto end      = pos + token.size();
        const bool right_ok = (end == opts.size()
                               || opts[end] == ','
                               || opts[end] == '=');
        if (left_ok && right_ok) return true;
        pos = opts.find(token, pos + 1);
    }
    return false;
}

}  // namespace

std::string buildOptionsLine(const MountSpec&             spec,
                             bool                         with_credentials,
                             const std::filesystem::path& cred_path,
                             const UnitGenContext&        ctx) {
    std::string opts = spec.options;
    // ro/rw — добавляем только если оператор не задал явно. Для библиотеки
    // ro=true дефолт; для writable HLS-выходов (follow-up) пользователь
    // зашлёт ro=false.
    if (spec.ro && !containsToken(opts, "ro") && !containsToken(opts, "rw")) {
        opts = joinOpts(opts, "ro");
    } else if (!spec.ro && !containsToken(opts, "rw")
               && !containsToken(opts, "ro")) {
        opts = joinOpts(opts, "rw");
    }

    if (spec.fs_type == FsType::Cifs && with_credentials) {
        // fix43: абсолютный путь к plaintext cred-файлу. mount.cifs читает
        // его напрямую (username=/password=/domain= по строкам). До fix43
        // был "%d/cifs-creds" поверх LoadCredentialEncrypted= — не работало
        // для [Mount] юнитов, см. CredsHelper.h.
        if (!containsToken(opts, "credentials")) {
            opts = joinOpts(opts, "credentials=" + cred_path.string());
        }
    }

    // fix44: для CIFS подставляем uid/gid/file_mode/dir_mode пользователя
    // liveqx (uid резолвит mountd при старте через getpwnam). Без
    // этого mount.cifs мапит файлы на uid=0 и Watcher под liveqx
    // видит пустой каталог. Оператор может перебить любой из четырёх токенов
    // через MountSpec.options — мы добавляем только отсутствующие.
    if (spec.fs_type == FsType::Cifs && ctx.cifs_uid != 0) {
        if (!containsToken(opts, "uid")) {
            opts = joinOpts(opts, "uid=" + std::to_string(ctx.cifs_uid));
        }
        if (ctx.cifs_gid != 0 && !containsToken(opts, "gid")) {
            opts = joinOpts(opts, "gid=" + std::to_string(ctx.cifs_gid));
        }
        if (!ctx.cifs_file_mode.empty() && !containsToken(opts, "file_mode")) {
            opts = joinOpts(opts, "file_mode=" + ctx.cifs_file_mode);
        }
        if (!ctx.cifs_dir_mode.empty() && !containsToken(opts, "dir_mode")) {
            opts = joinOpts(opts, "dir_mode=" + ctx.cifs_dir_mode);
        }
    }

    return opts;
}

namespace {

std::string makeMountUnit(const MountSpec&   spec,
                          const std::string& normalized_target,
                          const std::string& options) {
    // fix43: больше нет LoadCredentialEncrypted= — путь к creds живёт
    // прямо в Options=credentials=. Cм. CredsHelper.h за бэкстори.
    std::ostringstream o;
    o << "[Unit]\n"
      << "Description=Streaming Core mount: " << spec.source
      << " -> " << normalized_target << "\n"
      << "Documentation=file:/usr/share/doc/liveqx/MOUNTS.md\n"
      << "After=network-online.target\n"
      << "Wants=network-online.target\n"
      << "\n"
      << "[Mount]\n"
      << "What=" << spec.source << "\n"
      << "Where=" << normalized_target << "\n"
      << "Type=" << toString(spec.fs_type) << "\n";
    if (!options.empty()) {
        o << "Options=" << options << "\n";
    }
    o << "TimeoutSec=20\n"
      << "\n[Install]\nWantedBy=multi-user.target\n";
    return o.str();
}

std::string makeAutomountUnit(const std::string& normalized_target,
                              int                idle_seconds) {
    std::ostringstream o;
    o << "[Unit]\n"
      << "Description=Streaming Core automount: " << normalized_target << "\n"
      << "After=network-online.target\n"
      << "\n"
      << "[Automount]\n"
      << "Where=" << normalized_target << "\n";
    if (idle_seconds > 0) {
        o << "TimeoutIdleSec=" << idle_seconds << "\n";
    }
    o << "\n[Install]\nWantedBy=multi-user.target\n";
    return o.str();
}

// Формат CIFS credentials-файла, который читает mount.cifs из
// `credentials=` опции:
//   username=...
//   password=...
//   domain=...     (optional)
// Никаких лишних пробелов и trailing newline'ов — иначе mount.cifs
// иногда жалуется. Каждое поле своей строкой, конец файла без \n.
std::string makeCifsCredPayload(const CifsCreds& c) {
    std::string s;
    s += "username=" + c.username + "\n";
    s += "password=" + c.password + "\n";
    if (!c.domain.empty()) s += "domain=" + c.domain + "\n";
    return s;
}

}  // namespace

std::string normalizedTargetPath(std::string_view target_in) {
    return normalizeTargetImpl(target_in);
}

GeneratedUnits generateUnits(const MountSpec& spec, const UnitGenContext& ctx) {
    GeneratedUnits g;

    // Канонизуем target и из него выводим basename. Это исключает
    // расхождение между Where= и именем юнита, на котором systemd ругается
    // ("Where= setting doesn't match unit name. Refusing.").
    const std::string normalized_target = normalizeTargetImpl(spec.target);
    g.unit_basename = unitBasenameByTarget(normalized_target);

    const bool needs_creds = (spec.fs_type == FsType::Cifs
                              && spec.cifs.has_value()
                              && !spec.cifs->password.empty());

    if (needs_creds) {
        g.cred_payload = makeCifsCredPayload(*spec.cifs);
        // Имя cred-файла — id-based; стабильный ASCII-ключ для tmpfs-каталога.
        g.cred_path    = ctx.cred_dir / (credFilename(spec.id) + ".cred");
    }

    const std::string options = buildOptionsLine(spec, needs_creds, g.cred_path, ctx);

    g.mount_unit = makeMountUnit(spec, normalized_target, options);
    if (ctx.generate_automount) {
        g.automount_unit = makeAutomountUnit(normalized_target,
                                             ctx.automount_idle_seconds);
    }
    return g;
}

}  // namespace liveqx::mounts
