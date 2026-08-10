#include "stress/ClipCorrupt.h"

#include <fstream>
#include <random>
#include <system_error>

#include <spdlog/spdlog.h>

namespace fs = std::filesystem;

namespace liveqx::stress {

namespace {

std::int64_t pick_in_range(std::mt19937_64& rng, std::int64_t lo, std::int64_t hi) {
    if (hi <= lo) return lo;
    std::uniform_int_distribution<std::int64_t> d(lo, hi);
    return d(rng);
}

}  // namespace

void ClipCorrupt::scheduleNext(ScenarioContext& ctx, std::int64_t now_ms) {
    next_fail_at_ms_ = now_ms +
        pick_in_range(ctx.rng, iv_.fail_min_ms, iv_.fail_max_ms);
}

void ClipCorrupt::onStart(ScenarioContext& ctx) {
    if (candidates_.empty()) {
        spdlog::warn("stress[clip_corrupt]: no candidate paths — scenario will no-op");
    }
    scheduleNext(ctx, ctx.run_started_ms);
}

void ClipCorrupt::tick(ScenarioContext& ctx,
                       std::int64_t now_ms,
                       std::int64_t /*elapsed_ms*/,
                       std::vector<ScenarioEvent>& out) {
    if (pending_ && now_ms >= pending_->restore_at_ms) {
        doRestore(ctx, now_ms, out);
    }
    if (!pending_ && now_ms >= next_fail_at_ms_ && !candidates_.empty()) {
        doKill(ctx, now_ms, out);
    }
}

void ClipCorrupt::onFinish(ScenarioContext& ctx,
                           std::vector<ScenarioEvent>& out) {
    if (pending_) {
        doRestore(ctx, /*now_ms=*/pending_->restore_at_ms, out);
    }
    spdlog::info("stress[clip_corrupt]: kills={} restores={}", kills_, restores_);
}

void ClipCorrupt::doKill(ScenarioContext& ctx,
                         std::int64_t now_ms,
                         std::vector<ScenarioEvent>& out) {
    std::uniform_int_distribution<std::size_t> d(0, candidates_.size() - 1);
    const auto& path = candidates_[d(ctx.rng)];

    ScenarioEvent ev;
    ev.ts_ms    = now_ms;
    ev.scenario = name();

    std::error_code ec;
    if (!fs::exists(path, ec) || ec) {
        ev.ok     = false;
        ev.detail = "candidate path does not exist: " + path.string();
        out.push_back(std::move(ev));
        scheduleNext(ctx, now_ms);
        return;
    }
    const auto sz = fs::file_size(path, ec);
    if (ec) {
        ev.ok     = false;
        ev.detail = "cannot stat " + path.string() + ": " + ec.message();
        out.push_back(std::move(ev));
        scheduleNext(ctx, now_ms);
        return;
    }
    if (sz > kMaxFileBackupBytes) {
        ev.ok     = false;
        ev.detail = "skipping " + path.string() + " — file too large to back up ("
                  + std::to_string(sz) + " bytes)";
        out.push_back(std::move(ev));
        scheduleNext(ctx, now_ms);
        return;
    }

    Pending p;
    p.path = path;
    p.backup.resize(static_cast<std::size_t>(sz));
    {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            ev.ok     = false;
            ev.detail = "open-for-read failed: " + path.string();
            out.push_back(std::move(ev));
            scheduleNext(ctx, now_ms);
            return;
        }
        if (sz > 0) {
            in.read(reinterpret_cast<char*>(p.backup.data()),
                    static_cast<std::streamsize>(sz));
            if (in.gcount() != static_cast<std::streamsize>(sz)) {
                ev.ok     = false;
                ev.detail = "short read on " + path.string();
                out.push_back(std::move(ev));
                scheduleNext(ctx, now_ms);
                return;
            }
        }
    }
    {
        // Truncate to 0 bytes — ClipFactory's open path will fail and
        // FallbackClip should kick in on the next rotation.
        std::ofstream out_f(path, std::ios::binary | std::ios::trunc);
        if (!out_f) {
            ev.ok     = false;
            ev.detail = "open-for-truncate failed: " + path.string();
            out.push_back(std::move(ev));
            scheduleNext(ctx, now_ms);
            return;
        }
    }
    p.restore_at_ms = now_ms + pick_in_range(ctx.rng, iv_.restore_min_ms,
                                              iv_.restore_max_ms);
    ev.ok     = true;
    ev.detail = "corrupted " + path.string() + " (was " + std::to_string(sz)
              + " bytes, restore in "
              + std::to_string((p.restore_at_ms - now_ms) / 1000) + "s)";
    out.push_back(std::move(ev));
    pending_ = std::move(p);
    ++kills_;
}

void ClipCorrupt::doRestore(ScenarioContext& ctx,
                            std::int64_t now_ms,
                            std::vector<ScenarioEvent>& out) {
    if (!pending_) return;
    Pending p = std::move(*pending_);
    pending_.reset();

    ScenarioEvent ev;
    ev.ts_ms    = now_ms;
    ev.scenario = name();

    std::ofstream out_f(p.path, std::ios::binary | std::ios::trunc);
    if (!out_f) {
        ev.ok     = false;
        ev.detail = "open-for-restore failed: " + p.path.string();
        out.push_back(std::move(ev));
        scheduleNext(ctx, now_ms);
        return;
    }
    if (!p.backup.empty()) {
        out_f.write(reinterpret_cast<const char*>(p.backup.data()),
                    static_cast<std::streamsize>(p.backup.size()));
        if (!out_f) {
            ev.ok     = false;
            ev.detail = "short write on restore: " + p.path.string();
            out.push_back(std::move(ev));
            scheduleNext(ctx, now_ms);
            return;
        }
    }
    ev.ok     = true;
    ev.detail = "restored " + p.path.string() + " ("
              + std::to_string(p.backup.size()) + " bytes)";
    out.push_back(std::move(ev));
    ++restores_;
    scheduleNext(ctx, now_ms);
}

}  // namespace liveqx::stress
