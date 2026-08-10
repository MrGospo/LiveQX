#include "mounts/MountManager.h"

#include <chrono>
#include <utility>

#include <spdlog/spdlog.h>

#include "auth/MasterKey.h"
#include "utils/Log.h"

namespace liveqx::mounts {

namespace {

std::int64_t nowSec() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// Strip one trailing '/' (but keep root "/"). Mirrors UnitGenerator's
// canonicalisation so equality matches systemd's view of the target.
std::string_view stripTrailingSlash(std::string_view s) {
    while (s.size() > 1 && s.back() == '/') s.remove_suffix(1);
    return s;
}

// true if `child` is strictly under `parent` along a `/` boundary.
// "/a/b" is under "/a"  → true
// "/a/bc" is under "/a" → false (no '/' boundary)
// "/a"   is under "/a"  → false (equal, not strict child)
bool isStrictChild(std::string_view parent, std::string_view child) {
    parent = stripTrailingSlash(parent);
    child  = stripTrailingSlash(child);
    if (child.size() <= parent.size() + 1) return false;
    if (child.substr(0, parent.size()) != parent) return false;
    return child[parent.size()] == '/';
}

}  // namespace

std::string MountManager::checkOverlap(const std::string& target,
                                       std::int64_t       ignore_id) {
    const auto rows = db_.listAll();
    for (const auto& r : rows) {
        if (r.id == ignore_id) continue;
        if (isStrictChild(r.target, target)) {
            return "target " + target + " is nested under existing mount "
                   + r.target + " (id=" + std::to_string(r.id) + "); "
                   + "systemd would auto-add Requires=<parent>.mount and the "
                   + "two units would fight each other";
        }
        if (isStrictChild(target, r.target)) {
            return "existing mount " + r.target + " (id=" + std::to_string(r.id)
                   + ") is nested under target " + target + "; "
                   + "systemd would auto-add Requires=<parent>.mount and the "
                   + "two units would fight each other";
        }
    }
    return {};
}

MountManager::MountManager(MountsDb&                          db,
                           const auth::MasterKey&             master_key,
                           std::shared_ptr<IMountdRpcClient>  rpc)
    : db_(db), master_key_(master_key), rpc_(std::move(rpc)) {}

MountPublic MountManager::toPublic(const MountRow& r) {
    MountPublic p;
    p.id            = r.id;
    p.fs_type       = r.fs_type;
    p.source        = r.source;
    p.target        = r.target;
    p.options       = r.options;
    p.ro            = r.ro;
    p.enabled       = r.enabled;
    p.has_password  = r.hasPassword();
    p.cifs_username = r.cifs_username;
    p.cifs_domain   = r.cifs_domain;
    p.created_at    = r.created_at;
    p.updated_at    = r.updated_at;
    p.active_state  = r.last_active_state;
    p.last_status_at = r.last_status_at;
    return p;
}

std::vector<MountPublic> MountManager::listAll() {
    std::vector<MountPublic> out;
    auto rows = db_.listAll();
    out.reserve(rows.size());
    for (const auto& r : rows) out.push_back(toPublic(r));
    return out;
}

std::optional<MountPublic> MountManager::getById(std::int64_t id) {
    auto r = db_.findById(id);
    if (!r) return std::nullopt;
    return toPublic(*r);
}

bool MountManager::decryptPassword(const MountRow& row,
                                   std::string&    out,
                                   std::string&    err) const {
    if (row.cifs_password_blob.empty()) {
        out.clear();
        return true;
    }
    auto dec = master_key_.decrypt(row.cifs_password_blob);
    if (!dec) {
        err = "cannot decrypt CIFS password — wrong master key?";
        return false;
    }
    out = std::move(*dec);
    return true;
}

std::optional<MountSpec> MountManager::getDecryptedSpec(std::int64_t id) {
    auto row = db_.findById(id);
    if (!row) return std::nullopt;
    MountSpec s;
    s.id      = row->id;
    auto fs   = fsTypeFromString(row->fs_type);
    if (!fs) return std::nullopt;
    s.fs_type = *fs;
    s.source  = row->source;
    s.target  = row->target;
    s.options = row->options;
    s.ro      = row->ro;

    if (!row->cifs_username.empty() || !row->cifs_password_blob.empty()) {
        CifsCreds c;
        c.username = row->cifs_username;
        c.domain   = row->cifs_domain;
        std::string pw_err;
        if (!decryptPassword(*row, c.password, pw_err)) {
            LOG_WARN("MountManager: {}", pw_err);
            return std::nullopt;
        }
        s.cifs = std::move(c);
    }
    return s;
}

bool MountManager::toRow(const MountSpec& spec,
                         const MountRow*  existing,
                         MountRow&        out_row,
                         std::string&     err) {
    std::string verr;
    if (!spec.validate(verr)) { err = verr; return false; }

    out_row.id            = existing ? existing->id : 0;
    out_row.fs_type       = toString(spec.fs_type);
    out_row.source        = spec.source;
    out_row.target        = spec.target;
    out_row.options       = spec.options;
    out_row.ro            = spec.ro;
    out_row.cifs_username = spec.cifs.has_value() ? spec.cifs->username : "";
    out_row.cifs_domain   = spec.cifs.has_value() ? spec.cifs->domain   : "";
    out_row.enabled       = existing ? existing->enabled : true;

    // password handling.
    if (spec.cifs.has_value()) {
        const auto& pw = spec.cifs->password;
        if (!pw.empty()) {
            // Replace blob.
            out_row.cifs_password_blob = master_key_.encrypt(pw);
        } else if (existing && !existing->cifs_password_blob.empty()) {
            // Keep existing.
            out_row.cifs_password_blob = existing->cifs_password_blob;
        } else {
            // Guest CIFS or fresh creds without password.
            out_row.cifs_password_blob.clear();
        }
    } else {
        // CIFS → NFS or guest без user.
        out_row.cifs_password_blob.clear();
        out_row.cifs_username.clear();
        out_row.cifs_domain.clear();
    }
    return true;
}

MountOpResult MountManager::addMount(const MountSpec& spec) {
    std::lock_guard<std::mutex> lk(mu_);
    MountOpResult r;
    if (!db_.ok()) { r.error = "mounts.db not open"; r.error_code = "db"; return r; }

    MountRow row;
    if (!toRow(spec, /*existing=*/nullptr, row, r.error)) {
        r.error_code = "invalid";
        return r;
    }

    if (db_.findByTarget(row.target).has_value()) {
        r.error      = "another mount already targets " + row.target;
        r.error_code = "duplicate";
        return r;
    }

    if (auto overlap = checkOverlap(row.target, /*ignore_id=*/0);
        !overlap.empty()) {
        r.error      = overlap;
        r.error_code = "overlap";
        return r;
    }

    auto new_id = db_.insert(row);
    if (!new_id) {
        r.error      = "INSERT failed (target may already exist)";
        r.error_code = "db";
        return r;
    }
    r.id = *new_id;
    row.id = *new_id;

    // Кодим spec с актуальным id и шлём в helper. Если helper упал —
    // строка останется в БД с last_active_state="" и enabled=true; UI
    // покажет «failed/unknown», оператор может ретрай'нуть.
    MountSpec full = spec;
    full.id = *new_id;
    auto resp = rpc_->applyMount(full);
    r.helper_status = resp.status;
    if (!resp.ok) {
        r.helper_error = resp.error;
        r.error_code   = "rpc";
        r.error        = "helper error: " + resp.error;
    } else {
        r.ok = true;
        // Пишем фактический ActiveState чтобы /list сразу показал.
        const auto state = resp.extra.value("active_state", std::string{});
        if (!state.empty()) {
            db_.updateStatus(*new_id, state, nowSec());
        }
    }
    return r;
}

MountOpResult MountManager::updateMount(std::int64_t id, const MountSpec& spec) {
    std::lock_guard<std::mutex> lk(mu_);
    MountOpResult r;
    r.id = id;
    if (!db_.ok()) { r.error = "mounts.db not open"; r.error_code = "db"; return r; }

    auto existing = db_.findById(id);
    if (!existing) { r.error = "id not found"; r.error_code = "not_found"; return r; }

    MountRow row;
    if (!toRow(spec, &*existing, row, r.error)) {
        r.error_code = "invalid";
        return r;
    }
    row.id = id;

    const std::string old_target = existing->target;
    const bool target_changed = (row.target != old_target);

    // Если target меняется — проверим, что коллизии нет.
    if (target_changed) {
        auto clash = db_.findByTarget(row.target);
        if (clash && clash->id != id) {
            r.error      = "another mount already targets " + row.target;
            r.error_code = "duplicate";
            return r;
        }
        if (auto overlap = checkOverlap(row.target, /*ignore_id=*/id);
            !overlap.empty()) {
            r.error      = overlap;
            r.error_code = "overlap";
            return r;
        }
    }

    if (!db_.update(row)) {
        r.error      = "UPDATE failed";
        r.error_code = "db";
        return r;
    }

    // fix43 c4: при смене target прежний basename юнитов (target-derived,
    // см. unitBasenameByTarget) больше не соответствует строке. Если не
    // снять старые .mount/.automount — они останутся в /etc/systemd/system,
    // продолжат удерживать старый target и при reload помешают новому
    // applyMount (а также сделают checkOverlap'у бесполезной защитой
    // — на диске будет призрак). Чистим best-effort: ошибки логируем,
    // но не прерываем update — applyMount всё равно должен пройти.
    if (target_changed) {
        auto rm = rpc_->removeMount(id, old_target);
        if (!rm.ok) {
            LOG_WARN("MountManager: update id={} target changed {} -> {}, "
                     "removeMount(old) failed: {} (continuing with applyMount)",
                     id, old_target, row.target, rm.error);
        }
    }

    MountSpec full = spec;
    full.id = id;
    // Для applyMount нужны сами creds; если update пришёл с пустым
    // password — берём расшифрованный из row (existing-blob может быть
    // не тем же blob'ом, который в new row, но мы уже сохранили его).
    if (full.cifs.has_value() && full.cifs->password.empty()
        && !row.cifs_password_blob.empty()) {
        std::string decoded;
        std::string derr;
        if (!decryptPassword(row, decoded, derr)) {
            r.error      = "cannot decrypt password for helper RPC: " + derr;
            r.error_code = "crypto";
            return r;
        }
        full.cifs->password = std::move(decoded);
    }
    auto resp = rpc_->applyMount(full);
    r.helper_status = resp.status;
    if (!resp.ok) {
        r.helper_error = resp.error;
        r.error_code   = "rpc";
        r.error        = "helper error: " + resp.error;
        return r;
    }
    r.ok = true;
    const auto state = resp.extra.value("active_state", std::string{});
    if (!state.empty()) {
        db_.updateStatus(id, state, nowSec());
    }
    return r;
}

MountOpResult MountManager::removeMount(std::int64_t id) {
    std::lock_guard<std::mutex> lk(mu_);
    MountOpResult r;
    r.id = id;
    if (!db_.ok()) { r.error = "mounts.db not open"; r.error_code = "db"; return r; }

    auto existing = db_.findById(id);
    if (!existing) {
        // Идемпотентно: если строки нет, считаем «уже снят».
        r.ok = true;
        return r;
    }

    auto resp = rpc_->removeMount(id, existing->target);
    if (!resp.ok) {
        // Helper мог упасть на disable —, но юнит уже мог быть снят.
        // Логируем, но БД-row не оставляем — оператор кликнул «Удалить».
        LOG_WARN("MountManager: helper removeMount failed: {} — proceeding to DB delete",
                 resp.error);
        r.helper_error = resp.error;
    }
    if (!db_.deleteById(id)) {
        r.error      = "DB delete failed";
        r.error_code = "db";
        return r;
    }
    r.ok = true;
    return r;
}

MountOpResult MountManager::testMount(const MountSpec& spec) {
    MountOpResult r;
    auto resp = rpc_->testMount(spec);
    r.helper_status = resp.status;
    r.ok = resp.ok;
    r.error = resp.error;
    if (!resp.ok) r.error_code = "rpc";
    return r;
}

void MountManager::syncStatusFromHelper() {
    // fix42: helper не знает, какие mount'ы наши — после перехода на
    // target-derived basename'ы общего префикса нет. Поэтому передаём
    // полный список {id, target} из mounts.db.
    std::vector<StatusQueryItem> items;
    {
        auto rows = db_.listAll();
        items.reserve(rows.size());
        for (const auto& r : rows) {
            items.push_back({r.id, r.target});
        }
    }
    auto resp = rpc_->status(items);
    if (!resp.ok) {
        if (resp.status == "rpc_connect_failed")
            LOG_DEBUG("MountManager: mountd not available yet, skipping status sync");
        else
            LOG_WARN("MountManager: status RPC failed: {}", resp.error);
        return;
    }
    if (!resp.extra.contains("units") || !resp.extra["units"].is_array()) {
        return;
    }
    const auto now = nowSec();
    for (const auto& u : resp.extra["units"]) {
        if (!u.is_object()) continue;
        const auto id    = u.value("id", std::int64_t{0});
        const auto state = u.value("active_state", std::string{});
        if (id <= 0 || state.empty()) continue;
        db_.updateStatus(id, state, now);
    }
}

void MountManager::reconcileOnBoot() {
    auto rows = db_.listAll();
    int applied = 0, skipped = 0, failed = 0;
    for (const auto& row : rows) {
        if (!row.enabled) { ++skipped; continue; }
        auto spec = getDecryptedSpec(row.id);
        if (!spec) {
            LOG_WARN("MountManager: cannot decrypt spec for id={} on reconcile", row.id);
            ++failed;
            continue;
        }
        auto resp = rpc_->applyMount(*spec);
        if (!resp.ok) {
            if (resp.status == "rpc_connect_failed")
                LOG_DEBUG("MountManager: reconcile id={} — mountd not available, will retry via status sync",
                         row.id);
            else
                LOG_WARN("MountManager: reconcile applyMount(id={}) failed: {}",
                         row.id, resp.error);
            ++failed;
            continue;
        }
        const auto state = resp.extra.value("active_state", std::string{});
        if (!state.empty()) db_.updateStatus(row.id, state, nowSec());
        ++applied;
    }
    LOG_INFO("MountManager: reconcile applied={} skipped={} failed={} (total={})",
             applied, skipped, failed, rows.size());
}

std::size_t MountManager::size() {
    return db_.listAll().size();
}

}  // namespace liveqx::mounts
