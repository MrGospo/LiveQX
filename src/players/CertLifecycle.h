#pragma once
//
// How close a screen's certificate is to its end, in the steps the interface and
// the alerts speak of: more than 30 days, 30, 14, 3, and over.
//
// A screen that is switched on renews by itself 60 days before the end, so a
// screen that reaches 30 days is one that has NOT renewed in a month — switched
// off, or failing to renew. That is why the steps start there, not earlier.
// A screen that stays off past the end cannot renew any more (renewal needs the
// certificate it has lost); it can only be bound again with a new token.

#include <cstdint>
#include <optional>
#include <string>

namespace liveqx::players {

enum class CertStage { Ok, Soon30, Soon14, Soon3, Expired };

// The word used in the API and in the journal: ok, soon30, soon14, soon3, expired.
const char* certStageName(CertStage s) noexcept;
std::optional<CertStage> parseCertStage(const std::string& name) noexcept;

// `not_after` unix seconds; nullopt = the screen has no valid certificate at all
// (every one revoked, or the record gone after it had run out) — the same as over.
CertStage certStageOf(std::optional<std::int64_t> not_after, std::int64_t now) noexcept;

// Whole days left, rounded down; negative once the end is past.
std::int64_t certDaysLeft(std::int64_t not_after, std::int64_t now) noexcept;

// Any of the three "soon" steps.
bool certIsSoon(CertStage s) noexcept;
// Soon, or over: what someone should look at.
bool certNeedsAttention(CertStage s) noexcept;

}  // namespace liveqx::players
