#pragma once
//
// fix41 — fact-check для шары до того, как мы заведём в системе .mount-юнит.
//
// Вызывается, когда оператор жмёт «Test» в UI: dry-run примонтировать
// шару во временный каталог, посчитать файлы первого уровня, сразу
// размонтировать. Это даёт мгновенный ответ «работает / не работает /
// нет прав / нет сети» без необходимости создавать персистентный юнит.
//
// IMountTester — абстракция, чтобы MountdHandlers тестировались без
// настоящего mount(8). MountTester — production-импл через
// /bin/mount + /bin/umount.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>

#include "mounts/MountSpec.h"

namespace liveqx::mounts {

struct TestMountResult {
    bool         ok = false;
    std::string  error;          // непустой при !ok
    std::int64_t files = 0;      // count первого уровня (для UI sanity)
    int          duration_ms = 0;
};

class IMountTester {
public:
    virtual ~IMountTester() = default;
    virtual TestMountResult test(const MountSpec& spec) = 0;
};

class MountTester final : public IMountTester {
public:
    struct Config {
        std::filesystem::path mount_bin;     // /bin/mount
        std::filesystem::path umount_bin;    // /bin/umount

        // Базовый каталог для эфемерных точек. Default — /run/liveqx/test
        // (tmpfs, root:liveqx 0750).
        std::filesystem::path scratch_root = "/run/liveqx/test";

        // Тайм-аут на mount(8) и сам file-listing. 30s достаточно для
        // CIFS handshake + DNS + auth, без зависимости на оператора.
        std::chrono::milliseconds op_timeout{30000};

        // Сколько файлов максимум считать на первом уровне (cap).
        std::size_t max_files_listed = 1024;
    };

    static Config defaultConfig();
    explicit MountTester(Config cfg);

    TestMountResult test(const MountSpec& spec) override;

private:
    Config cfg_;
};

}  // namespace liveqx::mounts
