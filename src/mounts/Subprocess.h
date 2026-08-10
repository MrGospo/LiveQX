#pragma once
//
// fix41 — минимальный fork+execve+waitpid wrapper для mountd.
//
// Зачем своё, а не popen/std::system: и popen, и std::system проходят
// через /bin/sh, что добавляет shell-injection поверхность и съедает
// предсказуемость. Helper говорит со внешними программами (systemctl,
// systemd-creds, mount, umount), и единственный надёжный способ —
// execve с argv-вектором без shell.
//
// API:
//   Subprocess sp;
//   sp.argv     = {"/bin/systemctl", "daemon-reload"};
//   sp.stdin_data = "password\n";   // optional
//   sp.timeout_ms = 10000;
//   auto r = sp.run();
//   r.exit_code, r.stdout_data, r.stderr_data, r.timed_out
//
// Контракт: argv[0] — абсолютный путь (резолв через PATH в helper'е
// мы делать не хотим, чтобы не зависеть от $PATH у systemd-юнита).
// Если процесс не успевает за timeout_ms — мы шлём SIGTERM, ждём 1s,
// потом SIGKILL.

#include <chrono>
#include <string>
#include <vector>

namespace liveqx::mounts {

struct ProcessResult {
    int         exit_code   = -1;     // -1 если не вызван / kill'нут
    bool        timed_out   = false;
    bool        signaled    = false;
    std::string stdout_data;
    std::string stderr_data;
    std::string error;                 // непусто при системных провалах
                                       // (fork, pipe, execve)
};

class Subprocess {
public:
    std::vector<std::string> argv;     // argv[0] = абсолютный путь к binary
    std::string              stdin_data;            // что подать в stdin
    std::chrono::milliseconds timeout_ms{30000};

    // Outputs capped в RAM. Если внешняя программа печатает гигабайты —
    // мы дропаем «хвост» после лимита и продолжаем читать пустоту.
    std::size_t max_stdout_bytes = 256 * 1024;
    std::size_t max_stderr_bytes = 256 * 1024;

    ProcessResult run() const;
};

}  // namespace liveqx::mounts
