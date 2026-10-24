#include "players/CertLifecycle.h"

namespace liveqx::players {

namespace {
constexpr std::int64_t kDay = 86400;
}

const char* certStageName(CertStage s) noexcept {
    switch (s) {
        case CertStage::Ok:      return "ok";
        case CertStage::Soon30:  return "soon30";
        case CertStage::Soon14:  return "soon14";
        case CertStage::Soon3:   return "soon3";
        case CertStage::Expired: return "expired";
    }
    return "ok";
}

std::optional<CertStage> parseCertStage(const std::string& name) noexcept {
    if (name == "ok")      return CertStage::Ok;
    if (name == "soon30")  return CertStage::Soon30;
    if (name == "soon14")  return CertStage::Soon14;
    if (name == "soon3")   return CertStage::Soon3;
    if (name == "expired") return CertStage::Expired;
    return std::nullopt;
}

std::int64_t certDaysLeft(std::int64_t not_after, std::int64_t now) noexcept {
    const std::int64_t left = not_after - now;
    // Rounded down, also below zero: 1 second past the end is "-1 day", not "0".
    return left >= 0 ? left / kDay : -((-left + kDay - 1) / kDay);
}

CertStage certStageOf(std::optional<std::int64_t> not_after, std::int64_t now) noexcept {
    if (!not_after || *not_after <= now) return CertStage::Expired;
    const std::int64_t left = *not_after - now;
    if (left <= 3 * kDay)  return CertStage::Soon3;
    if (left <= 14 * kDay) return CertStage::Soon14;
    if (left <= 30 * kDay) return CertStage::Soon30;
    return CertStage::Ok;
}

bool certIsSoon(CertStage s) noexcept {
    return s == CertStage::Soon30 || s == CertStage::Soon14 || s == CertStage::Soon3;
}

bool certNeedsAttention(CertStage s) noexcept { return s != CertStage::Ok; }

}  // namespace liveqx::players
