// fix26 c8 — clip_corrupt scenario.
//
// Periodically picks a random file from a caller-provided candidate list,
// reads the original bytes into memory, truncates the file to 0 bytes
// (forcing ClipFactory to fail and FallbackClip to take over on the next
// rotation), and restores the bytes after restore_window. Only one
// corruption is in flight at a time.
//
// Path candidates are injected via cfg.scenario_options["clip_corrupt"]
// = {"paths": ["..."]}. The runner's cfg builder (c13) is responsible
// for populating this list — the scenario itself doesn't scan the
// filesystem so it stays trivially testable and side-effect-free
// outside the named paths.
//
// Files larger than kMaxFileBackupBytes are skipped at kill time with
// an ok=false event — backing up a multi-GB clip into RAM would dwarf
// the rest of the runtime working set.

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

#include "stress/Scenario.h"

namespace liveqx::stress {

struct ClipCorruptIntervals {
    std::int64_t fail_min_ms    = 60 * 60 * 1000;   // every 60..60 minutes
    std::int64_t fail_max_ms    = 60 * 60 * 1000;
    std::int64_t restore_min_ms = 60 * 1000;        // restore 1..5 minutes later
    std::int64_t restore_max_ms = 5  * 60 * 1000;
};

class ClipCorrupt final : public IScenario {
public:
    static constexpr std::size_t kMaxFileBackupBytes = 100ull * 1024 * 1024;

    ClipCorrupt(std::vector<std::filesystem::path> candidates,
                ClipCorruptIntervals iv = {})
        : candidates_(std::move(candidates)), iv_(iv) {}

    std::string name() const override { return "clip_corrupt"; }

    void onStart(ScenarioContext& ctx) override;
    void tick(ScenarioContext& ctx,
              std::int64_t now_ms,
              std::int64_t elapsed_ms,
              std::vector<ScenarioEvent>& out) override;
    void onFinish(ScenarioContext& ctx,
                  std::vector<ScenarioEvent>& out) override;

private:
    struct Pending {
        std::filesystem::path path;
        std::vector<std::uint8_t> backup;
        std::int64_t  restore_at_ms = 0;
    };

    void scheduleNext(ScenarioContext& ctx, std::int64_t now_ms);
    void doKill(ScenarioContext& ctx, std::int64_t now_ms,
                std::vector<ScenarioEvent>& out);
    void doRestore(ScenarioContext& ctx, std::int64_t now_ms,
                   std::vector<ScenarioEvent>& out);

    std::vector<std::filesystem::path> candidates_;
    ClipCorruptIntervals               iv_{};
    std::int64_t                       next_fail_at_ms_ = 0;
    std::optional<Pending>             pending_;
    int                                kills_   = 0;
    int                                restores_= 0;
};

}  // namespace liveqx::stress
