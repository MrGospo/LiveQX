cmake_minimum_required(VERSION 3.20)
project(liveqx VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ENABLE_ASAN "Build with AddressSanitizer" OFF)
if(ENABLE_ASAN)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address)
endif()

option(ENABLE_TSAN "Build with ThreadSanitizer" OFF)
if(ENABLE_TSAN)
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=thread)
endif()

# fix21 — sd_notify integration. Native AF_UNIX SOCK_DGRAM impl, no
# libsystemd link dep. Default ON on Linux (production deploys under
# systemd); OFF for container images that don't run a system manager.
option(ENABLE_SYSTEMD "Enable sd_notify integration (READY/STOPPING/WATCHDOG)" ON)
if(ENABLE_SYSTEMD)
    add_compile_definitions(LIVEQX_ENABLE_SYSTEMD=1)
endif()

# fix29 — GPU encoder backends. Each is independent; default OFF so the
# stock CPU-only build is unchanged. Enabling one requires:
#   * the bundled FFmpeg rebuilt with the matching --enable-* (see
#     scripts/build_ffmpeg.sh ENABLE_NVENC/QSV/VAAPI env vars),
#   * the runtime GPU + driver stack present on the host.
# When the option is ON we set both the umbrella LIVEQX_FEATURE_GPU
# (controls the "gpu" flag in /api/version) and a backend-specific
# LIVEQX_HAVE_NVENC/QSV/VAAPI used by EncoderFactory dispatch.
option(ENABLE_NVENC "Build with NVIDIA NVENC encoder support"   OFF)
option(ENABLE_QSV   "Build with Intel Quick Sync (QSV) support" OFF)
option(ENABLE_VAAPI "Build with VAAPI encoder support"          OFF)

if(ENABLE_NVENC OR ENABLE_QSV OR ENABLE_VAAPI)
    add_compile_definitions(LIVEQX_FEATURE_GPU=1)
endif()
if(ENABLE_NVENC)
    add_compile_definitions(LIVEQX_HAVE_NVENC=1
                            LIVEQX_FEATURE_NVENC=1)
endif()
if(ENABLE_QSV)
    add_compile_definitions(LIVEQX_HAVE_QSV=1
                            LIVEQX_FEATURE_QSV=1)
endif()
if(ENABLE_VAAPI)
    add_compile_definitions(LIVEQX_HAVE_VAAPI=1
                            LIVEQX_FEATURE_VAAPI=1)
endif()

include(FetchContent)

# FFmpeg — custom static build (no libsrt) or system fallback
find_package(PkgConfig REQUIRED)
set(FFMPEG_ROOT "${CMAKE_SOURCE_DIR}/third_party/ffmpeg"
    CACHE PATH "Path to custom FFmpeg install (third_party/ffmpeg)")
if(EXISTS "${FFMPEG_ROOT}/lib/pkgconfig/libavformat.pc")
    message(STATUS "FFmpeg: using custom static build at ${FFMPEG_ROOT}")
    set(ENV{PKG_CONFIG_PATH} "${FFMPEG_ROOT}/lib/pkgconfig")
    pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
        libavformat libavcodec libavutil libswscale libswresample libavfilter)
else()
    message(STATUS "FFmpeg: custom build not found, falling back to system")
    pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
        libavformat libavcodec libavutil libswscale libswresample libavfilter)
endif()
# cpp-httplib — header-only HTTP server. Was previously pulled from apt
# (libcpp-httplib0.14t64), but the SO name changes between Ubuntu releases
# (22.04 has no such package at all). Fetch sources via FetchContent —
# header-only target, nothing to link.
FetchContent_Declare(cpp-httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG        v0.15.3
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE
)
set(HTTPLIB_COMPILE OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(cpp-httplib)

# spdlog + fmt — static, so the binary is independent of the distro's
# libspdlog/libfmt SO version. Ubuntu 22.04: libspdlog1.10/libfmt8,
# 24.04: libspdlog1.12/libfmt9 — incompatible for cross-target deploys.
# SPDLOG_FMT_EXTERNAL=OFF (default): bundled fmt from a submodule, built
# into libspdlog.a, nothing exposed externally.
#
# IMPORTANT: PINNED to v1.13.0. On v1.14.x glibc regresses with a
# pthread_mutex assertion when logging from FFmpeg / x264 threads
# (see PreviewIntegration tests — they consistently fail with
# "Fatal glibc error: pthread_mutex_lock.c:94"). Looks like 1.14
# changed the default sink mutex behaviour on reentry from callbacks.
# Before upgrading, run ctest -R PreviewIntegration.
set(SPDLOG_BUILD_SHARED  OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS   OFF CACHE BOOL "" FORCE)
set(SPDLOG_INSTALL       OFF CACHE BOOL "" FORCE)
set(SPDLOG_FMT_EXTERNAL  OFF CACHE BOOL "" FORCE)
FetchContent_Declare(spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG        v1.13.0
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE
)
FetchContent_MakeAvailable(spdlog)

# nlohmann_json, GTest — system packages (apt). Stable ABI across
# releases; header-only / standard test framework.
find_package(nlohmann_json REQUIRED)
find_package(GTest REQUIRED)

# SQLite3 — system package
find_package(SQLite3 REQUIRED)

# libsodium — fix22: argon2id + crypto_secretbox + randombytes.
# Shipped in apt as libsodium-dev, exports a .pc file.
pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium)

# OpenLDAP client — fix22 c16/24: search+bind for LDAP authentication.
# Apt package libldap2-dev. We link -lldap -llber directly via an
# imported target — openldap has no stable pkg-config file on ubuntu 24.04.
find_library(LDAP_LIB    NAMES ldap   REQUIRED)
find_library(LBER_LIB    NAMES lber   REQUIRED)
find_path   (LDAP_INC    NAMES ldap.h REQUIRED)
add_library(openldap_client INTERFACE)
target_include_directories(openldap_client INTERFACE ${LDAP_INC})
target_link_libraries     (openldap_client INTERFACE ${LDAP_LIB} ${LBER_LIB})

# OpenSSL — needed by both libsrt (USE_OPENSSL=ON) and jwt-cpp (HS256
# via HMAC-SHA256). A single find_package covers both.
find_package(OpenSSL REQUIRED)

# libcurl — fix22 c23/24: SMTP transport for notifications. Apt package
# libcurl4-openssl-dev. CURL::libcurl — the standard imported target.
find_package(CURL REQUIRED)

# jwt-cpp — header-only HS256 wrapper (fix22 commit 4). Pulled in via
# FetchContent, install and tests disabled.
set(JWT_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(JWT_DISABLE_BASE64 OFF CACHE BOOL "" FORCE)
FetchContent_Declare(jwt-cpp
    GIT_REPOSITORY https://github.com/Thalhammer/jwt-cpp.git
    GIT_TAG        v0.7.0
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE
)
FetchContent_MakeAvailable(jwt-cpp)

# Howard Hinnant date/tz — portable C++20 tzdb. libstdc++ 12 (Debian 12,
# Ubuntu 22.04) lacks std::chrono::time_zone/locate_zone/to_local/to_sys,
# and Scheduler.cpp / TimeConfig.cpp rely on IANA tzdata. Vendor it so a
# single build works on GCC 12/13/14 without backports/PPA gymnastics.
# USE_SYSTEM_TZ_DB=ON — read /usr/share/zoneinfo (the tzdata package is
# installed by default on any Debian/Ubuntu). HAS_REMOTE_API=0 disables
# the curl-based fallback.
set(BUILD_TZ_LIB       ON  CACHE BOOL   "" FORCE)
set(USE_SYSTEM_TZ_DB   ON  CACHE BOOL   "" FORCE)
set(HAS_REMOTE_API     0   CACHE STRING "" FORCE)
FetchContent_Declare(hinnant_date
    GIT_REPOSITORY https://github.com/HowardHinnant/date.git
    GIT_TAG        v3.0.4
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE
)
FetchContent_MakeAvailable(hinnant_date)

# libdatachannel — fix23 commit 6: WebRTC peer connections for preview.
# Optional dependency: heavy (libsrtp2/usrsctp/libjuice/plog) and
# requires libsrtp2-dev from apt; OpenSSL is already pulled in by SRT.
# Prod builds without the preview feature use -DENABLE_WEBRTC_PREVIEW=OFF
# and don't pay the extra ~30s of build time.
option(ENABLE_WEBRTC_PREVIEW
       "Build libdatachannel-based WebRTC preview pipeline (fix23)" OFF)
if(ENABLE_WEBRTC_PREVIEW)
    # libdatachannel vendors usrsctp / libjuice / plog, and also builds
    # libsrtp2 itself (USE_SYSTEM_SRTP=OFF): its FindlibSRTP.cmake looks
    # for `srtp/srtp.h`, but Debian/Ubuntu install headers to
    # `/usr/include/srtp2/`, so find_package fails despite libsrtp2-dev
    # being installed. Vendoring removes the distro-specific path coupling.
    set(NO_EXAMPLES        ON  CACHE BOOL "" FORCE)
    set(NO_TESTS           ON  CACHE BOOL "" FORCE)
    set(USE_SYSTEM_SRTP    OFF CACHE BOOL "" FORCE)
    set(NO_MEDIA           OFF CACHE BOOL "" FORCE)  # need rtcp/rtp helpers
    set(NO_WEBSOCKET       ON  CACHE BOOL "" FORCE)
    FetchContent_Declare(libdatachannel
        GIT_REPOSITORY https://github.com/paullouisageneau/libdatachannel.git
        GIT_TAG        v0.21.0
        GIT_SHALLOW    TRUE
        GIT_PROGRESS   TRUE
        # libdatachannel vendors usrsctp/libjuice/plog/libsrtp2 as submodules,
        # the clone takes minutes. GIT_PROGRESS prints "Receiving objects: N%…"
        # so it doesn't look stuck at 22%.
    )
    FetchContent_MakeAvailable(libdatachannel)
endif()

# libsrt — built from source
FetchContent_Declare(srt
    GIT_REPOSITORY https://github.com/Haivision/srt.git
    GIT_TAG        v1.5.3
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE
)
set(ENABLE_APPS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(ENABLE_SHARED OFF CACHE BOOL "" FORCE)
set(ENABLE_STATIC ON CACHE BOOL "" FORCE)
set(USE_OPENSSL ON CACHE BOOL "" FORCE)
set(USE_GNUTLS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(srt)

# srt headers live in srtcore/, but the code expects <srt/srt.h>.
# version.h is generated in srt_BINARY_DIR — add both paths.
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/srt-include)
file(CREATE_LINK ${srt_SOURCE_DIR}/srtcore ${CMAKE_BINARY_DIR}/srt-include/srt SYMBOLIC)
target_include_directories(srt_static INTERFACE
    ${CMAKE_BINARY_DIR}/srt-include
    ${srt_BINARY_DIR}
)

# Silence vendored code — warnings from SRT/spdlog/libdatachannel/usrsctp
# drown out the ones from src/.
foreach(_vendored_tgt
        srt_static srt_virtual
        spdlog
        datachannel-static usrsctp usrsctp-static
        juice juice-static
        date-tz)
    if(TARGET ${_vendored_tgt})
        target_compile_options(${_vendored_tgt} PRIVATE -w)
        get_target_property(_inc ${_vendored_tgt} INTERFACE_INCLUDE_DIRECTORIES)
        if(_inc)
            set_target_properties(${_vendored_tgt} PROPERTIES
                INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_inc}")
        endif()
    endif()
endforeach()

set(SOURCES
    src/utils/Log.cpp
    src/utils/LogFFmpeg.cpp
    src/utils/PathUtils.cpp
    src/utils/Paths.cpp
    src/utils/Tls.cpp
    src/utils/TlsConfig.cpp
    src/utils/CpuAffinity.cpp
    src/utils/LegacyMigration.cpp
    src/utils/NetIfaces.cpp
    src/utils/SystemdNotifier.cpp
    src/utils/SimdRuntime.cpp
    src/utils/RuntimeLock.cpp
    src/core/ChannelInstance.cpp
    src/core/RenderLoop.cpp
    src/core/Timeline.cpp
    src/core/ScheduleEntry.cpp
    src/core/Scheduler.cpp
    src/core/ScheduleController.cpp
    src/core/FramePool.cpp
    src/core/Preloader.cpp
    src/core/Watchdog.cpp
    src/metrics/ChannelHealth.cpp
    src/metrics/ChannelProfiler.cpp
    src/metrics/ProfileSampler.cpp
    src/clips/ClipFactory.cpp
    src/clips/ClipGraveyard.cpp
    src/clips/MediaValidator.cpp
    src/content/CacheManager.cpp
    src/content/ShareScanner.cpp
    src/content/ContentSync.cpp
    src/clips/ImageClip.cpp
    src/clips/VideoClip.cpp
    src/clips/FallbackClip.cpp
    src/clips/MulticastInput.cpp
    src/clips/NdiInput.cpp
    src/clips/RtmpInput.cpp
    src/clips/RtspInput.cpp
    src/clips/LiveInputFactory.cpp
    src/clips/LiveClip.cpp
    src/clips/OnLossProvider.cpp
    src/clips/PlaylistEntry.cpp
    src/render/CpuCompositor.cpp
    src/transitions/CrossFade.cpp
    src/transitions/CrossFadeAvx2.cpp
    src/transitions/Wipe.cpp
    src/transitions/Push.cpp
    src/transitions/Dissolve.cpp
    src/transitions/FadeToBlack.cpp
    src/scaler/Scaler.cpp
    src/audio/AudioDecoder.cpp
    src/audio/AudioNormalizer.cpp
    src/audio/AudioMixer.cpp
    src/audio/AudioMixerAvx2.cpp
    src/audio/AudioEncoder.cpp
    src/encoding/Encoder.cpp
    src/encoding/EncoderFactory.cpp
    src/encoding/X264VideoEncoder.cpp
    src/encoding/Mpeg2VideoEncoder.cpp
    src/encoding/NvencVideoEncoder.cpp
    src/encoding/QsvVideoEncoder.cpp
    src/encoding/VaapiVideoEncoder.cpp
    src/output/OutputManager.cpp
    src/output/SrtOutput.cpp
    src/output/RtmpOutput.cpp
    src/output/HlsOutput.cpp
    src/output/RtspOutput.cpp
    src/output/MulticastOutput.cpp
    src/output/NdiLib.cpp
    src/output/NdiOutput.cpp
    src/api/ControlApi.cpp
    src/api/ChannelManager.cpp
    src/api/MetricsCollector.cpp
    src/api/RbacScope.cpp
    src/api/SseFilter.cpp
    src/events/EventBus.cpp
    src/preview/PreviewEncoder.cpp
    src/preview/PreviewManager.cpp
    src/preview/WebRtcSession.cpp
    src/logging/FilePlaybackSink.cpp
    src/logging/SqlitePlaybackSink.cpp
    src/metrics/ProcessMetrics.cpp
    src/metrics/HostMetrics.cpp
    src/persistence/StatePersistence.cpp
    src/persistence/ChannelStateSaver.cpp
    src/gateway/Gateway.cpp
    src/gateway/DemuxGateway.cpp
    src/gateway/RemuxGateway.cpp
    src/gateway/net/UdpSocket.cpp
    src/gateway/probe/PsiProbe.cpp
    src/gateway/TranscodeGateway.cpp
    src/gateway/GatewayFactory.cpp
    src/gateway/GatewayCfg.cpp
    src/gateway/GatewayManager.cpp
    src/gateway/ts/Crc32.cpp
    src/gateway/ts/Descriptors.cpp
    src/gateway/ts/PsiParser.cpp
    src/gateway/ts/PsiBuilder.cpp
    src/gateway/ts/ProgramTable.cpp
    src/gateway/ts/PcrRestamper.cpp
    src/gateway/ts/PesAssembler.cpp
    src/gateway/fec/RtpEncapsulator.cpp
    src/gateway/fec/FecMatrix.cpp
    src/gateway/fec/FecEncoder.cpp
    src/gateway/transcode/VideoTranscoder.cpp
    src/gateway/transcode/AudioTranscoder.cpp
    src/gateway/transcode/FfmpegTranscodePipeline.cpp
    src/audit/AuditTypes.cpp
    src/audit/AuditDb.cpp
    src/audit/AuditLogger.cpp
    src/auth/AuthTypes.cpp
    src/auth/AuthDb.cpp
    src/auth/PasswordHasher.cpp
    src/auth/JwtIssuer.cpp
    src/auth/AuthService.cpp
    src/auth/MasterKey.cpp
    src/auth/MasterKeyReset.cpp
    src/auth/RbacMiddleware.cpp
    src/auth/AuthCli.cpp
    src/auth/LdapClient.cpp
    src/auth/LdapConfigRepo.cpp
    src/auth/SmtpClient.cpp
    src/auth/SmtpConfigRepo.cpp
    src/auth/TimeConfig.cpp
    src/auth/TimeConfigRepo.cpp
    src/auth/TimeSource.cpp
    src/auth/SntpClient.cpp
    src/auth/NtpSource.cpp
    src/stress/StressConfig.cpp
    src/stress/StressReport.cpp
    src/stress/StressRunner.cpp
    src/stress/RandomOutputFail.cpp
    src/stress/ClipCorrupt.cpp
    src/stress/NetworkBlackout.cpp
    src/stress/ScenarioFactory.cpp
    src/stress/CronExpr.cpp
    src/stress/StressScheduler.cpp
    src/stress/StressReportStore.cpp
    src/stress/StressService.cpp
    src/plugins/PluginManager.cpp
    # fix41 — Storage Mounts (CIFS/NFS via systemd .mount units)
    src/mounts/MountSpec.cpp
    src/mounts/RpcProtocol.cpp
    src/mounts/Subprocess.cpp
    src/mounts/UnitGenerator.cpp
    src/mounts/SystemctlClient.cpp
    src/mounts/CredsHelper.cpp
    src/mounts/MountTester.cpp
    src/mounts/MountdHandlers.cpp
    src/mounts/MountsDb.cpp
    src/mounts/MountdRpcClient.cpp
    src/mounts/MountManager.cpp
)

# OBJECT library so the daemon and stress_runner_cli (fix26 c13) share
# the same compiled engine without duplicating link-time work.
add_library(liveqx-objs OBJECT ${SOURCES})
target_include_directories(liveqx-objs PUBLIC src)
target_link_libraries(liveqx-objs PUBLIC
    PkgConfig::FFMPEG
    httplib::httplib
    spdlog::spdlog
    nlohmann_json::nlohmann_json
    srt_static
    SQLite::SQLite3
    PkgConfig::SODIUM
    jwt-cpp::jwt-cpp
    date::date-tz
    openldap_client
    CURL::libcurl
    OpenSSL::SSL
    OpenSSL::Crypto
    ${CMAKE_DL_LIBS}
)
if(ENABLE_WEBRTC_PREVIEW)
    # Link the static target so libdatachannel.a is vendored into the
    # binary — the shared target would require shipping libdatachannel.so
    # into /usr/local/lib alongside the executable, which we don't want
    # for a single-binary deploy. The static target is EXCLUDE_FROM_ALL
    # upstream, so referencing it here is what forces it to be built.
    target_link_libraries(liveqx-objs PUBLIC
        LibDataChannel::LibDataChannelStatic
    )
    target_compile_definitions(liveqx-objs PUBLIC
        LIVEQX_ENABLE_WEBRTC_PREVIEW=1)
endif()
target_compile_options(liveqx-objs PRIVATE
    -Wall -Wextra -Wpedantic
    -march=native
)

add_executable(liveqx src/main.cpp)
target_link_libraries(liveqx PRIVATE liveqx-objs)

# fix28 — AVX2 path is gated at runtime by liveqx::simd::current().
# We compile only the SIMD-specific translation units with -mavx2 so the
# rest of the binary remains runnable on pre-AVX2 hardware.
set_source_files_properties(
    src/transitions/CrossFadeAvx2.cpp
    src/audio/AudioMixerAvx2.cpp
    PROPERTIES COMPILE_OPTIONS "-mavx2")

# Build identity injected as preprocessor macros (fix16). Reproducibility
# tools can override LIVEQX_BUILD_TIME to a fixed timestamp; commit
# stays as "unknown" outside a git checkout (e.g. tarball builds).
execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE LIVEQX_GIT_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT LIVEQX_GIT_COMMIT)
    set(LIVEQX_GIT_COMMIT "unknown")
endif()
execute_process(
    COMMAND git diff --quiet HEAD --
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    RESULT_VARIABLE LIVEQX_GIT_DIRTY
    ERROR_QUIET
)
if(LIVEQX_GIT_DIRTY)
    set(LIVEQX_GIT_COMMIT "${LIVEQX_GIT_COMMIT}-dirty")
endif()
string(TIMESTAMP LIVEQX_BUILD_TIME "%Y-%m-%dT%H:%M:%SZ" UTC)

# Version = (commit count + BASE), format MAJOR.MINOR; every 100 commits bumps major by 1.
# BASE=503 → the initial public release version is 5.04.
set(LIVEQX_VERSION_BASE 503)
execute_process(
    COMMAND git rev-list --count HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE LIVEQX_COMMIT_COUNT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(LIVEQX_COMMIT_COUNT MATCHES "^[0-9]+$")
    math(EXPR _liveqx_ver_n "${LIVEQX_COMMIT_COUNT} + ${LIVEQX_VERSION_BASE}")
    math(EXPR LIVEQX_VERSION_MAJOR "${_liveqx_ver_n} / 100")
    math(EXPR LIVEQX_VERSION_MINOR "${_liveqx_ver_n} % 100")
    # Zero-pad minor to two digits: 4.07, 4.94, 5.00 — sorts lexicographically.
    if(LIVEQX_VERSION_MINOR LESS 10)
        set(LIVEQX_VERSION_STR "${LIVEQX_VERSION_MAJOR}.0${LIVEQX_VERSION_MINOR}")
    else()
        set(LIVEQX_VERSION_STR "${LIVEQX_VERSION_MAJOR}.${LIVEQX_VERSION_MINOR}")
    endif()
else()
    set(LIVEQX_VERSION_STR "${PROJECT_VERSION}")
endif()

execute_process(
    COMMAND git symbolic-ref --short HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE LIVEQX_GIT_BRANCH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
set(_git_dir "${CMAKE_SOURCE_DIR}/.git")
if(EXISTS "${_git_dir}/HEAD")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_git_dir}/HEAD")
endif()
if(LIVEQX_GIT_BRANCH AND EXISTS "${_git_dir}/refs/heads/${LIVEQX_GIT_BRANCH}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${_git_dir}/refs/heads/${LIVEQX_GIT_BRANCH}")
endif()

message(STATUS "LiveQX version: ${LIVEQX_VERSION_STR} (${LIVEQX_COMMIT_COUNT} commits, ${LIVEQX_GIT_COMMIT})")

target_compile_definitions(liveqx-objs PUBLIC
    LIVEQX_VERSION="${LIVEQX_VERSION_STR}"
    LIVEQX_COMMIT="${LIVEQX_GIT_COMMIT}"
    LIVEQX_BUILD_TIME="${LIVEQX_BUILD_TIME}"
    LIVEQX_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
)

target_compile_options(liveqx PRIVATE
    -Wall -Wextra -Wpedantic
    -march=native
)

# ── fix26 c13 — stress_runner_cli ────────────────────────────────────────
# Standalone runner that loads a stress JSON, kicks StressRunner::runOnce
# against a bare ChannelManager, and prints the report to stdout. Reuses
# the engine OBJECT library so behaviour matches the daemon exactly.
add_executable(stress_runner_cli src/stress/StressRunnerCli.cpp)
target_link_libraries(stress_runner_cli PRIVATE liveqx-objs)
target_compile_options(stress_runner_cli PRIVATE
    -Wall -Wextra -Wpedantic
    -march=native
)

# ── fix41 — liveqx-mountd (privileged helper) ───────────────────
# Standalone binary, runs as root. Listens on AF_UNIX, applies systemd
# .mount/.automount units for CIFS/NFS shares on behalf of liveqx.
# Deliberately NOT linked against liveqx-objs (which pulls in
# ffmpeg/srt/openldap and a 100+ MB closure). Helper only needs
# nlohmann_json + spdlog + system headers — kept lightweight to minimise
# privileged attack surface.
add_executable(liveqx-mountd
    src/mounts/MountdMain.cpp
    src/mounts/RpcServer.cpp
    src/mounts/RpcProtocol.cpp
    src/mounts/MountSpec.cpp
    src/mounts/Subprocess.cpp
    src/mounts/UnitGenerator.cpp
    src/mounts/SystemctlClient.cpp
    src/mounts/CredsHelper.cpp
    src/mounts/MountTester.cpp
    src/mounts/MountdHandlers.cpp
)
target_include_directories(liveqx-mountd PRIVATE src)
target_link_libraries(liveqx-mountd PRIVATE
    spdlog::spdlog
    nlohmann_json::nlohmann_json
)
target_compile_options(liveqx-mountd PRIVATE
    -Wall -Wextra -Wpedantic
    -march=native
)

enable_testing()
add_subdirectory(tests)
