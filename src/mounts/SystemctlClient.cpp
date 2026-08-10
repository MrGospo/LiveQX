#include "mounts/SystemctlClient.h"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <utility>

#include <unistd.h>

#include "mounts/Subprocess.h"

namespace fs = std::filesystem;

namespace liveqx::mounts {

namespace {

bool fileExecutable(const fs::path& p) {
    std::error_code ec;
    if (!fs::exists(p, ec)) return false;
    return ::access(p.c_str(), X_OK) == 0;
}

// Достаём значение property по имени из `systemctl show` вывода.
// Формат — `Key=Value\n`, по строке на свойство.
std::string parseShowProperty(const std::string& blob,
                              std::string_view key) {
    std::istringstream is(blob);
    std::string line;
    while (std::getline(is, line)) {
        if (line.size() > key.size()
            && line.compare(0, key.size(), key.data(), key.size()) == 0
            && line[key.size()] == '=') {
            return line.substr(key.size() + 1);
        }
    }
    return {};
}

// Читаем UnitFileState/ActiveState/LoadState/SubState за один вызов.
UnitState parseUnitShow(std::int64_t id, const std::string& blob) {
    UnitState s;
    s.id           = id;
    s.load_state   = parseShowProperty(blob, "LoadState");
    s.active_state = parseShowProperty(blob, "ActiveState");
    s.sub_state    = parseShowProperty(blob, "SubState");
    return s;
}

}  // namespace

SystemctlClient::Config SystemctlClient::defaultConfig() {
    Config c;
    if (fileExecutable("/usr/bin/systemctl")) {
        c.bin_path = "/usr/bin/systemctl";
    } else if (fileExecutable("/bin/systemctl")) {
        c.bin_path = "/bin/systemctl";
    }
    return c;
}

SystemctlClient::SystemctlClient(Config cfg) : cfg_(std::move(cfg)) {}

std::vector<std::string> SystemctlClient::baseArgv() const {
    std::vector<std::string> argv;
    argv.reserve(4);
    argv.push_back(cfg_.bin_path.string());
    argv.push_back(cfg_.system_scope ? "--system" : "--user");
    argv.push_back("--no-pager");
    return argv;
}

bool SystemctlClient::daemonReload(std::string& err) {
    if (cfg_.bin_path.empty()) {
        err = "systemctl binary not found";
        return false;
    }
    Subprocess sp;
    sp.argv = baseArgv();
    sp.argv.push_back("daemon-reload");
    sp.timeout_ms = cfg_.op_timeout;
    auto r = sp.run();
    if (!r.error.empty()) {
        err = "daemon-reload: " + r.error;
        return false;
    }
    if (r.timed_out) {
        err = "daemon-reload timed out";
        return false;
    }
    if (r.exit_code != 0) {
        err = "daemon-reload exit=" + std::to_string(r.exit_code)
              + (r.stderr_data.empty() ? "" : (": " + r.stderr_data));
        return false;
    }
    return true;
}

bool SystemctlClient::enableNow(std::string_view unit, std::string& err) {
    if (cfg_.bin_path.empty()) {
        err = "systemctl binary not found";
        return false;
    }
    Subprocess sp;
    sp.argv = baseArgv();
    sp.argv.push_back("enable");
    sp.argv.push_back("--now");
    sp.argv.push_back(std::string(unit));
    sp.timeout_ms = cfg_.op_timeout;
    auto r = sp.run();
    if (!r.error.empty()) {
        err = "enable --now: " + r.error;
        return false;
    }
    if (r.timed_out) {
        err = "enable --now timed out";
        return false;
    }
    if (r.exit_code != 0) {
        // Сохраняем stderr — он обычно показывает корень («Failed to mount:
        // No such file or directory» и т.п.).
        err = "enable --now exit=" + std::to_string(r.exit_code)
              + (r.stderr_data.empty() ? "" : (": " + r.stderr_data));
        return false;
    }
    return true;
}

bool SystemctlClient::start(std::string_view unit, std::string& err) {
    if (cfg_.bin_path.empty()) {
        err = "systemctl binary not found";
        return false;
    }
    Subprocess sp;
    sp.argv = baseArgv();
    sp.argv.push_back("start");
    sp.argv.push_back(std::string(unit));
    sp.timeout_ms = cfg_.op_timeout;
    auto r = sp.run();
    if (!r.error.empty()) {
        err = "start: " + r.error;
        return false;
    }
    if (r.timed_out) {
        err = "start timed out";
        return false;
    }
    if (r.exit_code != 0) {
        // stderr содержит детали сбоя монтирования
        // (mount.cifs error, hostname unreachable, etc.) — пробрасываем.
        err = "start exit=" + std::to_string(r.exit_code)
              + (r.stderr_data.empty() ? "" : (": " + r.stderr_data));
        return false;
    }
    return true;
}

bool SystemctlClient::resetFailed(std::string_view unit, std::string& err) {
    if (cfg_.bin_path.empty()) {
        err = "systemctl binary not found";
        return false;
    }
    Subprocess sp;
    sp.argv = baseArgv();
    sp.argv.push_back("reset-failed");
    sp.argv.push_back(std::string(unit));
    sp.timeout_ms = cfg_.op_timeout;
    auto r = sp.run();
    if (!r.error.empty()) {
        err = "reset-failed: " + r.error;
        return false;
    }
    if (r.timed_out) {
        err = "reset-failed timed out";
        return false;
    }
    // Юнит, которого systemd не знает (никогда не загружался) → exit≠0,
    // но это валидный «нечего сбрасывать» сценарий. Симметрично с
    // disableNow: тихо считаем успехом.
    if (r.exit_code != 0) {
        const auto& s = r.stderr_data;
        const bool no_such = s.find("does not exist") != std::string::npos
                          || s.find("No such file or directory") != std::string::npos
                          || s.find("not loaded") != std::string::npos
                          || s.find("Unit ") != std::string::npos;  // "Unit X.mount not loaded."
        if (no_such) return true;
        err = "reset-failed exit=" + std::to_string(r.exit_code)
              + (s.empty() ? "" : (": " + s));
        return false;
    }
    return true;
}

bool SystemctlClient::disableNow(std::string_view unit, std::string& err) {
    if (cfg_.bin_path.empty()) {
        err = "systemctl binary not found";
        return false;
    }
    Subprocess sp;
    sp.argv = baseArgv();
    sp.argv.push_back("disable");
    sp.argv.push_back("--now");
    sp.argv.push_back(std::string(unit));
    sp.timeout_ms = cfg_.op_timeout;
    auto r = sp.run();
    if (!r.error.empty()) {
        err = "disable --now: " + r.error;
        return false;
    }
    if (r.timed_out) {
        err = "disable --now timed out";
        return false;
    }
    // Юнит, которого уже нет в системе → systemctl возвращает не-0, но
    // это валидный «уже снят» сценарий. Атрибутируем его в лог, ошибкой
    // не считаем.
    if (r.exit_code != 0) {
        const auto& s = r.stderr_data;
        const bool no_such_file = s.find("does not exist") != std::string::npos
                                  || s.find("No such file or directory") != std::string::npos
                                  || s.find("not loaded") != std::string::npos;
        if (no_such_file) return true;
        err = "disable --now exit=" + std::to_string(r.exit_code)
              + (s.empty() ? "" : (": " + s));
        return false;
    }
    return true;
}

std::string SystemctlClient::activeState(std::string_view unit) {
    if (cfg_.bin_path.empty()) return {};
    Subprocess sp;
    sp.argv = baseArgv();
    sp.argv.push_back("show");
    sp.argv.push_back(std::string(unit));
    sp.argv.push_back("--property=ActiveState");
    sp.timeout_ms = cfg_.op_timeout;
    auto r = sp.run();
    if (!r.error.empty() || r.timed_out) return {};
    // exit_code≠0 — юнит не найден; парсер вернёт пустую строку.
    return parseShowProperty(r.stdout_data, "ActiveState");
}

UnitState SystemctlClient::unitState(std::string_view unit) {
    UnitState s;
    if (cfg_.bin_path.empty()) return s;
    Subprocess show;
    show.argv = baseArgv();
    show.argv.push_back("show");
    show.argv.push_back(std::string(unit));
    show.argv.push_back("--property=LoadState,ActiveState,SubState");
    show.timeout_ms = cfg_.op_timeout;
    auto r = show.run();
    if (!r.error.empty() || r.timed_out) return s;
    // exit_code≠0 — юнит не найден; парсер вернёт пустые поля.
    return parseUnitShow(/*id=*/0, r.stdout_data);
}

}  // namespace liveqx::mounts
