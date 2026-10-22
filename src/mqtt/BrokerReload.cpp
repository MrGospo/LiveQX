#include "mqtt/BrokerReload.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <csignal>
#include <fstream>
#include <sys/types.h>

namespace liveqx::mqtt {

std::string reloadBrokerFromPidFile(const std::filesystem::path& pid_file, const std::string& expected_name) {
    std::ifstream f(pid_file);
    if (!f) return "pid_file_unreadable";
    std::string text;
    std::getline(f, text);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.pop_back();
    if (text.empty() || text.size() > 9 ||
        !std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isdigit(c); }))
        return "bad_pid";
    const long pid = std::stol(text);
    if (pid <= 1) return "bad_pid";                         // never 0 (our own group) or 1 (init)

    std::ifstream comm("/proc/" + text + "/comm");
    if (!comm) return "process_not_running";
    std::string name;
    std::getline(comm, name);
    if (name != expected_name) return "not_the_broker";

    if (::kill(static_cast<pid_t>(pid), SIGHUP) != 0)
        return errno == ESRCH ? "process_not_running" : "signal_failed";
    return "";
}

}  // namespace liveqx::mqtt
