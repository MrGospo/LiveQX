#pragma once
//
// Telling a local MQTT broker to re-read its files (SIGHUP).
//
// The broker is a service the installer sets up to run as the same user as the
// server, with a pid file, so the server can signal it directly — no privileged
// helper, no shell. The signal goes only to a process that really is the
// broker: a pid file left behind by a broker that is gone may now name some
// other process, and a hangup is not harmless to every program.

#include <filesystem>
#include <string>

namespace liveqx::mqtt {

// Sends SIGHUP to the process named in `pid_file` if that process is called
// `expected_name` (as /proc/<pid>/comm says). Returns "" on success, otherwise
// a short reason: pid_file_unreadable, bad_pid, process_not_running,
// not_the_broker, signal_failed.
std::string reloadBrokerFromPidFile(const std::filesystem::path& pid_file,
                                    const std::string& expected_name = "mosquitto");

}  // namespace liveqx::mqtt
