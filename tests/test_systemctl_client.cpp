// fix41 commit 3 — SystemctlClient: проверяем логику парсинга / argv
// сборки через подмену binary на shell-скрипт. Не требуем настоящего
// systemd, что позволяет гонять на любом CI и в Docker.
//
// Стратегия: пишем во временный каталог shell-скрипт, который ведёт
// себя как systemctl: эхает argv в файл, печатает заранее заданный
// stdout, возвращает заранее заданный exit-code. SystemctlClient
// трактует его как настоящий systemctl. После — читаем файл с argv
// и проверяем, что флаги собраны верно.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include <sys/stat.h>
#include <unistd.h>

#include "mounts/SystemctlClient.h"

using namespace liveqx::mounts;
namespace fs = std::filesystem;

namespace {

fs::path uniqueDir(const std::string& tag) {
    auto p = fs::temp_directory_path() /
        ("sc-systemctl-" + tag + "-" + std::to_string(::getpid())
         + "-" + std::to_string(std::chrono::steady_clock::now()
                                  .time_since_epoch().count()));
    fs::create_directories(p);
    return p;
}

// Создаёт исполняемый скрипт, который:
//   1) сериализует свои argv в файл $LOG (tab-separated).
//   2) печатает $STDOUT_FILE на stdout (если задан).
//   3) выходит с заданным $EXIT_CODE.
struct FakeBin {
    fs::path bin;
    fs::path log;
    fs::path out;        // stdout file (опционально, может быть пустым)

    void create(const fs::path& root,
                const std::string& name,
                const std::string& stdout_data,
                int exit_code) {
        log = root / (name + ".argv");
        out = root / (name + ".stdout");
        bin = root / name;
        std::ofstream(out.string()) << stdout_data;

        std::ofstream s(bin.string());
        s << "#!/bin/sh\n";
        s << "echo \"$@\" > " << log.string() << "\n";
        s << "cat " << out.string() << "\n";
        s << "exit " << exit_code << "\n";
        s.close();
        ::chmod(bin.c_str(), 0755);
    }

    std::string readArgv() const {
        std::ifstream f(log.string());
        std::string s((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
        // Убираем хвостовой newline.
        while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back();
        return s;
    }
};

}  // namespace

TEST(SystemctlClient, NoBinaryReportsClearError) {
    SystemctlClient::Config cfg;
    cfg.bin_path = "";  // не найден
    SystemctlClient c(std::move(cfg));
    std::string err;
    EXPECT_FALSE(c.daemonReload(err));
    EXPECT_NE(err.find("not found"), std::string::npos);
}

TEST(SystemctlClient, DaemonReloadSuccess) {
    auto root = uniqueDir("daemon-reload");
    FakeBin fb;
    fb.create(root, "systemctl", /*stdout=*/"", /*exit=*/0);

    SystemctlClient::Config cfg;
    cfg.bin_path = fb.bin;
    SystemctlClient c(std::move(cfg));

    std::string err;
    EXPECT_TRUE(c.daemonReload(err)) << err;
    auto argv = fb.readArgv();
    EXPECT_NE(argv.find("--system"),    std::string::npos);
    EXPECT_NE(argv.find("daemon-reload"), std::string::npos);
}

TEST(SystemctlClient, DaemonReloadFailurePropagates) {
    auto root = uniqueDir("daemon-reload-fail");
    FakeBin fb;
    fb.create(root, "systemctl", "Failed to connect to system bus", 1);
    SystemctlClient::Config cfg;
    cfg.bin_path = fb.bin;
    SystemctlClient c(std::move(cfg));
    std::string err;
    EXPECT_FALSE(c.daemonReload(err));
    EXPECT_NE(err.find("exit=1"), std::string::npos);
}

TEST(SystemctlClient, EnableNowPassesUnitArg) {
    auto root = uniqueDir("enable-now");
    FakeBin fb;
    fb.create(root, "systemctl", "", 0);
    SystemctlClient::Config cfg;
    cfg.bin_path = fb.bin;
    SystemctlClient c(std::move(cfg));
    std::string err;
    EXPECT_TRUE(c.enableNow("liveqx-mnt-7.automount", err));
    auto argv = fb.readArgv();
    EXPECT_NE(argv.find("enable"),  std::string::npos);
    EXPECT_NE(argv.find("--now"),   std::string::npos);
    EXPECT_NE(argv.find("liveqx-mnt-7.automount"), std::string::npos);
}

TEST(SystemctlClient, StartPassesUnitArgWithoutEnable) {
    auto root = uniqueDir("start");
    FakeBin fb;
    fb.create(root, "systemctl", "", 0);
    SystemctlClient::Config cfg;
    cfg.bin_path = fb.bin;
    SystemctlClient c(std::move(cfg));
    std::string err;
    EXPECT_TRUE(c.start("mnt-liveqx-lib1.mount", err)) << err;
    auto argv = fb.readArgv();
    EXPECT_NE(argv.find("start"), std::string::npos);
    // 'enable' и '--now' не должны попасть в argv eager-start'а.
    EXPECT_EQ(argv.find("enable"), std::string::npos);
    EXPECT_EQ(argv.find("--now"),  std::string::npos);
    EXPECT_NE(argv.find("mnt-liveqx-lib1.mount"), std::string::npos);
}

TEST(SystemctlClient, StartFailurePropagatesStderr) {
    auto root = uniqueDir("start-fail");
    auto bin = root / "systemctl";
    {
        std::ofstream s(bin.string());
        // mount.cifs error попадает в stderr.
        s << "#!/bin/sh\n"
          << "echo \"Failed to mount mnt-foo.mount: mount.cifs error(13)\" >&2\n"
          << "exit 32\n";
    }
    ::chmod(bin.c_str(), 0755);

    SystemctlClient::Config cfg;
    cfg.bin_path = bin;
    SystemctlClient c(std::move(cfg));
    std::string err;
    EXPECT_FALSE(c.start("mnt-foo.mount", err));
    EXPECT_NE(err.find("exit=32"),       std::string::npos);
    EXPECT_NE(err.find("mount.cifs"),    std::string::npos);
}

TEST(SystemctlClient, ResetFailedPassesUnitArg) {
    auto root = uniqueDir("reset");
    FakeBin fb;
    fb.create(root, "systemctl", "", 0);
    SystemctlClient::Config cfg;
    cfg.bin_path = fb.bin;
    SystemctlClient c(std::move(cfg));
    std::string err;
    EXPECT_TRUE(c.resetFailed("mnt-liveqx-foo.mount", err)) << err;
    auto argv = fb.readArgv();
    EXPECT_NE(argv.find("reset-failed"), std::string::npos);
    EXPECT_NE(argv.find("mnt-liveqx-foo.mount"), std::string::npos);
}

TEST(SystemctlClient, ResetFailedOnMissingUnitTreatedAsSuccess) {
    auto root = uniqueDir("reset-missing");
    auto bin = root / "systemctl";
    {
        std::ofstream s(bin.string());
        // systemctl reset-failed на нечто, чего нет, ругается на stderr и
        // отдаёт exit≠0 — это валидный «нечего сбрасывать».
        s << "#!/bin/sh\n"
          << "echo \"Failed to reset failed state of mnt-foo.mount: Unit mnt-foo.mount not loaded.\" >&2\n"
          << "exit 1\n";
    }
    ::chmod(bin.c_str(), 0755);

    SystemctlClient::Config cfg;
    cfg.bin_path = bin;
    SystemctlClient c(std::move(cfg));
    std::string err;
    EXPECT_TRUE(c.resetFailed("mnt-foo.mount", err))
        << "reset-failed on missing unit must not be a hard error; err=" << err;
}

TEST(SystemctlClient, DisableNowOnMissingUnitTreatedAsSuccess) {
    auto root = uniqueDir("disable-missing");
    // Реальный systemctl пишет «does not exist» в stderr и exit=1.
    auto bin = root / "systemctl";
    {
        std::ofstream s(bin.string());
        s << "#!/bin/sh\n"
          << "echo \"Unit liveqx-mnt-99.mount does not exist.\" >&2\n"
          << "exit 1\n";
    }
    ::chmod(bin.c_str(), 0755);

    SystemctlClient::Config cfg;
    cfg.bin_path = bin;
    SystemctlClient c(std::move(cfg));
    std::string err;
    EXPECT_TRUE(c.disableNow("liveqx-mnt-99.mount", err))
        << "disable on missing unit must not be a hard error; err=" << err;
}

TEST(SystemctlClient, ActiveStateParsesShowOutput) {
    auto root = uniqueDir("active");
    FakeBin fb;
    fb.create(root, "systemctl", "ActiveState=active\n", 0);
    SystemctlClient::Config cfg;
    cfg.bin_path = fb.bin;
    SystemctlClient c(std::move(cfg));
    EXPECT_EQ(c.activeState("liveqx-mnt-7.mount"), "active");
}

TEST(SystemctlClient, ActiveStateEmptyForUnknownUnit) {
    auto root = uniqueDir("active-empty");
    FakeBin fb;
    // systemctl show на несуществующий юнит обычно даёт ActiveState=inactive
    // с exit-кодом 0; тут моделируем «нет такого свойства».
    fb.create(root, "systemctl", "", 0);
    SystemctlClient::Config cfg;
    cfg.bin_path = fb.bin;
    SystemctlClient c(std::move(cfg));
    EXPECT_EQ(c.activeState("nope.mount"), "");
}

TEST(SystemctlClient, UnitStateEmptyForUnknownUnit) {
    auto root = uniqueDir("us-empty");
    FakeBin fb;
    fb.create(root, "systemctl", "", 0);
    SystemctlClient::Config cfg;
    cfg.bin_path = fb.bin;
    SystemctlClient c(std::move(cfg));
    auto s = c.unitState("nope.mount");
    EXPECT_TRUE(s.load_state.empty());
    EXPECT_TRUE(s.active_state.empty());
    EXPECT_TRUE(s.sub_state.empty());
}

TEST(SystemctlClient, UnitStateParsesShowOutput) {
    auto root = uniqueDir("us-parse");
    FakeBin fb;
    fb.create(root, "systemctl",
              "LoadState=loaded\nActiveState=active\nSubState=mounted\n", 0);
    SystemctlClient::Config cfg;
    cfg.bin_path = fb.bin;
    SystemctlClient c(std::move(cfg));
    auto s = c.unitState("foo.mount");
    EXPECT_EQ(s.load_state,   "loaded");
    EXPECT_EQ(s.active_state, "active");
    EXPECT_EQ(s.sub_state,    "mounted");
}

TEST(SystemctlClient, UserScopeAddsUserFlag) {
    auto root = uniqueDir("user-scope");
    FakeBin fb;
    fb.create(root, "systemctl", "", 0);
    SystemctlClient::Config cfg;
    cfg.bin_path     = fb.bin;
    cfg.system_scope = false;
    SystemctlClient c(std::move(cfg));
    std::string err;
    (void) c.daemonReload(err);
    auto argv = fb.readArgv();
    EXPECT_NE(argv.find("--user"),   std::string::npos);
    EXPECT_EQ(argv.find("--system"), std::string::npos);
}
