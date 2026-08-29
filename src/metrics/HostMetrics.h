#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

// Host-level resource metrics scraped from /proc and statvfs().
// Distinct from ProcessMetrics (which reports the liveqx process itself);
// this describes the whole machine and is exposed on the admin-only
// /api/system/host_metrics endpoint.
//
// All fields populated on Linux. On non-Linux builds (developer mac),
// counters remain at 0 and vectors are empty; callers should treat empty
// snapshots as "unavailable" and render "—" in the UI.

namespace liveqx::metrics {

// One line from /proc/stat: cumulative jiffies since boot. Callers compute
// utilisation as delta over a sampling window (see toJsonWithRates below).
struct HostCpuLine {
    std::string   name;      // "cpu" (aggregate) or "cpu0", "cpu1", ...
    std::uint64_t user     = 0;
    std::uint64_t nice     = 0;
    std::uint64_t system   = 0;
    std::uint64_t idle     = 0;
    std::uint64_t iowait   = 0;
    std::uint64_t irq      = 0;
    std::uint64_t softirq  = 0;
    std::uint64_t steal    = 0;
};

// /proc/meminfo — values converted from kB to bytes at parse time so
// downstream code (and JSON output) speaks a single unit.
struct HostMemSnapshot {
    std::uint64_t total_bytes     = 0;
    std::uint64_t free_bytes      = 0;
    std::uint64_t available_bytes = 0;  // MemAvailable (kernel's own estimate)
    std::uint64_t buffers_bytes   = 0;
    std::uint64_t cached_bytes    = 0;
    std::uint64_t swap_total_bytes = 0;
    std::uint64_t swap_free_bytes  = 0;
};

// One row from /proc/net/dev. Cumulative counters — the SSE handler
// converts to bytes-per-second between successive snapshots.
struct HostNicSnapshot {
    std::string   name;
    bool          is_up = false;
    std::uint64_t rx_bytes   = 0;
    std::uint64_t rx_packets = 0;
    std::uint64_t rx_errs    = 0;
    std::uint64_t rx_drop    = 0;
    std::uint64_t tx_bytes   = 0;
    std::uint64_t tx_packets = 0;
    std::uint64_t tx_errs    = 0;
    std::uint64_t tx_drop    = 0;
};

// statvfs() result for a single local mount point. CIFS / NFS / fuse mounts
// are intentionally omitted from the enumeration because statvfs() can
// block for the network round-trip if the share is unreachable, and the
// SSE sampler must stay off the critical path.
struct HostFsSnapshot {
    std::string   mount;      // e.g. "/", "/var/lib/liveqx"
    std::string   fstype;     // e.g. "ext4"
    std::uint64_t total_bytes = 0;
    std::uint64_t free_bytes  = 0;
};

// One row from /proc/diskstats. sectors_* are 512-byte sectors as
// documented in the kernel — converted to bytes by the JSON layer.
struct HostDiskIoSnapshot {
    std::string   name;             // "sda", "nvme0n1", ...
    std::uint64_t reads_completed  = 0;
    std::uint64_t sectors_read     = 0;
    std::uint64_t writes_completed = 0;
    std::uint64_t sectors_written  = 0;
};

struct HostSnapshot {
    std::int64_t                     sampled_at_unix_ms = 0;
    std::int64_t                     uptime_seconds     = 0;
    double                           load1  = 0.0;
    double                           load5  = 0.0;
    double                           load15 = 0.0;
    std::vector<HostCpuLine>         cpu;     // [0] = aggregate, [1..N] = per-core
    HostMemSnapshot                  mem;
    std::vector<HostNicSnapshot>     nics;
    std::vector<HostFsSnapshot>      fs;
    std::vector<HostDiskIoSnapshot>  disks;
};

class HostMetricsReader {
public:
    // Fresh read on every call — no cache. Cheap enough (~1 ms on typical
    // /proc) that a 1 Hz SSE sampler is imperceptible. Thread-safe (each
    // call opens its own FILE*/statvfs handles).
    static HostSnapshot sample();

    // Snapshot without rates — used for the one-shot GET endpoint and for
    // the FIRST SSE frame (before a delta is available). Contains all
    // absolute values plus cumulative counters that a client can rate on
    // its own if it wants to.
    static nlohmann::json toJson(const HostSnapshot& s);

    // Snapshot enriched with rates computed from (curr - prev) / dt:
    //   cpu.total_pct, cpu.per_core[i].pct
    //   nics[i].rx_bps, nics[i].tx_bps
    //   disks[i].read_bps, disks[i].write_bps
    // Used for every SSE frame after the first.
    static nlohmann::json toJsonWithRates(const HostSnapshot& prev,
                                          const HostSnapshot& curr);
};

}  // namespace liveqx::metrics
