// fix22 commit 17/24 — LdapConfigRepo (см. LdapConfigRepo.h).

#include "auth/LdapConfigRepo.h"

#include <chrono>

#include <nlohmann/json.hpp>

#include "utils/Log.h"

namespace liveqx::auth {

using nlohmann::json;

namespace {

std::int64_t nowUnixSec() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

}  // namespace

LdapConfigRepo::LdapConfigRepo(AuthDb& db, MasterKey& master_key)
    : db_(db), master_key_(master_key) {}

std::string LdapConfigRepo::tlsModeToString(LdapTlsMode m) noexcept {
    switch (m) {
        case LdapTlsMode::Plain:    return "plain";
        case LdapTlsMode::StartTls: return "starttls";
        case LdapTlsMode::Ldaps:    return "ldaps";
    }
    return "starttls";
}

std::optional<LdapTlsMode>
LdapConfigRepo::tlsModeFromString(std::string_view s) noexcept {
    if (s == "plain")    return LdapTlsMode::Plain;
    if (s == "starttls") return LdapTlsMode::StartTls;
    if (s == "ldaps")    return LdapTlsMode::Ldaps;
    return std::nullopt;
}

std::string LdapConfigRepo::groupRoleMapToJson(
    const std::map<std::string, Role>& m) {
    json j = json::object();
    for (const auto& [k, v] : m) j[k] = roleName(v);
    return j.dump();
}

std::map<std::string, Role>
LdapConfigRepo::groupRoleMapFromJson(std::string_view s) {
    std::map<std::string, Role> out;
    if (s.empty()) return out;
    try {
        json j = json::parse(s);
        if (!j.is_object()) return out;
        for (auto it = j.begin(); it != j.end(); ++it) {
            if (!it.value().is_string()) continue;
            auto r = roleFromString(it.value().get<std::string>());
            if (r.has_value()) out[it.key()] = *r;
        }
    } catch (const json::exception& e) {
        LOG_WARN("LdapConfigRepo: group_role_map_json parse error: {}", e.what());
    }
    return out;
}

std::string LdapConfigRepo::channelAclToJson(
    const std::vector<LdapConfig::ChannelAcl>& v) {
    json j = json::array();
    for (const auto& acl : v) {
        json item;
        item["channel_id"] = acl.channel_id;
        json groups = json::object();
        for (const auto& [dn, perm] : acl.group_perms) {
            groups[dn] = channelPermissionName(perm);
        }
        item["groups"] = std::move(groups);
        j.push_back(std::move(item));
    }
    return j.dump();
}

std::vector<LdapConfig::ChannelAcl>
LdapConfigRepo::channelAclFromJson(std::string_view s) {
    std::vector<LdapConfig::ChannelAcl> out;
    if (s.empty()) return out;
    try {
        json j = json::parse(s);
        if (!j.is_array()) return out;
        for (const auto& item : j) {
            if (!item.is_object()) continue;
            LdapConfig::ChannelAcl acl;
            acl.channel_id = item.value("channel_id", std::int64_t{0});
            if (item.contains("groups") && item["groups"].is_object()) {
                for (auto it = item["groups"].begin();
                     it != item["groups"].end(); ++it) {
                    if (!it.value().is_string()) continue;
                    auto p = channelPermissionFromString(
                        it.value().get<std::string>());
                    if (p.has_value()) acl.group_perms[it.key()] = *p;
                }
            }
            out.push_back(std::move(acl));
        }
    } catch (const json::exception& e) {
        LOG_WARN("LdapConfigRepo: channel_acl_json parse error: {}", e.what());
    }
    return out;
}

std::optional<LdapConfig> LdapConfigRepo::load() {
    auto row = db_.readLdapConfigRow();
    if (!row.has_value()) return std::nullopt;

    LdapConfig cfg;
    cfg.enabled                   = row->enabled;
    cfg.server                    = row->server;
    if (auto m = tlsModeFromString(row->tls_mode); m.has_value()) {
        cfg.tls_mode = *m;
    }
    cfg.base_dn                   = row->base_dn;
    cfg.bind_dn                   = row->bind_dn;
    cfg.user_filter               = row->user_filter.empty()
                                      ? std::string("(uid=%s)")
                                      : row->user_filter;
    cfg.group_attribute           = row->group_attribute.empty()
                                      ? std::string("memberOf")
                                      : row->group_attribute;
    cfg.recheck_groups_on_refresh = row->recheck_groups_on_refresh;
    cfg.network_timeout_sec       = row->network_timeout_sec;
    cfg.group_role_map            = groupRoleMapFromJson(row->group_role_map_json);
    cfg.channel_acl               = channelAclFromJson(row->channel_acl_json);

    if (!row->bind_password_encrypted.empty()) {
        if (!master_key_.loaded()) {
            // Без master key мы не можем расшифровать — но возвращаем
            // cfg с пустым bind_password, потому что не-encrypted поля
            // (например, server/base_dn) всё ещё нужны для UI/доступа.
            LOG_WARN("LdapConfigRepo: master key not loaded — "
                     "bind_password left empty");
        } else {
            auto pw = master_key_.decrypt(row->bind_password_encrypted);
            if (pw.has_value()) {
                cfg.bind_password = *pw;
            } else {
                LOG_ERROR("LdapConfigRepo: cannot decrypt bind_password "
                          "(MAC failed or wrong key)");
            }
        }
    }
    return cfg;
}

bool LdapConfigRepo::save(const LdapConfig& cfg,
                          std::optional<std::int64_t> updated_by_user_id) {
    AuthDb::LdapConfigRow row;
    row.enabled                   = cfg.enabled;
    row.server                    = cfg.server;
    row.tls_mode                  = tlsModeToString(cfg.tls_mode);
    row.base_dn                   = cfg.base_dn;
    row.bind_dn                   = cfg.bind_dn;
    row.user_filter               = cfg.user_filter;
    row.group_attribute           = cfg.group_attribute;
    row.group_role_map_json       = groupRoleMapToJson(cfg.group_role_map);
    row.channel_acl_json          = channelAclToJson(cfg.channel_acl);
    row.recheck_groups_on_refresh = cfg.recheck_groups_on_refresh;
    row.network_timeout_sec       = cfg.network_timeout_sec;
    row.updated_at                = nowUnixSec();
    row.updated_by                = updated_by_user_id;

    if (!cfg.bind_password.empty()) {
        if (!master_key_.loaded()) {
            LOG_ERROR("LdapConfigRepo::save: master key not loaded — "
                      "refusing to store bind_password as plaintext");
            return false;
        }
        row.bind_password_encrypted = master_key_.encrypt(cfg.bind_password);
        if (row.bind_password_encrypted.empty()) {
            LOG_ERROR("LdapConfigRepo::save: encrypt of bind_password "
                      "returned empty blob");
            return false;
        }
    } else {
        // Empty plain → пустой ciphertext в БД (=> bind anonymous).
        row.bind_password_encrypted.clear();
    }

    if (!db_.writeLdapConfigRow(row)) {
        LOG_ERROR("LdapConfigRepo::save: writeLdapConfigRow failed");
        return false;
    }
    return true;
}

}  // namespace liveqx::auth
