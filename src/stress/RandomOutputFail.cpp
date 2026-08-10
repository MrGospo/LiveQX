#include "stress/RandomOutputFail.h"

#include <algorithm>

#include <spdlog/spdlog.h>

#include "api/ChannelManager.h"

namespace liveqx::stress {

namespace {

std::int64_t pick_in_range(std::mt19937_64& rng, std::int64_t lo, std::int64_t hi) {
    if (hi <= lo) return lo;
    std::uniform_int_distribution<std::int64_t> d(lo, hi);
    return d(rng);
}

}  // namespace

void RandomOutputFail::scheduleNextFail(ScenarioContext& ctx, std::int64_t now_ms) {
    next_fail_at_ms_ = now_ms +
        pick_in_range(ctx.rng, iv_.fail_min_ms, iv_.fail_max_ms);
}

void RandomOutputFail::onStart(ScenarioContext& ctx) {
    scheduleNextFail(ctx, ctx.run_started_ms);
}

void RandomOutputFail::tick(ScenarioContext& ctx,
                            std::int64_t now_ms,
                            std::int64_t /*elapsed_ms*/,
                            std::vector<ScenarioEvent>& out) {
    // Restore takes priority — if a kill is pending and its restore window
    // has elapsed, recover it first so we don't pile up multiple kills.
    if (pending_ && now_ms >= pending_->restore_at_ms) {
        doRestore(ctx, now_ms, out);
    }
    if (!pending_ && now_ms >= next_fail_at_ms_) {
        doKill(ctx, now_ms, out);
    }
}

void RandomOutputFail::onFinish(ScenarioContext& ctx,
                                std::vector<ScenarioEvent>& out) {
    // If a kill is still in flight when the run ends, restore it now so
    // teardown sees a clean state.
    if (pending_) {
        doRestore(ctx, /*now_ms=*/pending_->restore_at_ms, out);
    }
    spdlog::info("stress[{}]: kills={} restores={}", name(), kills_, restores_);
}

void RandomOutputFail::doKill(ScenarioContext& ctx,
                              std::int64_t now_ms,
                              std::vector<ScenarioEvent>& out) {
    if (!ctx.mgr || ctx.channel_ids.empty()) {
        scheduleNextFail(ctx, now_ms);
        return;
    }
    // Pick a random channel and a random output on it.
    std::uniform_int_distribution<std::size_t> ch_d(0, ctx.channel_ids.size() - 1);
    const int channel_id = ctx.channel_ids[ch_d(ctx.rng)];
    auto outs = ctx.mgr->outputsJson(channel_id);
    if (!outs.is_array() || outs.empty()) {
        ScenarioEvent ev;
        ev.ts_ms    = now_ms;
        ev.scenario = name();
        ev.detail   = "channel " + std::to_string(channel_id) + " has no outputs to kill";
        ev.ok       = false;
        out.push_back(std::move(ev));
        scheduleNextFail(ctx, now_ms);
        return;
    }
    std::uniform_int_distribution<std::size_t> o_d(0, outs.size() - 1);
    const auto& target = outs[o_d(ctx.rng)];
    if (!target.contains("id") || !target["id"].is_string()) {
        ScenarioEvent ev;
        ev.ts_ms    = now_ms;
        ev.scenario = name();
        ev.detail   = "selected output on channel " + std::to_string(channel_id)
                    + " has no string id";
        ev.ok       = false;
        out.push_back(std::move(ev));
        scheduleNextFail(ctx, now_ms);
        return;
    }
    Pending p;
    p.channel_id    = channel_id;
    p.output_id     = target["id"].get<std::string>();
    p.saved_cfg     = target;          // outputsJson returns a "build-able" obj
    p.restore_at_ms = now_ms + pick_in_range(ctx.rng, iv_.restore_min_ms,
                                              iv_.restore_max_ms);
    auto rr = ctx.mgr->removeOutput(channel_id, p.output_id);
    ScenarioEvent ev;
    ev.ts_ms    = now_ms;
    ev.scenario = name();
    if (rr != ChannelManager::Result::Ok) {
        ev.ok     = false;
        ev.detail = "removeOutput(" + std::to_string(channel_id) + ", "
                  + p.output_id + ") failed: code "
                  + std::to_string(static_cast<int>(rr));
        out.push_back(std::move(ev));
        scheduleNextFail(ctx, now_ms);
        return;
    }
    ++kills_;
    ev.ok     = true;
    ev.detail = "killed output " + p.output_id + " on channel "
              + std::to_string(channel_id) + " (restore in "
              + std::to_string((p.restore_at_ms - now_ms) / 1000) + "s)";
    out.push_back(std::move(ev));
    pending_ = std::move(p);
    // Don't schedule the next fail until after restore — only one in flight.
}

void RandomOutputFail::doRestore(ScenarioContext& ctx,
                                 std::int64_t now_ms,
                                 std::vector<ScenarioEvent>& out) {
    if (!pending_ || !ctx.mgr) {
        pending_.reset();
        return;
    }
    Pending p = std::move(*pending_);
    pending_.reset();
    auto rr = ctx.mgr->addOutput(p.channel_id, p.saved_cfg);
    ScenarioEvent ev;
    ev.ts_ms    = now_ms;
    ev.scenario = name();
    if (rr != ChannelManager::Result::Ok) {
        ev.ok     = false;
        ev.detail = "addOutput(" + std::to_string(p.channel_id) + ", "
                  + p.output_id + ") failed: code "
                  + std::to_string(static_cast<int>(rr));
    } else {
        ++restores_;
        ev.ok     = true;
        ev.detail = "restored output " + p.output_id + " on channel "
                  + std::to_string(p.channel_id);
    }
    out.push_back(std::move(ev));
    scheduleNextFail(ctx, now_ms);
}

}  // namespace liveqx::stress
