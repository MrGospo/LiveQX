#include "api/MetricsCollector.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

extern "C" {
#include <libavutil/avutil.h>      // av_version_info()
}
#include <openssl/opensslv.h>      // OPENSSL_VERSION_TEXT
#include <srt/srt.h>               // srt_getversion()

#include "api/ChannelManager.h"
#include "api/RbacScope.h"
#include "core/ChannelInstance.h"
#include "gateway/IGateway.h"
#include "gateway/GatewayManager.h"
#include "metrics/ChannelHealth.h"
#include "metrics/ChannelMetrics.h"
#include "metrics/ProcessMetrics.h"
#include "mounts/MountManager.h"
#include "plugins/PluginManager.h"
#include "stress/StressService.h"

namespace {

// Escape a label value per Prometheus exposition spec:
// `\` → `\\`, `"` → `\"`, `\n` → `\n` (literal two-char escape).
// UTF-8 bytes pass through.
std::string escapeLabel(const std::string& v) {
    std::string out;
    out.reserve(v.size() + 4);
    for (char c : v) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\"";  break;
            case '\n': out += "\\n";  break;
            default:   out += c;      break;
        }
    }
    return out;
}

void writeHeader(std::ostringstream& out, const char* name,
                 const char* help, const char* type) {
    out << "# HELP " << name << ' ' << help << '\n';
    out << "# TYPE " << name << ' ' << type << '\n';
}

// Writes a fp64 value, special-casing NaN / +Inf / -Inf per spec.
void writeDouble(std::ostringstream& out, double v) {
    if (std::isnan(v)) {
        out << "NaN";
    } else if (std::isinf(v)) {
        out << (v > 0 ? "+Inf" : "-Inf");
    } else {
        // 6 significant digits is plenty for fps / cpu_seconds. Avoid
        // locale-dependent printing — Prometheus expects '.' decimal.
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.6g", v);
        out << buf;
    }
}

std::string channelLabels(int id, const std::string& name) {
    std::ostringstream s;
    s << "channel_id=\"" << id << "\",channel_name=\"" << escapeLabel(name) << "\"";
    return s.str();
}

void emitChannelGauge(std::ostringstream& out, const char* name,
                      int id, const std::string& chname,
                      double value) {
    out << name << '{' << channelLabels(id, chname) << "} ";
    writeDouble(out, value);
    out << '\n';
}

void emitChannelCounter(std::ostringstream& out, const char* name,
                        int id, const std::string& chname,
                        std::uint64_t value) {
    out << name << '{' << channelLabels(id, chname) << "} " << value << '\n';
}

// Per-output {channel_id, channel_name, output_id, output_type} labels.
std::string outputLabels(int channel_id, const std::string& chname,
                         const std::string& output_id, const std::string& type) {
    std::ostringstream s;
    s << "channel_id=\"" << channel_id << "\""
      << ",channel_name=\"" << escapeLabel(chname) << "\""
      << ",output_id=\""    << escapeLabel(output_id) << "\""
      << ",output_type=\""  << escapeLabel(type) << "\"";
    return s.str();
}

}  // namespace

MetricsCollector::MetricsCollector(const ChannelManager& mgr,
                                   ProcessMetrics&       process,
                                   BuildInfo             build,
                                   const liveqx::gateway::GatewayManager* gateways,
                                   const liveqx::stress::StressService*   stress,
                                   const liveqx::plugins::PluginManager*  plugins,
                                   liveqx::mounts::MountManager*          mounts)
    : mgr_(mgr), process_(process), build_(std::move(build)),
      gateways_(gateways), stress_(stress), plugins_(plugins),
      mounts_(mounts) {}

std::string MetricsCollector::renderPrometheus(
    const liveqx::api::ChannelScope* scope) const {
    std::ostringstream out;
    const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count();

    // ── Build info ──────────────────────────────────────────────────────────
    writeHeader(out, "liveqx_build_info",
                "Build metadata (always 1).", "gauge");
    out << "liveqx_build_info{"
        << "version=\""    << escapeLabel(build_.version)    << "\","
        << "commit=\""     << escapeLabel(build_.commit)     << "\","
        << "build_time=\"" << escapeLabel(build_.build_time) << "\","
        << "build_type=\"" << escapeLabel(build_.build_type) << "\""
        << "} 1\n";

    // ── Process metrics ────────────────────────────────────────────────────
    const auto pm = process_.snapshot();
    if (pm.start_time_unix_sec > 0) {
        writeHeader(out, "process_start_time_seconds",
                    "Process start time, seconds since epoch.", "gauge");
        out << "process_start_time_seconds " << pm.start_time_unix_sec << '\n';

        writeHeader(out, "liveqx_uptime_seconds",
                    "Seconds since process start.", "gauge");
        const auto now_unix = std::chrono::duration_cast<std::chrono::seconds>(
                                  std::chrono::system_clock::now().time_since_epoch())
                                  .count();
        out << "liveqx_uptime_seconds "
            << (now_unix - pm.start_time_unix_sec) << '\n';
    }

    if (pm.rss_bytes >= 0) {
        writeHeader(out, "process_resident_memory_bytes",
                    "Resident memory (VmRSS).", "gauge");
        out << "process_resident_memory_bytes " << pm.rss_bytes << '\n';
    }
    if (pm.vsize_bytes >= 0) {
        writeHeader(out, "process_virtual_memory_bytes",
                    "Virtual memory size (VmSize).", "gauge");
        out << "process_virtual_memory_bytes " << pm.vsize_bytes << '\n';
    }
    if (pm.open_fds >= 0) {
        writeHeader(out, "process_open_fds",
                    "Open file descriptors.", "gauge");
        out << "process_open_fds " << pm.open_fds << '\n';
    }
    if (pm.threads >= 0) {
        writeHeader(out, "process_threads",
                    "Thread count.", "gauge");
        out << "process_threads " << pm.threads << '\n';
    }
    if (pm.cpu_seconds >= 0.0) {
        writeHeader(out, "process_cpu_seconds_total",
                    "User + system CPU time, seconds.", "counter");
        out << "process_cpu_seconds_total ";
        writeDouble(out, pm.cpu_seconds);
        out << '\n';
    }

    // ── Channel-level summary counters ─────────────────────────────────────
    int total = 0, running = 0, failed = 0, outputs_total = 0;

    // Headers for repeated per-channel metric families. Emit once before the
    // loop so the # HELP / # TYPE lines do not repeat per channel.
    writeHeader(out, "liveqx_fps",
                "Render fps measured over last 1s window.", "gauge");
    writeHeader(out, "liveqx_target_fps",
                "Configured render fps.", "gauge");
    writeHeader(out, "liveqx_frames_rendered_total",
                "Frames emitted by the render loop.", "counter");
    writeHeader(out, "liveqx_frames_dropped_total",
                "Frames dropped on queue full or invalid getFrame().", "counter");
    writeHeader(out, "liveqx_audio_underruns_total",
                "Audio buffer underruns (padded with silence).", "counter");
    writeHeader(out, "liveqx_audio_loop_glitches_total",
                "Audio glitches at clip-loop boundaries.", "counter");
    writeHeader(out, "liveqx_decode_errors_total",
                "FFmpeg send/receive failures.", "counter");
    writeHeader(out, "liveqx_loop_fallback_total",
                "EOFs that landed on fallback (no warm preload).", "counter");
    writeHeader(out, "liveqx_frame_time_max_us",
                "Peak per-frame work time in current 60s window, microseconds.",
                "gauge");
    writeHeader(out, "liveqx_health_state",
                "Channel health: 0=Running 1=Degraded 2=Failed.", "gauge");
    writeHeader(out, "liveqx_last_tick_age_seconds",
                "Seconds since last render tick (heartbeat lag).", "gauge");
    writeHeader(out, "liveqx_running",
                "1 if channel is in running state, else 0.", "gauge");
    writeHeader(out, "liveqx_clip_changes_total",
                "Active-clip transitions observed since channel start.", "counter");
    writeHeader(out, "liveqx_boundary_drops_total",
                "Boundary events dropped because the dispatcher queue was full.",
                "counter");
    writeHeader(out, "liveqx_transition_active",
                "1 iff render loop is currently mid-transition between two clips.",
                "gauge");
    writeHeader(out, "liveqx_cache_files",
                "ContentSync cache file count.", "gauge");
    writeHeader(out, "liveqx_cache_size_bytes",
                "ContentSync cache size in bytes.", "gauge");
    writeHeader(out, "liveqx_cache_pending_deletes",
                "ContentSync deletes deferred until clip release.", "gauge");
    writeHeader(out, "liveqx_share_unreachable_total",
                "Times the share scan failed to reach the source.", "counter");
    writeHeader(out, "liveqx_cache_copy_errors_total",
                "ContentSync copy operations that failed.", "counter");
    writeHeader(out, "liveqx_oversized_skipped_total",
                "ContentSync entries skipped because they exceeded max size.",
                "counter");

    // Output-family headers.
    writeHeader(out, "liveqx_output_state",
                "Output driver: 1 = healthy, 0 = unhealthy.", "gauge");
    writeHeader(out, "liveqx_output_bytes_total",
                "Bytes pushed to driver socket.", "counter");
    writeHeader(out, "liveqx_output_packets_total",
                "Packets pushed to driver socket.", "counter");
    writeHeader(out, "liveqx_output_drops_total",
                "Packets dropped by per-driver SPSC queue.", "counter");
    writeHeader(out, "liveqx_output_queue_bytes_used",
                "Bytes currently buffered in per-driver queue.", "gauge");
    writeHeader(out, "liveqx_output_queue_bytes_limit",
                "Per-driver queue byte budget.", "gauge");

    mgr_.forEachChannel([&](const ChannelInstance& ch) {
        if (scope && !scope->allows(ch.id())) return;
        ++total;
        const auto m_ptr = ch.metrics();
        const auto h_ptr = ch.health();
        if (!m_ptr || !h_ptr) return;

        const auto       snap        = m_ptr->snapshot();
        const HealthState health     = h_ptr->state();
        const int         health_code = static_cast<int>(health);
        const int         target_fps  = h_ptr->targetFps();
        const bool        is_running  = ch.isRunning();
        if (is_running) ++running;
        if (health == HealthState::Failed) ++failed;

        const double tick_age_s = snap.last_tick_ns > 0
            ? static_cast<double>(now_ns - snap.last_tick_ns) / 1e9
            : -1.0;

        emitChannelGauge  (out, "liveqx_fps",                ch.id(), ch.name(), snap.actual_fps);
        emitChannelGauge  (out, "liveqx_target_fps",         ch.id(), ch.name(), target_fps);
        emitChannelCounter(out, "liveqx_frames_rendered_total",   ch.id(), ch.name(), snap.frames_rendered);
        emitChannelCounter(out, "liveqx_frames_dropped_total",    ch.id(), ch.name(), snap.frames_dropped);
        emitChannelCounter(out, "liveqx_audio_underruns_total",   ch.id(), ch.name(), snap.audio_underruns);
        emitChannelCounter(out, "liveqx_audio_loop_glitches_total", ch.id(), ch.name(), snap.audio_loop_glitches);
        emitChannelCounter(out, "liveqx_decode_errors_total",     ch.id(), ch.name(), snap.decode_errors);
        emitChannelCounter(out, "liveqx_loop_fallback_total",     ch.id(), ch.name(), snap.loop_fallback_count);
        emitChannelGauge  (out, "liveqx_frame_time_max_us",  ch.id(), ch.name(), static_cast<double>(snap.frame_time_max_us));
        emitChannelGauge  (out, "liveqx_health_state",       ch.id(), ch.name(), health_code);
        emitChannelGauge  (out, "liveqx_last_tick_age_seconds", ch.id(), ch.name(), tick_age_s);
        emitChannelGauge  (out, "liveqx_running",            ch.id(), ch.name(), is_running ? 1.0 : 0.0);
        emitChannelCounter(out, "liveqx_clip_changes_total", ch.id(), ch.name(), snap.clip_changes);
        emitChannelCounter(out, "liveqx_boundary_drops_total", ch.id(), ch.name(), snap.boundary_drops);
        emitChannelGauge  (out, "liveqx_transition_active",  ch.id(), ch.name(), snap.transition_active ? 1.0 : 0.0);
        emitChannelGauge  (out, "liveqx_cache_files",        ch.id(), ch.name(), static_cast<double>(snap.cache_files_count));
        emitChannelGauge  (out, "liveqx_cache_size_bytes",   ch.id(), ch.name(), static_cast<double>(snap.cache_size_bytes));
        emitChannelGauge  (out, "liveqx_cache_pending_deletes", ch.id(), ch.name(), static_cast<double>(snap.pending_deletes));
        emitChannelCounter(out, "liveqx_share_unreachable_total", ch.id(), ch.name(), snap.share_unreachable_count);
        emitChannelCounter(out, "liveqx_cache_copy_errors_total", ch.id(), ch.name(), snap.cache_copy_errors);
        emitChannelCounter(out, "liveqx_oversized_skipped_total", ch.id(), ch.name(), snap.oversized_skipped);

        // ── Per-output ──────────────────────────────────────────────────────
        // outputsJson() returns runtime stats while running, otherwise the
        // raw cfg array. We only emit per-output counters when we have
        // actual stats — i.e. the entry includes "transport".
        const auto outputs = ch.outputsJson();
        if (!outputs.is_array()) return;

        for (const auto& o : outputs) {
            if (!o.is_object()) continue;
            ++outputs_total;

            const std::string oid = o.value("id", std::string{});
            const std::string transport =
                o.contains("transport") ? o.value("transport", std::string{})
                : o.value("type", std::string{});
            if (oid.empty() || transport.empty()) continue;

            const auto labels = outputLabels(ch.id(), ch.name(), oid, transport);

            // Always emit state — even if stats absent, healthy=false is
            // useful "this output exists but isn't producing".
            const bool healthy = o.value("healthy", false);
            out << "liveqx_output_state{" << labels << "} "
                << (healthy ? 1 : 0) << '\n';

            if (!o.contains("bytes_sent")) continue;  // cfg-only entry, no runtime stats

            out << "liveqx_output_bytes_total{"   << labels << "} "
                << o.value("bytes_sent",   std::uint64_t{0}) << '\n';
            out << "liveqx_output_packets_total{" << labels << "} "
                << o.value("packets_sent", std::uint64_t{0}) << '\n';
            out << "liveqx_output_drops_total{"   << labels << "} "
                << o.value("queue_drops",  std::uint64_t{0}) << '\n';
            out << "liveqx_output_queue_bytes_used{"  << labels << "} "
                << o.value("queue_bytes_used",  std::uint64_t{0}) << '\n';
            out << "liveqx_output_queue_bytes_limit{" << labels << "} "
                << o.value("queue_bytes_limit", std::uint64_t{0}) << '\n';
        }
    });

    // ── Cross-channel summary ───────────────────────────────────────────────
    writeHeader(out, "liveqx_channels_total",
                "Number of channels registered (running or stopped).", "gauge");
    out << "liveqx_channels_total " << total << '\n';

    writeHeader(out, "liveqx_channels_running",
                "Channels with state==running.", "gauge");
    out << "liveqx_channels_running " << running << '\n';

    writeHeader(out, "liveqx_channels_failed",
                "Channels with health==failed.", "gauge");
    out << "liveqx_channels_failed " << failed << '\n';

    writeHeader(out, "liveqx_outputs_total",
                "Outputs registered across all channels.", "gauge");
    out << "liveqx_outputs_total " << outputs_total << '\n';

    // ── Gateway metrics (fix18 c7/10) ───────────────────────────────────────
    // Cardinality is bounded by the number of gateways the operator creates,
    // typically ≪ channels, so per-id labelling is fine. Hidden from
    // viewer-scoped callers (gateways are cross-channel infra).
    if (gateways_ && (!scope || scope->allowsCrossChannel())) {
        writeHeader(out, "liveqx_gateway_running",
                    "1 iff gateway io-thread is forwarding.", "gauge");
        writeHeader(out, "liveqx_gateway_packets_in_total",
                    "Datagrams received on the input socket.", "counter");
        writeHeader(out, "liveqx_gateway_packets_out_total",
                    "Datagrams pushed across all outputs (sum of per-output sendto).",
                    "counter");
        writeHeader(out, "liveqx_gateway_bytes_in_total",
                    "Bytes received on the input socket.", "counter");
        writeHeader(out, "liveqx_gateway_bytes_out_total",
                    "Bytes pushed across all outputs.", "counter");
        writeHeader(out, "liveqx_gateway_drops_total",
                    "sendto failures across all outputs (EAGAIN, ENOBUFS, ...).",
                    "counter");
        writeHeader(out, "liveqx_gateway_outputs",
                    "Configured outputs for this gateway.", "gauge");

        int gw_total = 0, gw_running = 0;
        gateways_->forEachGateway([&](const liveqx::gateway::IGateway& gw) {
            ++gw_total;
            const auto stats   = gw.getStats();
            const bool running_now = gw.isRunning();
            if (running_now) ++gw_running;

            std::ostringstream l;
            l << "gateway_id=\"" << gw.id() << "\""
              << ",gateway_name=\"" << escapeLabel(gw.name()) << "\"";
            const auto labels = l.str();

            out << "liveqx_gateway_running{"          << labels << "} "
                << (running_now ? 1 : 0) << '\n';
            out << "liveqx_gateway_packets_in_total{" << labels << "} "
                << stats.pkt_in << '\n';
            out << "liveqx_gateway_packets_out_total{" << labels << "} "
                << stats.pkt_out << '\n';
            out << "liveqx_gateway_bytes_in_total{"   << labels << "} "
                << stats.bytes_in << '\n';
            out << "liveqx_gateway_bytes_out_total{"  << labels << "} "
                << stats.bytes_out << '\n';
            out << "liveqx_gateway_drops_total{"      << labels << "} "
                << stats.drops << '\n';
            out << "liveqx_gateway_outputs{"          << labels << "} "
                << gw.cfg().outputs.size() << '\n';
        });

        writeHeader(out, "liveqx_gateways_total",
                    "Number of gateways registered (running or stopped).", "gauge");
        out << "liveqx_gateways_total " << gw_total << '\n';

        writeHeader(out, "liveqx_gateways_running",
                    "Gateways currently forwarding.", "gauge");
        out << "liveqx_gateways_running " << gw_running << '\n';
    }

    // ── Stress runner (fix26) ───────────────────────────────────────────────
    // Cardinality is a fixed handful of gauges/counters per host — no labels.
    if (stress_) {
        const auto s = stress_->metricsSnapshot();

        writeHeader(out, "liveqx_stress_running",
                    "1 iff a stress run is currently in progress.", "gauge");
        out << "liveqx_stress_running " << (s.running ? 1 : 0) << '\n';

        writeHeader(out, "liveqx_stress_runs_total",
                    "Stress runs that have completed since process start.",
                    "counter");
        out << "liveqx_stress_runs_total " << s.runs_total << '\n';

        writeHeader(out, "liveqx_stress_passes_total",
                    "Stress runs that finished with pass=true.", "counter");
        out << "liveqx_stress_passes_total " << s.passes_total << '\n';

        writeHeader(out, "liveqx_stress_failures_total",
                    "Stress runs that finished with pass=false.", "counter");
        out << "liveqx_stress_failures_total " << s.failures_total << '\n';

        writeHeader(out, "liveqx_stress_last_started_ms",
                    "Wall-clock ms when the most recent run started "
                    "(0 if never).", "gauge");
        out << "liveqx_stress_last_started_ms " << s.last_started_ms << '\n';

        writeHeader(out, "liveqx_stress_last_ended_ms",
                    "Wall-clock ms when the most recent run ended "
                    "(0 if never).", "gauge");
        out << "liveqx_stress_last_ended_ms " << s.last_ended_ms << '\n';

        writeHeader(out, "liveqx_stress_last_pass",
                    "1 iff the most recent finished run passed.", "gauge");
        out << "liveqx_stress_last_pass " << (s.last_pass ? 1 : 0) << '\n';

        writeHeader(out, "liveqx_stress_reports_count",
                    "Reports currently kept on disk by the report store.",
                    "gauge");
        out << "liveqx_stress_reports_count " << s.reports_count << '\n';

        writeHeader(out, "liveqx_stress_scheduler_armed",
                    "1 iff a cron schedule is currently armed.", "gauge");
        out << "liveqx_stress_scheduler_armed "
            << (s.scheduler_armed ? 1 : 0) << '\n';
    }

    // ── Mounts (fix41) ──────────────────────────────────────────────────────
    // Admin-only feature; cardinality bounded by configured mounts (≪ 100).
    // Per-mount label set kept tight: id + target + fs_type.
    if (mounts_ && (!scope || scope->allowsCrossChannel())) {
        const auto rows = mounts_->listAll();

        int total = 0, active = 0, failed = 0, enabled = 0;

        writeHeader(out, "liveqx_mount_active",
                    "1 iff systemd reports the mount unit as active.",
                    "gauge");
        for (const auto& m : rows) {
            ++total;
            const bool is_active = (m.active_state == "active");
            const bool is_failed = (m.active_state == "failed");
            if (m.enabled) ++enabled;
            if (is_active) ++active;
            if (is_failed) ++failed;

            std::ostringstream l;
            l << "mount_id=\"" << m.id << "\""
              << ",target=\""  << escapeLabel(m.target)  << "\""
              << ",fs_type=\"" << escapeLabel(m.fs_type) << "\"";
            out << "liveqx_mount_active{" << l.str() << "} "
                << (is_active ? 1 : 0) << '\n';
        }

        writeHeader(out, "liveqx_mounts_total",
                    "Mounts known to the catalogue (enabled or not).", "gauge");
        out << "liveqx_mounts_total " << total << '\n';

        writeHeader(out, "liveqx_mounts_enabled",
                    "Mounts with enabled=true (intended to be online).",
                    "gauge");
        out << "liveqx_mounts_enabled " << enabled << '\n';

        writeHeader(out, "liveqx_mounts_active",
                    "Mounts whose systemd active_state is `active`.", "gauge");
        out << "liveqx_mounts_active " << active << '\n';

        writeHeader(out, "liveqx_mounts_failed",
                    "Mounts whose systemd active_state is `failed`.", "gauge");
        out << "liveqx_mounts_failed " << failed << '\n';
    }

    return out.str();
}

namespace {

nlohmann::json buildFeatures() {
    nlohmann::json f;
#ifdef LIVEQX_FEATURE_GPU
    f["gpu"] = true;
#else
    f["gpu"] = false;
#endif
#ifdef LIVEQX_FEATURE_NVENC
    f["nvenc"] = true;
#else
    f["nvenc"] = false;
#endif
#ifdef LIVEQX_FEATURE_QSV
    f["qsv"] = true;
#else
    f["qsv"] = false;
#endif
#ifdef LIVEQX_FEATURE_VAAPI
    f["vaapi"] = true;
#else
    f["vaapi"] = false;
#endif
#ifdef LIVEQX_ENABLE_SYSTEMD
    f["systemd"] = true;
#else
    f["systemd"] = false;
#endif
#ifdef LIVEQX_FEATURE_LDAP
    f["ldap"] = true;
#else
    f["ldap"] = false;
#endif
#ifdef LIVEQX_FEATURE_SMTP
    f["smtp"] = true;
#else
    f["smtp"] = false;
#endif
#ifdef LIVEQX_SIMD
    f["simd"] = LIVEQX_SIMD;
#else
    f["simd"] = "scalar";
#endif
#ifdef LIVEQX_ENABLE_WEBRTC_PREVIEW
    f["preview"] = true;
#else
    f["preview"] = false;
#endif
    // SSE event stream is always compiled in — flag stays exposed so UIs
    // can probe support without parsing version strings.
    f["sse"] = true;
    return f;
}

}  // namespace

// Baseline attributions for compiled-in third-party libraries. Versions
// are read at runtime from the linked libs (av_version_info, srt_getversion,
// OPENSSL_VERSION_TEXT) so they always reflect what's actually loaded —
// no hardcoded numbers to drift out of sync with vcpkg/build_ffmpeg.sh.
//
// Licence labels are static facts about the linked editions:
//   * FFmpeg compiled with --enable-gpl (см. scripts/build_ffmpeg.sh) →
//     эффективная лицензия GPL 2.0+ (libx264 — GPL).
//   * libsrt — Mozilla Public License 2.0.
//   * OpenSSL 3.x — Apache 2.0 (1.1.x был dual OpenSSL/SSLeay).
static std::vector<std::string> coreAttributions() {
    std::vector<std::string> out;
    out.reserve(3);

    out.emplace_back(std::string("FFmpeg ") + av_version_info()
                     + " (GPL 2.0+)");

    {
        const std::uint32_t v = srt_getversion();
        char buf[48];
        std::snprintf(buf, sizeof(buf), "libsrt %u.%u.%u (MPL 2.0)",
                      (v >> 16) & 0xff, (v >> 8) & 0xff, v & 0xff);
        out.emplace_back(buf);
    }

    // OPENSSL_VERSION_TEXT — full string like "OpenSSL 3.0.13 30 Jan 2024".
    // We strip everything after the version number to keep the line tidy.
    {
        std::string ssl = OPENSSL_VERSION_TEXT;
        // "OpenSSL 3.0.13 30 Jan 2024" → "OpenSSL 3.0.13"
        std::size_t first_sp = ssl.find(' ');
        if (first_sp != std::string::npos) {
            std::size_t second_sp = ssl.find(' ', first_sp + 1);
            if (second_sp != std::string::npos) ssl.resize(second_sp);
        }
        out.emplace_back(ssl + " (Apache 2.0)");
    }

    return out;
}

nlohmann::json MetricsCollector::renderVersion() const {
    auto attributions = nlohmann::json::array();
    // Core deps come first — UI рендерит в порядке массива.
    for (const auto& text : coreAttributions()) attributions.push_back(text);
    if (plugins_) {
        for (const auto& text : plugins_->attributions())
            attributions.push_back(text);
    }
    return {
        {"name",         "liveqx"},
        {"version",      build_.version},
        {"build_commit", build_.commit},
        {"build_time",   build_.build_time},
        {"build_type",   build_.build_type},
        {"features",     buildFeatures()},
        {"attributions", std::move(attributions)},
    };
}

nlohmann::json MetricsCollector::renderStatus(
    const liveqx::api::ChannelScope* scope) const {
    using nlohmann::json;
    json out;
    out["version"]      = build_.version;
    out["build_commit"] = build_.commit;
    out["build_time"]   = build_.build_time;

    const auto pm = process_.snapshot();
    const auto now_unix = std::chrono::duration_cast<std::chrono::seconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count();
    out["uptime_seconds"] = (pm.start_time_unix_sec > 0)
        ? (now_unix - pm.start_time_unix_sec)
        : 0;

    json process = json::object();
    if (pm.rss_bytes   >= 0)   process["rss_bytes"]         = pm.rss_bytes;
    if (pm.vsize_bytes >= 0)   process["vsize_bytes"]       = pm.vsize_bytes;
    if (pm.open_fds    >= 0)   process["open_fds"]          = pm.open_fds;
    if (pm.threads     >= 0)   process["threads"]           = pm.threads;
    if (pm.cpu_seconds >= 0.0) process["cpu_seconds_total"] = pm.cpu_seconds;
    out["process"] = std::move(process);

    const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count();

    json channels = json::array();
    int total = 0, running = 0, failed = 0, outputs_total = 0, outputs_failed = 0;

    mgr_.forEachChannel([&](const ChannelInstance& ch) {
        if (scope && !scope->allows(ch.id())) return;
        ++total;
        const auto m_ptr = ch.metrics();
        const auto h_ptr = ch.health();
        json entry;
        entry["id"]   = ch.id();
        entry["name"] = ch.name();
        entry["state"] = ch.isRunning() ? "running" : "stopped";

        if (m_ptr && h_ptr) {
            const auto snap     = m_ptr->snapshot();
            const auto state    = h_ptr->state();
            entry["health"]     = healthStateName(state);
            entry["fps"]        = snap.actual_fps;
            entry["target_fps"] = h_ptr->targetFps();
            entry["transition_active"] = snap.transition_active;
            entry["clip_changes_total"] = snap.clip_changes;
            entry["boundary_drops_total"] = snap.boundary_drops;
            entry["last_tick_age_seconds"] =
                snap.last_tick_ns > 0
                    ? static_cast<double>(now_ns - snap.last_tick_ns) / 1e9
                    : -1.0;
            entry["rolling"] = {
                {"drops_10s",     snap.drops_10s},
                {"underruns_10s", snap.underruns_10s},
                {"rendered_10s",  snap.rendered_10s},
            };
            entry["content_sync"] = {
                {"files",                       snap.cache_files_count},
                {"size_bytes",                  snap.cache_size_bytes},
                {"share_unreachable_total",     snap.share_unreachable_count},
                {"pending_deletes",             snap.pending_deletes},
            };
            if (ch.isRunning()) ++running;
            if (state == HealthState::Failed) ++failed;
        }

        const auto outputs = ch.outputsJson();
        if (outputs.is_array()) {
            json oarr = json::array();
            for (const auto& o : outputs) {
                if (!o.is_object()) continue;
                ++outputs_total;
                const bool healthy = o.value("healthy", false);
                if (!healthy) ++outputs_failed;
                json oo;
                oo["id"]      = o.value("id",   std::string{});
                oo["type"]    = o.contains("transport")
                                  ? o.value("transport", std::string{})
                                  : o.value("type",      std::string{});
                oo["healthy"] = healthy;
                if (o.contains("bytes_sent"))
                    oo["bytes_total"] = o.value("bytes_sent", std::uint64_t{0});
                if (o.contains("queue_drops"))
                    oo["drops_total"] = o.value("queue_drops", std::uint64_t{0});
                if (o.contains("queue_bytes_used"))
                    oo["queue_bytes_used"] = o.value("queue_bytes_used", std::uint64_t{0});
                if (o.contains("queue_bytes_limit"))
                    oo["queue_bytes_limit"] = o.value("queue_bytes_limit", std::uint64_t{0});
                oarr.push_back(std::move(oo));
            }
            entry["outputs"] = std::move(oarr);
        } else {
            entry["outputs"] = json::array();
        }

        channels.push_back(std::move(entry));
    });

    out["channels"] = std::move(channels);

    int gw_total = 0, gw_running = 0;
    if (gateways_ && (!scope || scope->allowsCrossChannel())) {
        out["gateways"] = gateways_->listJson();
        for (const auto& g : out["gateways"]) {
            ++gw_total;
            if (g.value("running", false)) ++gw_running;
        }
    } else {
        out["gateways"] = json::array();
    }

    out["summary"]  = {
        {"channels_total",   total},
        {"channels_running", running},
        {"channels_failed",  failed},
        {"outputs_total",    outputs_total},
        {"outputs_failed",   outputs_failed},
        {"gateways_total",   gw_total},
        {"gateways_running", gw_running},
    };
    return out;
}
