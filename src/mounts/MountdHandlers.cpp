#include "mounts/MountdHandlers.h"

#include <cerrno>
#include <cstring>
#include <fstream>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <unistd.h>

#include <spdlog/spdlog.h>

namespace fs = std::filesystem;

namespace liveqx::mounts {

MountdHandlers::MountdHandlers(MountdHandlersConfig         cfg,
                               std::shared_ptr<ISystemctl>  systemctl,
                               std::shared_ptr<ICredsHelper> creds,
                               std::shared_ptr<IMountTester> tester)
    : cfg_(std::move(cfg)),
      systemctl_(std::move(systemctl)),
      creds_(std::move(creds)),
      tester_(std::move(tester)) {}

bool MountdHandlers::writeUnitFile(const fs::path&    path,
                                   const std::string& content,
                                   std::string&       err) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) {
        err = "mkdir " + path.parent_path().string() + ": " + ec.message();
        return false;
    }

    const auto tmp = path.string() + ".tmp";
    int fd = ::open(tmp.c_str(),
                    O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
                    0644);
    if (fd < 0) {
        err = "open " + tmp + ": " + std::strerror(errno);
        return false;
    }
    const char* p = content.data();
    std::size_t left = content.size();
    while (left > 0) {
        ssize_t n = ::write(fd, p, left);
        if (n < 0) {
            if (errno == EINTR) continue;
            err = "write " + tmp + ": " + std::strerror(errno);
            ::close(fd);
            ::unlink(tmp.c_str());
            return false;
        }
        p    += n;
        left -= static_cast<std::size_t>(n);
    }
    if (::fsync(fd) < 0) {
        // fsync на tmpfs / overlayfs может не поддерживаться — это
        // не считаем фатальным.
    }
    ::close(fd);

    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        err = "rename " + tmp + " -> " + path.string() + ": "
              + std::strerror(errno);
        ::unlink(tmp.c_str());
        return false;
    }
    return true;
}

bool MountdHandlers::removeIfExists(const fs::path& path, std::string& err) {
    std::error_code ec;
    if (!fs::exists(path, ec)) return true;
    fs::remove(path, ec);
    if (ec) {
        err = "unlink " + path.string() + ": " + ec.message();
        return false;
    }
    return true;
}

RpcResponse MountdHandlers::handleApply(const MountSpec& spec) {
    std::string verr;
    if (!spec.validate(verr)) {
        return RpcResponse::fail("invalid spec: " + verr, "invalid");
    }
    if (spec.id <= 0) {
        return RpcResponse::fail("apply requires id > 0", "invalid");
    }

    auto g = generateUnits(spec, cfg_.unit_ctx);

    // fix42-migration: pre-fix42 хелпер писал .mount/.automount с
    // id-based basename'ом ("liveqx-mnt-<id>"). systemd такие
    // юниты отказывался загружать ("Where= doesn't match unit name"),
    // но файлы всё равно оставались на диске. Гасим их перед записью
    // новых, чтобы при daemon-reload не оставалось зомби-юнитов.
    const std::string legacy_basename = legacyUnitBasename(spec.id);
    if (legacy_basename != g.unit_basename) {
        const auto unit_dir = cfg_.unit_ctx.systemd_unit_dir;
        const auto legacy_mount     = unit_dir / (legacy_basename + ".mount");
        const auto legacy_automount = unit_dir / (legacy_basename + ".automount");
        std::error_code ec;
        // disable --now на нерабочий unit вернёт not-found — это ОК, метод
        // молча проглотит.
        if (std::filesystem::exists(legacy_automount, ec)) {
            std::string derr;
            (void) systemctl_->disableNow(legacy_basename + ".automount", derr);
        }
        if (std::filesystem::exists(legacy_mount, ec)) {
            std::string derr;
            (void) systemctl_->disableNow(legacy_basename + ".mount", derr);
        }
        std::string ferr;
        (void) removeIfExists(legacy_mount,     ferr);
        (void) removeIfExists(legacy_automount, ferr);
    }

    // fix43-migration: pre-fix43 хелпер писал зашифрованный cred-файл в
    // /etc/credstore.encrypted/<credFilename>.cred. Теперь cred живёт на
    // tmpfs под cred_dir, но zombie-blob с паролем не должен оставаться
    // на диске. Удаляем независимо от того, нужны ли новые creds.
    {
        const std::filesystem::path legacy_credstore = "/etc/credstore.encrypted";
        std::string ferr;
        (void) creds_->removeFile(legacy_credstore / (credFilename(spec.id) + ".cred"), ferr);
    }

    // Шаг 1: cred (если нужен) — перед записью юнита, чтобы новый юнит,
    // прочитанный daemon-reload'ом, ссылался на уже существующий файл.
    if (g.needs_credentials()) {
        std::string err;
        if (!creds_->writeFile(g.cred_payload, g.cred_path, err)) {
            return RpcResponse::fail("creds: " + err, "creds_failed");
        }
    } else {
        // Если ранее были креды (CIFS → guest), удаляем их файл — чтобы
        // на tmpfs не лежал зомби с паролем.
        std::string ferr;
        const auto stale = cfg_.unit_ctx.cred_dir
                           / (credFilename(spec.id) + ".cred");
        (void) creds_->removeFile(stale, ferr);
    }

    // Шаг 2: запись .mount + .automount юнитов.
    const auto unit_dir = cfg_.unit_ctx.systemd_unit_dir;
    const auto mount_path     = unit_dir / (g.unit_basename + ".mount");
    const auto automount_path = unit_dir / (g.unit_basename + ".automount");

    {
        std::string err;
        if (!writeUnitFile(mount_path, g.mount_unit, err)) {
            return RpcResponse::fail("write mount unit: " + err, "io_failed");
        }
    }
    if (!g.automount_unit.empty()) {
        std::string err;
        if (!writeUnitFile(automount_path, g.automount_unit, err)) {
            return RpcResponse::fail("write automount unit: " + err, "io_failed");
        }
    } else {
        // Гасим возможный legacy-файл .automount, если режим был сменён.
        std::string err;
        (void) removeIfExists(automount_path, err);
    }

    // Шаг 3: systemctl daemon-reload + enable --now <basename>.<auto|mount>.
    {
        std::string err;
        if (!systemctl_->daemonReload(err)) {
            return RpcResponse::fail("daemon-reload: " + err, "systemd_failed");
        }
    }

    const std::string mount_unit = g.unit_basename + ".mount";
    const std::string entry_unit = !g.automount_unit.empty()
        ? (g.unit_basename + ".automount")
        : mount_unit;

    // fix44: чистим память systemd о прошлом сбое этого юнита перед
    // start'ом. Без этого второй apply после неудачного mount остаётся
    // в state=failed навсегда — daemon-reload перечитывает unit-файл, но
    // failed-state в памяти не трогает. Best-effort: ошибку только
    // логируем, чтобы не блокировать enable --now (если он реально не
    // поднимется, отдадим конкретный stderr пользователю).
    {
        std::string err;
        if (!systemctl_->resetFailed(mount_unit, err)) {
            spdlog::warn("reset-failed {}: {}", mount_unit, err);
        }
    }
    if (!g.automount_unit.empty()) {
        const std::string am_unit = g.unit_basename + ".automount";
        std::string err;
        if (!systemctl_->resetFailed(am_unit, err)) {
            spdlog::warn("reset-failed {}: {}", am_unit, err);
        }
    }

    {
        std::string err;
        if (!systemctl_->enableNow(entry_unit, err)) {
            return RpcResponse::fail("enable --now " + entry_unit + ": " + err,
                                     "systemd_failed");
        }
    }

    // fix43: eager-mount. `enable --now <basename>.automount` поднимает
    // только path-watcher; сам mount(2) systemd дёрнет лишь при первом
    // доступе к Where=. Но UI/пользователь ожидает, что после "Save"
    // mount сразу active — иначе ls /mnt/... выдаёт «No such device» и
    // оператор не понимает, монтирование прошло или нет. Явно запускаем
    // .mount и, если он не поднялся, возвращаем mount-ошибку клиенту с
    // полным stderr (mount.cifs, NFS server unreachable и т.п.).
    // Если .automount не генерируется — `enable --now <basename>.mount`
    // выше уже запустил .mount, второй start будет no-op.
    if (!g.automount_unit.empty()) {
        std::string err;
        if (!systemctl_->start(mount_unit, err)) {
            return RpcResponse::fail("start " + mount_unit + ": " + err,
                                     "mount_failed");
        }
    }

    // Возвращаем фактический ActiveState — чтобы liveqx записал
    // его в state/mounts.db и UI сразу показал «mounted» / «activating».
    const auto active = systemctl_->activeState(mount_unit);

    nlohmann::json extra{
        {"unit_basename", g.unit_basename},
        {"active_state", active.empty() ? "activating" : active},
        {"automount", !g.automount_unit.empty()},
    };
    return RpcResponse::okWith(active.empty() ? "applied" : active, std::move(extra));
}

RpcResponse MountdHandlers::handleRemove(std::int64_t id, std::string_view target) {
    if (id <= 0) {
        return RpcResponse::fail("remove requires id > 0", "invalid");
    }
    if (target.empty()) {
        return RpcResponse::fail("remove requires target", "invalid");
    }

    const auto unit_dir = cfg_.unit_ctx.systemd_unit_dir;

    // Имена под удаление: новое (target-derived) + legacy (id-based).
    // Legacy гасим без условий — миграция: если файлов нет, removeIfExists
    // молча вернёт true.
    const std::string new_base    = unitBasenameByTarget(target);
    const std::string legacy_base = legacyUnitBasename(id);

    auto disableAndUnlink = [&](const std::string& base) {
        const auto mount_path     = unit_dir / (base + ".mount");
        const auto automount_path = unit_dir / (base + ".automount");
        std::error_code ec;
        if (fs::exists(automount_path, ec)) {
            std::string err;
            if (!systemctl_->disableNow(base + ".automount", err)) {
                spdlog::warn("disable --now {}.automount: {}", base, err);
            }
        }
        if (fs::exists(mount_path, ec)) {
            std::string err;
            if (!systemctl_->disableNow(base + ".mount", err)) {
                spdlog::warn("disable --now {}.mount: {}", base, err);
            }
        }
        std::string err;
        (void) removeIfExists(mount_path, err);
        (void) removeIfExists(automount_path, err);
    };

    disableAndUnlink(new_base);
    if (legacy_base != new_base) disableAndUnlink(legacy_base);

    // Cred-файл — id-based, единственный (см. credFilename).
    {
        std::string err;
        const auto cred_path = cfg_.unit_ctx.cred_dir
                               / (credFilename(id) + ".cred");
        (void) creds_->removeFile(cred_path, err);
    }
    // fix43-migration: на pre-fix43 cred жил в /etc/credstore.encrypted/
    // (зашифрованный). Чистим, чтобы не оставлять blob с паролем на диске.
    {
        std::string err;
        const std::filesystem::path legacy_credstore = "/etc/credstore.encrypted";
        (void) creds_->removeFile(legacy_credstore / (credFilename(id) + ".cred"), err);
    }

    // daemon-reload, чтобы systemd забыл про юнит.
    {
        std::string err;
        if (!systemctl_->daemonReload(err)) {
            return RpcResponse::fail("daemon-reload: " + err, "systemd_failed");
        }
    }
    return RpcResponse::okWith("removed");
}

RpcResponse MountdHandlers::handleTest(const MountSpec& spec) {
    auto r = tester_->test(spec);
    if (!r.ok) {
        return RpcResponse::fail(r.error, "test_failed");
    }
    nlohmann::json extra{
        {"files", r.files},
        {"duration_ms", r.duration_ms},
    };
    return RpcResponse::okWith("ok", std::move(extra));
}

RpcResponse MountdHandlers::handleStatus(const std::vector<StatusItem>& items) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& it : items) {
        if (it.id <= 0 || it.target.empty()) continue;
        const auto base = unitBasenameByTarget(it.target);
        auto u = systemctl_->unitState(base + ".mount");
        arr.push_back({
            {"id",           it.id},
            {"load_state",   u.load_state},
            {"active_state", u.active_state},
            {"sub_state",    u.sub_state},
        });
    }
    return RpcResponse::okWith("ok",
        nlohmann::json{{"units", std::move(arr)}});
}

RpcHandler MountdHandlers::asRpcHandler() {
    return [this](RpcOp op, const nlohmann::json& body) -> RpcResponse {
        try {
            switch (op) {
                case RpcOp::ApplyMount: {
                    auto it = body.find("spec");
                    if (it == body.end() || !it->is_object()) {
                        return RpcResponse::fail("missing spec", "invalid");
                    }
                    std::string serr;
                    auto spec = MountSpec::fromJson(*it, serr);
                    if (!spec) return RpcResponse::fail("bad spec: " + serr, "invalid");
                    return handleApply(*spec);
                }
                case RpcOp::RemoveMount: {
                    auto id_it = body.find("id");
                    if (id_it == body.end() || !id_it->is_number_integer()) {
                        return RpcResponse::fail("missing id", "invalid");
                    }
                    auto t_it = body.find("target");
                    if (t_it == body.end() || !t_it->is_string()) {
                        return RpcResponse::fail("missing target", "invalid");
                    }
                    return handleRemove(id_it->get<std::int64_t>(),
                                        t_it->get<std::string>());
                }
                case RpcOp::TestMount: {
                    auto it = body.find("spec");
                    if (it == body.end() || !it->is_object()) {
                        return RpcResponse::fail("missing spec", "invalid");
                    }
                    std::string serr;
                    auto spec = MountSpec::fromJson(*it, serr);
                    if (!spec) return RpcResponse::fail("bad spec: " + serr, "invalid");
                    return handleTest(*spec);
                }
                case RpcOp::Status: {
                    std::vector<StatusItem> items;
                    auto it = body.find("items");
                    if (it != body.end() && it->is_array()) {
                        items.reserve(it->size());
                        for (const auto& e : *it) {
                            if (!e.is_object()) continue;
                            StatusItem s;
                            s.id     = e.value("id", std::int64_t{0});
                            s.target = e.value("target", std::string{});
                            if (s.id > 0 && !s.target.empty()) {
                                items.push_back(std::move(s));
                            }
                        }
                    }
                    return handleStatus(items);
                }
            }
            return RpcResponse::fail("unknown op", "invalid");
        } catch (const std::exception& e) {
            return RpcResponse::fail(std::string("handler exception: ") + e.what(),
                                     "internal_error");
        }
    };
}

}  // namespace liveqx::mounts
