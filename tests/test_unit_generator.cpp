// fix41 commit 2 — UnitGenerator: pure-функции, проверяемые без systemd.
// fix42 — basename юнита выводится из target через systemd-escape(5).

#include <gtest/gtest.h>

#include "mounts/MountSpec.h"
#include "mounts/UnitGenerator.h"

using namespace liveqx::mounts;

namespace {

// Что должно получаться для "/mnt/liveqx/lib1" — это эталон, против
// которого мы проверяем все ассерты. Можно убедиться вручную:
//   $ systemd-escape --path /mnt/liveqx/lib1
//   mnt-liveqx-lib1
constexpr const char* kCifsTarget        = "/mnt/liveqx/lib1";
constexpr const char* kCifsBasename      = "mnt-liveqx-lib1";
constexpr const char* kNfsTarget         = "/mnt/liveqx/nfs1";
constexpr const char* kNfsBasename       = "mnt-liveqx-nfs1";

MountSpec cifsSpec() {
    MountSpec s;
    s.id      = 7;
    s.fs_type = FsType::Cifs;
    s.source  = "//srv/share";
    s.target  = kCifsTarget;
    s.options = "vers=3.0,iocharset=utf8";
    s.ro      = true;
    s.cifs    = CifsCreds{"viewer", "secret", "WORKGROUP"};
    return s;
}

MountSpec nfsSpec() {
    MountSpec s;
    s.id      = 9;
    s.fs_type = FsType::Nfs;
    s.source  = "nas:/export/video";
    s.target  = kNfsTarget;
    s.options = "vers=4,proto=tcp";
    s.ro      = true;
    return s;
}

}  // namespace

TEST(UnitGenerator, CifsHasCredentialsLine) {
    auto g = generateUnits(cifsSpec(), {});
    EXPECT_EQ(g.unit_basename, kCifsBasename);
    EXPECT_NE(g.mount_unit.find("Type=cifs"), std::string::npos);
    // fix43: LoadCredentialEncrypted= больше не используется (см. CredsHelper.h).
    EXPECT_EQ(g.mount_unit.find("LoadCredentialEncrypted"), std::string::npos);
    // credentials= содержит абсолютный путь к plaintext cred-файлу.
    EXPECT_NE(g.mount_unit.find(
        "credentials=/run/liveqx/creds/liveqx-mnt-7.cred"),
              std::string::npos);
    EXPECT_TRUE(g.needs_credentials());
    // Cred-файл — id-based на tmpfs.
    EXPECT_EQ(g.cred_path,
              "/run/liveqx/creds/liveqx-mnt-7.cred");
}

TEST(UnitGenerator, CifsCredPayloadFormat) {
    auto g = generateUnits(cifsSpec(), {});
    EXPECT_EQ(g.cred_payload,
              "username=viewer\npassword=secret\ndomain=WORKGROUP\n");
}

TEST(UnitGenerator, CifsGuestNoCredentials) {
    MountSpec s = cifsSpec();
    s.cifs.reset();  // guest, no creds
    auto g = generateUnits(s, {});
    EXPECT_FALSE(g.needs_credentials());
    EXPECT_EQ(g.mount_unit.find("LoadCredentialEncrypted"), std::string::npos);
    EXPECT_EQ(g.mount_unit.find("credentials="), std::string::npos);
}

TEST(UnitGenerator, NfsNoCredentialsLine) {
    auto g = generateUnits(nfsSpec(), {});
    EXPECT_NE(g.mount_unit.find("Type=nfs"), std::string::npos);
    EXPECT_FALSE(g.needs_credentials());
    EXPECT_EQ(g.mount_unit.find("credentials"), std::string::npos);
}

TEST(UnitGenerator, RoFlagAddedWhenAbsent) {
    MountSpec s = cifsSpec();
    s.options = "vers=3.0";
    s.ro      = true;
    EXPECT_NE(buildOptionsLine(s, false, {}).find(",ro"), std::string::npos);
}

TEST(UnitGenerator, RoFlagNotDuplicatedWhenPresent) {
    MountSpec s = cifsSpec();
    s.options = "vers=3.0,ro";
    s.ro      = true;
    auto opts = buildOptionsLine(s, false, {});
    // Должно быть ровно одно «ro» — два бы ломало mount(8).
    std::size_t count = 0;
    for (std::size_t pos = 0;;) {
        const auto p = opts.find(",ro", pos);
        if (p == std::string::npos) break;
        ++count;
        pos = p + 1;
    }
    EXPECT_LE(count, 1u);
}

TEST(UnitGenerator, CifsCredentialsLineUsesAbsolutePath) {
    MountSpec s = cifsSpec();
    auto opts = buildOptionsLine(s, true, "/run/liveqx/creds/x.cred");
    EXPECT_NE(opts.find("credentials=/run/liveqx/creds/x.cred"),
              std::string::npos);
}

TEST(UnitGenerator, AutomountUnitGenerated) {
    auto g = generateUnits(cifsSpec(), {});
    EXPECT_FALSE(g.automount_unit.empty());
    EXPECT_NE(g.automount_unit.find("[Automount]"), std::string::npos);
    EXPECT_NE(g.automount_unit.find("TimeoutIdleSec=600"), std::string::npos);
    EXPECT_NE(g.automount_unit.find("Where=/mnt/liveqx/lib1"),
              std::string::npos);
}

TEST(UnitGenerator, AutomountSkippedWhenDisabled) {
    UnitGenContext ctx;
    ctx.generate_automount = false;
    auto g = generateUnits(cifsSpec(), ctx);
    EXPECT_TRUE(g.automount_unit.empty());
}

// fix44: liveqx под пользователем liveqx (uid≠0). mount.cifs
// без uid=/gid=/file_mode=/dir_mode= мапит все файлы на root, и Watcher
// видит пустой каталог. Генератор обязан подставить эти токены, если в
// ctx указан non-zero cifs_uid.
TEST(UnitGenerator, CifsInjectsOwnerWhenCtxUidSet) {
    UnitGenContext ctx;
    ctx.cifs_uid = 998;
    ctx.cifs_gid = 999;
    auto g = generateUnits(cifsSpec(), ctx);
    EXPECT_NE(g.mount_unit.find("uid=998"),       std::string::npos);
    EXPECT_NE(g.mount_unit.find("gid=999"),       std::string::npos);
    EXPECT_NE(g.mount_unit.find("file_mode=0644"),std::string::npos);
    EXPECT_NE(g.mount_unit.find("dir_mode=0755"), std::string::npos);
}

TEST(UnitGenerator, CifsOwnerInjectionRespectsOperatorOverride) {
    // Если оператор уже задал uid=/gid=/file_mode=/dir_mode= в options —
    // не дублируем, оставляем его значения.
    MountSpec s = cifsSpec();
    s.options = "vers=3.0,uid=1000,gid=1000,file_mode=0600,dir_mode=0700";
    UnitGenContext ctx;
    ctx.cifs_uid = 998;
    ctx.cifs_gid = 999;
    auto opts = buildOptionsLine(s, false, {}, ctx);
    // Должны остаться значения оператора, без удвоения.
    EXPECT_NE(opts.find("uid=1000"),       std::string::npos);
    EXPECT_EQ(opts.find("uid=998"),        std::string::npos);
    EXPECT_NE(opts.find("gid=1000"),       std::string::npos);
    EXPECT_EQ(opts.find("gid=999"),        std::string::npos);
    EXPECT_NE(opts.find("file_mode=0600"), std::string::npos);
    EXPECT_NE(opts.find("dir_mode=0700"),  std::string::npos);
}

TEST(UnitGenerator, CifsOwnerInjectionSkippedWhenCtxUidZero) {
    // pre-fix44 поведение: при cifs_uid=0 (например, если на хосте нет
    // пользователя liveqx) ничего не инжектим — оставляем mount.cifs
    // дефолтам.
    UnitGenContext ctx;
    ctx.cifs_uid = 0;
    auto g = generateUnits(cifsSpec(), ctx);
    EXPECT_EQ(g.mount_unit.find("uid="),       std::string::npos);
    EXPECT_EQ(g.mount_unit.find("gid="),       std::string::npos);
    EXPECT_EQ(g.mount_unit.find("file_mode="), std::string::npos);
    EXPECT_EQ(g.mount_unit.find("dir_mode="),  std::string::npos);
}

TEST(UnitGenerator, NfsOwnerInjectionSkipped) {
    // Для NFS uid mapping серверный — токены не инжектим, даже если в ctx
    // выставлен cifs_uid.
    UnitGenContext ctx;
    ctx.cifs_uid = 998;
    ctx.cifs_gid = 999;
    auto g = generateUnits(nfsSpec(), ctx);
    EXPECT_EQ(g.mount_unit.find("uid=998"), std::string::npos);
    EXPECT_EQ(g.mount_unit.find("gid=999"), std::string::npos);
    EXPECT_EQ(g.mount_unit.find("file_mode="), std::string::npos);
    EXPECT_EQ(g.mount_unit.find("dir_mode="),  std::string::npos);
}

TEST(UnitGenerator, CifsCustomFileAndDirMode) {
    UnitGenContext ctx;
    ctx.cifs_uid       = 998;
    ctx.cifs_gid       = 999;
    ctx.cifs_file_mode = "0640";
    ctx.cifs_dir_mode  = "0750";
    auto opts = buildOptionsLine(cifsSpec(), false, {}, ctx);
    EXPECT_NE(opts.find("file_mode=0640"), std::string::npos);
    EXPECT_NE(opts.find("dir_mode=0750"),  std::string::npos);
}

TEST(UnitGenerator, CredPathHonoursContextDir) {
    UnitGenContext ctx;
    ctx.cred_dir = "/tmp/creds";
    auto g = generateUnits(cifsSpec(), ctx);
    EXPECT_EQ(g.cred_path, "/tmp/creds/liveqx-mnt-7.cred");
    // fix43: путь попадает в Options=credentials=, а не в
    // LoadCredentialEncrypted=.
    EXPECT_NE(g.mount_unit.find(
        "credentials=/tmp/creds/liveqx-mnt-7.cred"),
              std::string::npos);
    EXPECT_EQ(g.mount_unit.find("LoadCredentialEncrypted"), std::string::npos);
}

TEST(UnitGenerator, MountUnitHasNetworkOnline) {
    auto g = generateUnits(cifsSpec(), {});
    EXPECT_NE(g.mount_unit.find("After=network-online.target"),
              std::string::npos);
    EXPECT_NE(g.mount_unit.find("Wants=network-online.target"),
              std::string::npos);
}

TEST(UnitGenerator, LegacyBasenameStable) {
    // Сохраняется только для миграции pre-fix42 файлов.
    EXPECT_EQ(legacyUnitBasename(0),    "liveqx-mnt-0");
    EXPECT_EQ(legacyUnitBasename(123),  "liveqx-mnt-123");
}

TEST(UnitGenerator, CredFilenameIdBased) {
    EXPECT_EQ(credFilename(42), "liveqx-mnt-42");
}

// ───── systemd-escape(5) edge cases ─────────────────────────────────────
// Все эталоны можно проверить вручную:
//   $ systemd-escape --path <input>

TEST(SystemdEscapePath, SimplePath) {
    EXPECT_EQ(systemdEscapePath("/mnt/x"),     "mnt-x");
    EXPECT_EQ(systemdEscapePath("/mnt/foo"),   "mnt-foo");
    EXPECT_EQ(systemdEscapePath("/mnt/foo/bar"), "mnt-foo-bar");
}

TEST(SystemdEscapePath, TrailingSlashStripped) {
    EXPECT_EQ(systemdEscapePath("/mnt/foo/"),  "mnt-foo");
    EXPECT_EQ(systemdEscapePath("/mnt/foo"),   "mnt-foo");
}

TEST(SystemdEscapePath, RootAndEmptyAreDash) {
    EXPECT_EQ(systemdEscapePath(""),  "-");
    EXPECT_EQ(systemdEscapePath("/"), "-");
}

TEST(SystemdEscapePath, HyphenIsEscaped) {
    // Hyphen в сегменте → \x2d, иначе ' / ' и '-' были бы неотличимы.
    EXPECT_EQ(systemdEscapePath("/mnt/liveqx/lib1"),
              "mnt-liveqx-lib1");
    EXPECT_EQ(systemdEscapePath("/a-b"), "a\\x2db");
}

TEST(SystemdEscapePath, LeadingDotEscaped) {
    // systemd-escape эскейпит '.' только если это ПЕРВЫЙ символ всего
    // итогового escape'нутого имени (а не первый символ каждого сегмента).
    EXPECT_EQ(systemdEscapePath("/.hidden"),    "\\x2ehidden");
    // '.' после '-'-разделителя сегментов уже не первый символ → не эскейпим.
    EXPECT_EQ(systemdEscapePath("/normal/.x"),  "normal-.x");
}

TEST(SystemdEscapePath, NonAsciiAndSpecialChars) {
    // Пробелы и спецсимволы — \xHH.
    EXPECT_EQ(systemdEscapePath("/with space"), "with\\x20space");
    EXPECT_EQ(systemdEscapePath("/a:b"),        "a\\x3ab");
}

TEST(SystemdEscapePath, AlnumDotUnderscorePassthrough) {
    EXPECT_EQ(systemdEscapePath("/a_b.c"), "a_b.c");
    EXPECT_EQ(systemdEscapePath("/AbC0_9"), "AbC0_9");
}

TEST(UnitBasenameByTarget, MatchesSystemdEscape) {
    // Контракт: то, что мы пишем в Where=, и то, что мы используем как имя
    // .mount-файла, должны совпадать после systemd-escape — иначе systemd
    // отвергает юнит при load'е ("Where= doesn't match unit name").
    EXPECT_EQ(unitBasenameByTarget("/mnt/liveqx/lib1"),
              "mnt-liveqx-lib1");
}

TEST(NormalizedTargetPath, StripTrailingAndPrependSlash) {
    EXPECT_EQ(normalizedTargetPath("/mnt/x/"),   "/mnt/x");
    EXPECT_EQ(normalizedTargetPath("/mnt/x"),    "/mnt/x");
    EXPECT_EQ(normalizedTargetPath("mnt/x"),     "/mnt/x");
    EXPECT_EQ(normalizedTargetPath(""),          "/");
}
