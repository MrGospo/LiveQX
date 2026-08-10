#include "metrics/ProcessMetrics.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#if defined(__linux__)
#include <dirent.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace {

#if defined(__linux__)

// Parse /proc/self/status. Looks for "VmRSS:", "VmSize:", "Threads:".
// Values for VmRSS/VmSize are in kB; we convert to bytes.
void readProcStatus(ProcessMetricsSnapshot& s) {
    std::FILE* f = std::fopen("/proc/self/status", "r");
    if (!f) return;
    char line[256];
    while (std::fgets(line, sizeof(line), f)) {
        if (std::strncmp(line, "VmRSS:", 6) == 0) {
            long kb = 0;
            if (std::sscanf(line + 6, "%ld", &kb) == 1) s.rss_bytes = kb * 1024;
        } else if (std::strncmp(line, "VmSize:", 7) == 0) {
            long kb = 0;
            if (std::sscanf(line + 7, "%ld", &kb) == 1) s.vsize_bytes = kb * 1024;
        } else if (std::strncmp(line, "Threads:", 8) == 0) {
            int n = 0;
            if (std::sscanf(line + 8, "%d", &n) == 1) s.threads = n;
        }
    }
    std::fclose(f);
}

// Count entries in /proc/self/fd. Each entry = one open file descriptor.
// Skip "." and "..". Race with close() during readdir manifests as ENOENT
// on individual entries — DIR* iteration just yields fewer entries, no
// special handling needed (we only count names, never open them).
void readOpenFds(ProcessMetricsSnapshot& s) {
    DIR* d = ::opendir("/proc/self/fd");
    if (!d) return;
    long n = 0;
    while (auto* ent = ::readdir(d)) {
        if (ent->d_name[0] == '.' &&
            (ent->d_name[1] == '\0' ||
             (ent->d_name[1] == '.' && ent->d_name[2] == '\0'))) {
            continue;
        }
        ++n;
    }
    ::closedir(d);
    // opendir() itself consumes one fd; subtract it so the count reflects
    // fds owned by application code, not the act of measuring.
    s.open_fds = n > 0 ? n - 1 : 0;
}

void readCpuSeconds(ProcessMetricsSnapshot& s) {
    struct rusage ru{};
    if (::getrusage(RUSAGE_SELF, &ru) != 0) return;
    const double user = static_cast<double>(ru.ru_utime.tv_sec) +
                        static_cast<double>(ru.ru_utime.tv_usec) / 1e6;
    const double sys  = static_cast<double>(ru.ru_stime.tv_sec) +
                        static_cast<double>(ru.ru_stime.tv_usec) / 1e6;
    s.cpu_seconds = user + sys;
}

#endif  // __linux__

}  // namespace

ProcessMetrics::ProcessMetrics(std::int64_t process_start_unix_sec) noexcept
    : start_unix_sec_(process_start_unix_sec) {}

void ProcessMetrics::setCacheTtl(std::chrono::milliseconds ttl) noexcept {
    std::lock_guard lk(mu_);
    cache_ttl_ = ttl;
}

void ProcessMetrics::refreshLocked() const {
    ProcessMetricsSnapshot s;
    s.start_time_unix_sec = start_unix_sec_;
#if defined(__linux__)
    readProcStatus(s);
    readOpenFds(s);
    readCpuSeconds(s);
#endif
    cached_   = s;
    has_cache_ = true;
    last_read_ = std::chrono::steady_clock::now();
}

ProcessMetricsSnapshot ProcessMetrics::snapshot() const {
    std::lock_guard lk(mu_);
    const auto now = std::chrono::steady_clock::now();
    if (!has_cache_ || (now - last_read_) >= cache_ttl_) {
        refreshLocked();
    }
    return cached_;
}
