#!/usr/bin/env bash
#
# LiveQX Engine — bootstrap для чистого сервера.
#
# Делает на голой Ubuntu/Debian всё с нуля:
#   1. Ставит build-tools и dev-пакеты через apt
#   2. Собирает backend (cmake --build)
#   3. Собирает Web UI (npm ci && npm run build)
#   4. Запускает packaging/install.sh для деплоя
#
# Запуск из корня репозитория:
#   sudo ./packaging/bootstrap.sh [-y]
#
# Или одной строкой на чистом сервере (репо ещё не склонен):
#   curl -fsSL https://raw.githubusercontent.com/MrGospo/LiveQX/main/packaging/bootstrap.sh \
#       | sudo REPO_URL=https://github.com/MrGospo/LiveQX.git CLONE_DIR=/opt/liveqx-src bash
#
set -euo pipefail
IFS=$'\n\t'

# ── Цветной вывод ───────────────────────────────────────────────────────────
if [[ -t 1 ]]; then
    C_RED=$'\033[0;31m'; C_GRN=$'\033[0;32m'; C_YEL=$'\033[1;33m'
    C_BLU=$'\033[0;34m'; C_BLD=$'\033[1m';     C_RST=$'\033[0m'
else
    C_RED=''; C_GRN=''; C_YEL=''; C_BLU=''; C_BLD=''; C_RST=''
fi
info(){ echo "${C_BLU}[i]${C_RST} $*"; }
ok()  { echo "${C_GRN}[✓]${C_RST} $*"; }
warn(){ echo "${C_YEL}[!]${C_RST} $*"; }
err() { echo "${C_RED}[✗]${C_RST} $*" >&2; }
die() { err "$*"; exit 1; }
step(){ echo; echo "${C_BLD}── $* ──${C_RST}"; }

# ── Параметры ───────────────────────────────────────────────────────────────
ASSUME_YES=0
for arg in "$@"; do
    case "$arg" in
        -y|--yes) ASSUME_YES=1 ;;
        -h|--help)
            cat <<EOF
LiveQX Engine — bootstrap (apt → build → install)

Использование:
    sudo ./packaging/bootstrap.sh [-y]

Переменные окружения (для curl-режима):
    REPO_URL    URL git-репозитория (по умолчанию: из CWD, если это git-репо)
    REPO_REF    ветка/тег/коммит для checkout (по умолчанию: main)
    CLONE_DIR   куда клонировать репо (по умолчанию: /opt/liveqx-src)
    JOBS        число параллельных задач сборки (по умолчанию: nproc)

Опции:
    -y, --yes   Принимать все подтверждения автоматически
    -h, --help  Эта справка
EOF
            exit 0
            ;;
        *) die "Неизвестный аргумент: $arg (см. --help)" ;;
    esac
done

REPO_URL="${REPO_URL:-}"
REPO_REF="${REPO_REF:-main}"
CLONE_DIR="${CLONE_DIR:-/opt/liveqx-src}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 2)}"

[[ $EUID -eq 0 ]] || die "Запустите через sudo: sudo $0"

# ── ОС ──────────────────────────────────────────────────────────────────────
[[ -f /etc/os-release ]] || die "Не найден /etc/os-release"
# shellcheck disable=SC1091
. /etc/os-release
case "${ID:-}" in
    ubuntu|debian) ;;
    *) die "Поддерживаются только Ubuntu и Debian (нашёл: ${ID:-unknown})" ;;
esac
info "Дистрибутив: ${PRETTY_NAME:-$ID}"

# ── Где репозиторий ─────────────────────────────────────────────────────────
# Случай 1: скрипт запущен из репо (CWD содержит CMakeLists.txt + packaging/)
# Случай 2: curl | bash — репо ещё не существует, клонируем
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)" || SCRIPT_DIR=""
REPO_ROOT=""
if [[ -n "$SCRIPT_DIR" && -f "$SCRIPT_DIR/../CMakeLists.txt" ]]; then
    REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
    info "Репозиторий уже здесь: $REPO_ROOT"
elif [[ -d "$CLONE_DIR/.git" ]]; then
    REPO_ROOT="$CLONE_DIR"
    info "Использую существующий клон: $REPO_ROOT"
elif [[ -n "$REPO_URL" ]]; then
    info "Клонирую $REPO_URL → $CLONE_DIR"
else
    die "Не определить корень репо. Запустите из корня или укажите REPO_URL=..."
fi

# ── Apt build-deps ──────────────────────────────────────────────────────────
step "Build-зависимости через apt"
info "Обновляю индекс apt..."
apt-get update -qq

# Базовые build-tools + dev-пакеты библиотек, которые проект тянет из системы.
# FFmpeg, cpp-httplib, spdlog/fmt, jwt-cpp — vendored через FetchContent или
# third_party/ffmpeg, поэтому их dev-пакеты НЕ нужны.
BUILD_DEPS=(
    build-essential cmake git pkg-config python3
    ca-certificates curl
    nlohmann-json3-dev libgtest-dev
    libsqlite3-dev libsodium-dev libldap2-dev
    libssl-dev libcurl4-openssl-dev
    # Транзитивные dev-зависимости вендорного FFmpeg (x264 + zlib + VAAPI + QSV).
    libx264-dev zlib1g-dev libva-dev libdrm-dev libmfx-dev
)

# Node.js для сборки UI. Node 20.x есть в репах Ubuntu 24.04 (nodejs пакет)
# и Debian 13. Для старых LTS (Ubuntu 22.04, Debian 12) штатный nodejs
# слишком старый (v12/v18) — добавим NodeSource при необходимости.
NODE_OK=0
if command -v node >/dev/null 2>&1; then
    NODE_VER=$(node -v 2>/dev/null | sed 's/^v//' | cut -d. -f1)
    if [[ "${NODE_VER:-0}" -ge 20 ]]; then
        NODE_OK=1
        info "Node.js уже установлен: v$(node -v | sed 's/^v//')"
    else
        warn "Node.js v$NODE_VER слишком старый, нужен >=20"
    fi
fi
if [[ "$NODE_OK" == "0" ]]; then
    # Проверяем штатный nodejs-пакет
    APT_NODE_VER=$(apt-cache madison nodejs 2>/dev/null | head -1 | awk '{print $3}' | cut -d. -f1 | sed 's/[^0-9]//g')
    if [[ -n "$APT_NODE_VER" && "${APT_NODE_VER:-0}" -ge 20 ]]; then
        info "Ставлю nodejs+npm из штатных репозиториев (v${APT_NODE_VER})"
        BUILD_DEPS+=(nodejs npm)
    else
        info "Подключаю NodeSource 20.x (штатный nodejs слишком старый)"
        curl -fsSL https://deb.nodesource.com/setup_20.x | bash -
        BUILD_DEPS+=(nodejs)
    fi
fi

info "Устанавливаю: ${BUILD_DEPS[*]}"
DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends "${BUILD_DEPS[@]}" \
    || die "apt-get install не сработал. Проверьте включён ли universe (Ubuntu) или non-free (Debian)."
ok "Build-зависимости установлены"

# ── Клон (если нужно) ───────────────────────────────────────────────────────
if [[ -z "$REPO_ROOT" ]]; then
    step "Клонирование репозитория"
    if [[ -e "$CLONE_DIR" && ! -d "$CLONE_DIR/.git" ]]; then
        die "$CLONE_DIR существует и не является git-репо — удалите вручную или укажите другой CLONE_DIR"
    fi
    if [[ ! -d "$CLONE_DIR/.git" ]]; then
        git clone --depth 50 --branch "$REPO_REF" "$REPO_URL" "$CLONE_DIR"
    fi
    REPO_ROOT="$CLONE_DIR"
    ok "Репо в $REPO_ROOT"
fi

# ── Сборка backend ──────────────────────────────────────────────────────────
step "Сборка backend (cmake, -j$JOBS)"
cd "$REPO_ROOT"

# CMakeCache.txt хранит абсолютные пути — если build/ достался от копии
# исходников в другом каталоге, cmake откажется работать. Пересоздаём.
if [[ -f build/CMakeCache.txt ]]; then
    cached_src=$(awk -F= '/^CMAKE_HOME_DIRECTORY:/ {print $2; exit}' build/CMakeCache.txt || true)
    if [[ -n "$cached_src" && "$cached_src" != "$REPO_ROOT" ]]; then
        warn "build/ от другого пути ($cached_src → $REPO_ROOT), пересоздаю"
        rm -rf build
    fi
fi

# Фильтр вывода сборки: показываем прогресс + ошибки, warnings уводим в лог.
# GCC 12 сыпет false-positive'ы -Wstringop-overflow на std::copy — засоряют
# экран и путают пользователя. Полный лог всегда доступен в build.log.
BUILD_LOG="$REPO_ROOT/build.log"
run_quiet() {
    local rc
    "$@" 2>&1 | tee "$BUILD_LOG" | grep --line-buffered -E \
        "^\[[ 0-9]+%\]|^-- |error:|Error |Ошибк|FAILED|undefined reference|Linking (CXX|C) (static|executable)" \
        || true
    rc=${PIPESTATUS[0]}
    if [[ $rc -ne 0 ]]; then
        err "Команда упала (rc=$rc). Хвост build.log:"
        tail -80 "$BUILD_LOG" >&2
        die "Сборка прервана. Полный лог: $BUILD_LOG"
    fi
}

# HW-энкодеры включены на этапе компиляции — финальный выбор в UI/конфиге.
# FETCHCONTENT_QUIET=OFF, чтобы прогресс clone'ов libdatachannel/SRT был виден.
run_quiet cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
      -DFETCHCONTENT_QUIET=OFF \
      -DENABLE_NVENC=ON -DENABLE_QSV=ON -DENABLE_VAAPI=ON \
      -DENABLE_WEBRTC_PREVIEW=ON -DENABLE_SYSTEMD=ON

run_quiet cmake --build build -j"$JOBS" --target liveqx liveqx-mountd
ok "Backend собран: build/liveqx, build/liveqx-mountd"

# ── Сборка UI ───────────────────────────────────────────────────────────────
step "Сборка Web UI (npm ci → npm run build)"
cd "$REPO_ROOT/ui"
if [[ -f package-lock.json ]]; then
    npm ci --no-audit --no-fund --silent
else
    warn "package-lock.json отсутствует — использую npm install"
    npm install --no-audit --no-fund --silent
fi
# vite build пишет прогресс в stderr, TS-ошибки — тоже видны. Фильтровать
# минимально: убираем шум "transforming" и "rendering".
UI_LOG="$REPO_ROOT/ui/build.log"
if ! npm run build 2>&1 | tee "$UI_LOG" | grep --line-buffered -E \
        "^> |vite v|building|✓|error|Error|dist/|modules transformed"; then
    err "UI-сборка упала. Хвост ui/build.log:"
    tail -80 "$UI_LOG" >&2
    die "npm run build прервана. Полный лог: $UI_LOG"
fi
[[ -d dist ]] || die "ui/dist/ не создан после npm run build"
ok "UI собран: ui/dist/"
cd "$REPO_ROOT"

# ── Установка ───────────────────────────────────────────────────────────────
step "Запуск install.sh"
INSTALL_ARGS=(install)
[[ "$ASSUME_YES" == "1" ]] && INSTALL_ARGS+=(-y)
bash "$REPO_ROOT/packaging/install.sh" "${INSTALL_ARGS[@]}"

echo
ok "Bootstrap завершён. Сервис должен быть запущен."
