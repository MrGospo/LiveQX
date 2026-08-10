#include "stress/NetworkBlackout.h"

#include <cstdlib>
#include <random>
#include <unistd.h>

#include <spdlog/spdlog.h>

namespace liveqx::stress {

namespace {

std::int64_t pick_in_range(std::mt19937_64& rng, std::int64_t lo, std::int64_t hi) {
    if (hi <= lo) return lo;
    std::uniform_int_distribution<std::int64_t> d(lo, hi);
    return d(rng);
}

int defaultTcRunner(const std::string& cmd) {
    // 2>/dev/null so spurious tc warnings don't pollute stress logs;
    // we still log success/failure based on the exit code.
    return std::system((cmd + " 2>/dev/null").c_str());
}

bool defaultPrivCheck() {
    return ::geteuid() == 0;
}

}  // namespace

NetworkBlackout::NetworkBlackout(std::vector<std::string> interfaces,
                                 NetworkBlackoutIntervals iv,
                                 TcRunner runner,
                                 PrivCheck priv)
    : interfaces_(std::move(interfaces)),
      iv_(iv),
      runner_(runner ? std::move(runner) : TcRunner{&defaultTcRunner}),
      priv_(priv ? std::move(priv) : PrivCheck{&defaultPrivCheck}) {}

void NetworkBlackout::scheduleNext(ScenarioContext& ctx, std::int64_t now_ms) {
    next_fail_at_ms_ = now_ms +
        pick_in_range(ctx.rng, iv_.fail_min_ms, iv_.fail_max_ms);
}

void NetworkBlackout::onStart(ScenarioContext& ctx) {
    privileged_ = priv_ ? priv_() : false;
    if (interfaces_.empty()) {
        spdlog::warn("stress[network_blackout]: no interfaces — scenario will no-op");
    } else if (!privileged_) {
        spdlog::warn("stress[network_blackout]: not root (CAP_NET_ADMIN required)"
                     " — kills will be skipped with ok=false events");
    }
    scheduleNext(ctx, ctx.run_started_ms);
}

void NetworkBlackout::tick(ScenarioContext& ctx,
                           std::int64_t now_ms,
                           std::int64_t /*elapsed_ms*/,
                           std::vector<ScenarioEvent>& out) {
    if (pending_ && now_ms >= pending_->restore_at_ms) {
        doRestore(ctx, now_ms, out);
    }
    if (!pending_ && now_ms >= next_fail_at_ms_ && !interfaces_.empty()) {
        doKill(ctx, now_ms, out);
    }
}

void NetworkBlackout::onFinish(ScenarioContext& ctx,
                               std::vector<ScenarioEvent>& out) {
    if (pending_) {
        // Force restore at finish to avoid leaving a dangling qdisc.
        doRestore(ctx, /*now_ms=*/pending_->restore_at_ms, out);
    }
    spdlog::info("stress[network_blackout]: kills={} restores={} privileged={}",
                 kills_, restores_, privileged_);
}

void NetworkBlackout::doKill(ScenarioContext& ctx,
                             std::int64_t now_ms,
                             std::vector<ScenarioEvent>& out) {
    std::uniform_int_distribution<std::size_t> d(0, interfaces_.size() - 1);
    const auto& iface = interfaces_[d(ctx.rng)];

    ScenarioEvent ev;
    ev.ts_ms    = now_ms;
    ev.scenario = name();

    if (!privileged_) {
        ev.ok     = false;
        ev.detail = "skipped " + iface + " — needs CAP_NET_ADMIN (run as root)";
        out.push_back(std::move(ev));
        scheduleNext(ctx, now_ms);
        return;
    }

    const std::string cmd = "tc qdisc add dev " + iface +
                            " root netem loss 100%";
    const int rc = runner_ ? runner_(cmd) : -1;
    if (rc != 0) {
        ev.ok     = false;
        ev.detail = "tc add failed for " + iface + " (rc=" + std::to_string(rc)
                  + ", cmd: " + cmd + ")";
        out.push_back(std::move(ev));
        scheduleNext(ctx, now_ms);
        return;
    }

    Pending p;
    p.iface         = iface;
    p.restore_at_ms = now_ms + pick_in_range(ctx.rng, iv_.blackout_min_ms,
                                              iv_.blackout_max_ms);
    ev.ok     = true;
    ev.detail = "blackout on " + iface + " (clear in "
              + std::to_string((p.restore_at_ms - now_ms) / 1000) + "s)";
    out.push_back(std::move(ev));
    pending_ = std::move(p);
    ++kills_;
}

void NetworkBlackout::doRestore(ScenarioContext& ctx,
                                std::int64_t now_ms,
                                std::vector<ScenarioEvent>& out) {
    if (!pending_) return;
    Pending p = std::move(*pending_);
    pending_.reset();

    ScenarioEvent ev;
    ev.ts_ms    = now_ms;
    ev.scenario = name();

    const std::string cmd = "tc qdisc del dev " + p.iface + " root";
    const int rc = runner_ ? runner_(cmd) : -1;
    if (rc != 0) {
        ev.ok     = false;
        ev.detail = "tc del failed for " + p.iface + " (rc=" + std::to_string(rc)
                  + ") — leaving qdisc, manual cleanup may be needed";
        out.push_back(std::move(ev));
        scheduleNext(ctx, now_ms);
        return;
    }
    ev.ok     = true;
    ev.detail = "restored " + p.iface;
    out.push_back(std::move(ev));
    ++restores_;
    scheduleNext(ctx, now_ms);
}

}  // namespace liveqx::stress
