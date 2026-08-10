#include "mounts/MountTester.h"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <fstream>
#include <random>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <unistd.h>

#include "mounts/Subprocess.h"
#include "mounts/UnitGenerator.h"

namespace fs = std::filesystem;

namespace liveqx::mounts {

namespace {

bool fileExecutable(const fs::path& p) {
    std::error_code ec;
    if (!fs::exists(p, ec)) return false;
    return ::access(p.c_str(), X_OK) == 0;
}

// 16 hex знаков из crypto-grade RNG. id = 0 для test-pad'а, поэтому
// нужен отдельный suffix чтобы ParaTest'ы не сталкивались.
std::string randomSuffix() {
    std::random_device rd;
    std::uniform_int_distribution<std::uint64_t> dist;
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016lx",
                  static_cast<unsigned long>(dist(rd)));
    return std::string(buf, 16);
}

bool writeAndChmod(const fs::path& path,
                   const std::string& content,
                   std::string& err) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) {
        err = "mkdir " + path.parent_path().string() + ": " + ec.message();
        return false;
    }
    // O_CREAT|O_EXCL чтобы не перезаписать чужой файл (например symlink).
    int fd = ::open(path.c_str(),
                    O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                    0600);
    if (fd < 0) {
        err = "open " + path.string() + ": " + std::strerror(errno);
        return false;
    }
    const char* p = content.data();
    std::size_t left = content.size();
    while (left > 0) {
        ssize_t n = ::write(fd, p, left);
        if (n < 0) {
            if (errno == EINTR) continue;
            err = "write " + path.string() + ": " + std::strerror(errno);
            ::close(fd);
            return false;
        }
        p    += n;
        left -= static_cast<std::size_t>(n);
    }
    ::close(fd);
    return true;
}

}  // namespace

MountTester::Config MountTester::defaultConfig() {
    Config c;
    if      (fileExecutable("/bin/mount"))      c.mount_bin = "/bin/mount";
    else if (fileExecutable("/usr/bin/mount"))  c.mount_bin = "/usr/bin/mount";
    if      (fileExecutable("/bin/umount"))     c.umount_bin = "/bin/umount";
    else if (fileExecutable("/usr/bin/umount")) c.umount_bin = "/usr/bin/umount";
    return c;
}

MountTester::MountTester(Config cfg) : cfg_(std::move(cfg)) {}

TestMountResult MountTester::test(const MountSpec& spec) {
    TestMountResult res;
    const auto t0 = std::chrono::steady_clock::now();

    if (cfg_.mount_bin.empty() || cfg_.umount_bin.empty()) {
        res.error = "mount/umount binary not found";
        return res;
    }

    std::string verr;
    if (!spec.validate(verr)) {
        res.error = "invalid spec: " + verr;
        return res;
    }

    // Готовим эфемерную точку монтирования: /run/liveqx/test/<rand>
    // вместо самого spec.target — чтобы тест не задевал production-mount'ы.
    std::error_code ec;
    fs::create_directories(cfg_.scratch_root, ec);
    if (ec) {
        res.error = "mkdir " + cfg_.scratch_root.string() + ": " + ec.message();
        return res;
    }
    const auto suffix     = randomSuffix();
    const auto mount_dir  = cfg_.scratch_root / ("probe-" + suffix);
    const auto creds_file = cfg_.scratch_root / ("probe-" + suffix + ".cred");
    fs::create_directories(mount_dir, ec);
    if (ec) {
        res.error = "mkdir " + mount_dir.string() + ": " + ec.message();
        return res;
    }
    fs::permissions(mount_dir,
                    fs::perms::owner_all,
                    fs::perm_options::replace, ec);

    // Гарантируем уборку точки и креда даже на ранних returns.
    struct Cleanup {
        const fs::path& dir;
        const fs::path& creds;
        std::filesystem::path umount_bin;
        std::chrono::milliseconds op_timeout;
        bool mounted = false;
        ~Cleanup() {
            if (mounted) {
                Subprocess u;
                u.argv = {umount_bin.string(), dir.string()};
                u.timeout_ms = op_timeout;
                (void) u.run();
                // Если mount всё ещё держится — лень не наша; cleanup
                // background'ом доделает GC по таймауту.
            }
            std::error_code lec;
            std::filesystem::remove(creds, lec);
            std::filesystem::remove(dir,   lec);
        }
    };
    Cleanup cleanup{mount_dir, creds_file, cfg_.umount_bin, cfg_.op_timeout};

    // Собираем mount-команду:
    //   /bin/mount -t cifs -o vers=3.0,credentials=<file>,ro //srv/share <mount_dir>
    // Для CIFS с креды — пишем temp creds-файл (0600). Для NFS / guest
    // CIFS — без credentials-флага.
    const bool needs_creds = (spec.fs_type == FsType::Cifs
                              && spec.cifs.has_value()
                              && !spec.cifs->password.empty());

    // with_credentials=false: MountTester подмешивает credentials= вручную
    // в temp-файл ниже, чтобы держать ephemeral cred отдельно от продового
    // /run/liveqx/creds/. Поэтому cred_path не нужен.
    std::string options = buildOptionsLine(spec, /*with_credentials=*/false, {});
    if (needs_creds) {
        std::string payload =
            "username=" + spec.cifs->username + "\n" +
            "password=" + spec.cifs->password + "\n";
        if (!spec.cifs->domain.empty()) {
            payload += "domain=" + spec.cifs->domain + "\n";
        }
        std::string werr;
        if (!writeAndChmod(creds_file, payload, werr)) {
            res.error = werr;
            return res;
        }
        if (!options.empty()) options.push_back(',');
        options += "credentials=" + creds_file.string();
    }

    Subprocess m;
    m.argv = {
        cfg_.mount_bin.string(),
        "-t", toString(spec.fs_type),
        "-o", options,
        spec.source,
        mount_dir.string(),
    };
    m.timeout_ms = cfg_.op_timeout;
    auto mr = m.run();

    if (!mr.error.empty()) {
        res.error = "mount: " + mr.error;
        return res;
    }
    if (mr.timed_out) {
        res.error = "mount timed out";
        return res;
    }
    if (mr.exit_code != 0) {
        // mount.cifs/mount.nfs пишут осмысленный stderr — пробрасываем.
        const auto& s = !mr.stderr_data.empty() ? mr.stderr_data : mr.stdout_data;
        res.error = "mount failed (exit=" + std::to_string(mr.exit_code) + ")"
                    + (s.empty() ? "" : (": " + s));
        return res;
    }
    cleanup.mounted = true;

    // Считаем файлы первого уровня. Любые I/O-ошибки на этом этапе
    // считаем «mount удался, но шара пустая/недоступна» — и это всё
    // равно полезный сигнал для UI.
    std::int64_t count = 0;
    std::error_code lec;
    auto it = fs::directory_iterator(mount_dir, lec);
    if (!lec) {
        for (const auto& e : it) {
            (void)e;
            if (++count >= static_cast<std::int64_t>(cfg_.max_files_listed)) break;
        }
    }
    res.files = count;
    res.ok    = true;
    res.duration_ms = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count());
    return res;
}

}  // namespace liveqx::mounts
