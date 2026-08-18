#!/usr/bin/env bash
#
# LiveQX Engine — bootstrap for a clean server.
#
# Does everything from scratch on a bare Ubuntu/Debian:
#   1. Installs build-tools and dev packages via apt
#   2. Builds backend (cmake --build)
#   3. Builds Web UI (npm ci && npm run build)
#   4. Runs packaging/install.sh to deploy
#
# Run from the repository root:
#   sudo ./packaging/bootstrap.sh [-y]
#
# Or as a one-liner on a clean server (repo not yet cloned):
#   curl -fsSL https://raw.githubusercontent.com/MrGospo/LiveQX/main/packaging/bootstrap.sh \
#       | sudo REPO_URL=https://github.com/MrGospo/LiveQX.git CLONE_DIR=/opt/liveqx-src bash
#
# -E (errtrace) — required so the ERR trap fires inside functions,
# subshells and command substitutions (e.g. run_quiet / cmake / npm).
set -Eeuo pipefail
IFS=$'\n\t'

# ── Colored output ──────────────────────────────────────────────────────────
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

# CURRENT_STEP is updated by step() and printed by on_error so a failure
# report always names the phase (apt / clone / backend / UI / install).
CURRENT_STEP=""
step() {
    CURRENT_STEP="$*"
    echo
    echo "${C_BLD}── $* ──${C_RST}"
}

# Fires on any command that exits non-zero under `set -e` (and inside
# functions/subshells thanks to `set -E`). Prints phase + line + command
# + exit code so the user does not have to re-run with bash -x.
on_error() {
    local exit_code=$1 line=$2 cmd=$3
    echo
    err "════════════════════════════════════════════════════════════════"
    err "Bootstrap aborted at step: ${CURRENT_STEP:-<startup>}"
    err "  line:    $line"
    err "  command: $cmd"
    err "  exit:    $exit_code"
    err "════════════════════════════════════════════════════════════════"
    err "Build logs (if produced): build.log, ui/build.log"
    err "For a full shell trace: bash -x $0 2>&1 | tee /tmp/bootstrap.trace"
    exit "$exit_code"
}
trap 'on_error $? $LINENO "$BASH_COMMAND"' ERR

# ── Parameters ──────────────────────────────────────────────────────────────
ASSUME_YES=0
for arg in "$@"; do
    case "$arg" in
        -y|--yes) ASSUME_YES=1 ;;
        -h|--help)
            cat <<EOF
LiveQX Engine — bootstrap (apt → build → install)

Usage:
    sudo ./packaging/bootstrap.sh [-y]

Environment variables (for curl mode):
    REPO_URL    git repository URL (default: from CWD if it is a git repo)
    REPO_REF    branch/tag/commit to check out (default: main)
    CLONE_DIR   where to clone the repo (default: /opt/liveqx-src)
    JOBS        number of parallel build jobs (default: nproc)

Options:
    -y, --yes   Assume yes for all prompts
    -h, --help  This help text
EOF
            exit 0
            ;;
        *) die "Unknown argument: $arg (see --help)" ;;
    esac
done

REPO_URL="${REPO_URL:-}"
REPO_REF="${REPO_REF:-main}"
CLONE_DIR="${CLONE_DIR:-/opt/liveqx-src}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 2)}"

[[ $EUID -eq 0 ]] || die "Run via sudo: sudo $0"

# ── OS ──────────────────────────────────────────────────────────────────────
[[ -f /etc/os-release ]] || die "/etc/os-release not found"
# shellcheck disable=SC1091
. /etc/os-release
case "${ID:-}" in
    ubuntu|debian) ;;
    *) die "Only Ubuntu and Debian are supported (found: ${ID:-unknown})" ;;
esac
info "Distribution: ${PRETTY_NAME:-$ID}"

# ── Repository location ─────────────────────────────────────────────────────
# Case 1: script launched from the repo (CWD contains CMakeLists.txt + packaging/)
# Case 2: curl | bash — repo does not yet exist, clone it
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)" || SCRIPT_DIR=""
REPO_ROOT=""
if [[ -n "$SCRIPT_DIR" && -f "$SCRIPT_DIR/../CMakeLists.txt" ]]; then
    REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
    info "Repository already here: $REPO_ROOT"
elif [[ -d "$CLONE_DIR/.git" ]]; then
    REPO_ROOT="$CLONE_DIR"
    info "Using existing clone: $REPO_ROOT"
elif [[ -n "$REPO_URL" ]]; then
    info "Cloning $REPO_URL → $CLONE_DIR"
else
    die "Cannot determine repo root. Run from the repo root or set REPO_URL=..."
fi

# ── Apt build-deps ──────────────────────────────────────────────────────────
step "Build dependencies via apt"
info "Refreshing apt index..."
apt-get update -qq \
    || die "apt-get update failed (check network, DNS and /etc/apt/sources.list)"

# Base build-tools + dev packages for libraries the project pulls from the system.
# FFmpeg, cpp-httplib, spdlog/fmt, jwt-cpp are vendored via FetchContent or
# third_party/ffmpeg, so their dev packages are NOT needed.
BUILD_DEPS=(
    build-essential cmake git pkg-config python3
    ca-certificates curl
    nlohmann-json3-dev libgtest-dev
    libsqlite3-dev libsodium-dev libldap2-dev
    libssl-dev libcurl4-openssl-dev
    # Transitive dev-deps of the vendored FFmpeg (x264 + zlib + VAAPI + QSV).
    libx264-dev zlib1g-dev libva-dev libdrm-dev libmfx-dev
)

# Node.js for building the UI. Node 20.x is in Ubuntu 24.04 repos (nodejs package)
# and Debian 13. For older LTS releases (Ubuntu 22.04, Debian 12) the stock
# nodejs is too old (v12/v18) — add NodeSource if needed.
NODE_OK=0
if command -v node >/dev/null 2>&1; then
    NODE_VER=$(node -v 2>/dev/null | sed 's/^v//' | cut -d. -f1)
    if [[ "${NODE_VER:-0}" -ge 20 ]]; then
        NODE_OK=1
        info "Node.js already installed: v$(node -v | sed 's/^v//')"
    else
        warn "Node.js v$NODE_VER is too old, need >=20"
    fi
fi
if [[ "$NODE_OK" == "0" ]]; then
    # Check the stock nodejs package
    APT_NODE_VER=$(apt-cache madison nodejs 2>/dev/null | head -1 | awk '{print $3}' | cut -d. -f1 | sed 's/[^0-9]//g')
    if [[ -n "$APT_NODE_VER" && "${APT_NODE_VER:-0}" -ge 20 ]]; then
        info "Installing nodejs+npm from stock repositories (v${APT_NODE_VER})"
        BUILD_DEPS+=(nodejs npm)
    else
        info "Adding NodeSource 20.x (stock nodejs is too old)"
        # NOTE: do not pipe curl into bash. The NodeSource setup script
        # exits before consuming all of stdin, curl gets SIGPIPE on its next
        # write, and `set -o pipefail` then marks the whole pipeline failed
        # (exit 141) even though bash itself succeeded. Download first,
        # execute from a file — bash reads a fully-written file, no pipe.
        ns_setup="$(mktemp)"
        curl -fsSL https://deb.nodesource.com/setup_20.x -o "$ns_setup" \
            || die "Failed to download NodeSource setup_20.x (network / proxy?)"
        bash "$ns_setup" \
            || { rm -f "$ns_setup"; die "NodeSource setup script failed (see output above)"; }
        rm -f "$ns_setup"
        BUILD_DEPS+=(nodejs)
    fi
fi

info "Installing: ${BUILD_DEPS[*]}"
DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends "${BUILD_DEPS[@]}" \
    || die "apt-get install failed. Check that universe (Ubuntu) or non-free (Debian) is enabled."
ok "Build dependencies installed"

# ── Clone (if needed) ───────────────────────────────────────────────────────
if [[ -z "$REPO_ROOT" ]]; then
    step "Cloning repository"
    if [[ -e "$CLONE_DIR" && ! -d "$CLONE_DIR/.git" ]]; then
        die "$CLONE_DIR exists and is not a git repo — remove it manually or set another CLONE_DIR"
    fi
    if [[ ! -d "$CLONE_DIR/.git" ]]; then
        git clone --depth 50 --branch "$REPO_REF" "$REPO_URL" "$CLONE_DIR" \
            || die "git clone failed (repo: $REPO_URL, ref: $REPO_REF, target: $CLONE_DIR)"
    fi
    REPO_ROOT="$CLONE_DIR"
    ok "Repo at $REPO_ROOT"
fi

# ── Backend build ───────────────────────────────────────────────────────────
step "Building backend (cmake, -j$JOBS)"
cd "$REPO_ROOT"

# CMakeCache.txt stores absolute paths — if build/ was copied from a different
# source directory, cmake will refuse to work. Recreate it.
if [[ -f build/CMakeCache.txt ]]; then
    cached_src=$(awk -F= '/^CMAKE_HOME_DIRECTORY:/ {print $2; exit}' build/CMakeCache.txt || true)
    if [[ -n "$cached_src" && "$cached_src" != "$REPO_ROOT" ]]; then
        warn "build/ is from a different path ($cached_src → $REPO_ROOT), recreating"
        rm -rf build
    fi
fi

# Build output filter: show progress + errors, warnings go to the log.
# GCC 12 spits out false-positive -Wstringop-overflow on std::copy — they
# pollute the screen and confuse the user. Full log is always in build.log.
BUILD_LOG="$REPO_ROOT/build.log"
run_quiet() {
    local rc
    "$@" 2>&1 | tee "$BUILD_LOG" | grep --line-buffered -E \
        "^\[[ 0-9]+%\]|^-- |error:|Error |FAILED|undefined reference|Linking (CXX|C) (static|executable)" \
        || true
    rc=${PIPESTATUS[0]}
    if [[ $rc -ne 0 ]]; then
        err "Command failed (rc=$rc). Tail of build.log:"
        tail -80 "$BUILD_LOG" >&2
        die "Build aborted. Full log: $BUILD_LOG"
    fi
}

# HW encoders are enabled at compile time — the final choice is in the UI/config.
# FETCHCONTENT_QUIET=OFF so libdatachannel/SRT clone progress stays visible.
run_quiet cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
      -DFETCHCONTENT_QUIET=OFF \
      -DENABLE_NVENC=ON -DENABLE_QSV=ON -DENABLE_VAAPI=ON \
      -DENABLE_WEBRTC_PREVIEW=ON -DENABLE_SYSTEMD=ON

run_quiet cmake --build build -j"$JOBS" --target liveqx liveqx-mountd
ok "Backend built: build/liveqx, build/liveqx-mountd"

# ── UI build ────────────────────────────────────────────────────────────────
step "Building Web UI (npm ci → npm run build)"
cd "$REPO_ROOT/ui"
if [[ -f package-lock.json ]]; then
    npm ci --no-audit --no-fund --silent \
        || die "npm ci failed (check network, node/npm version, package-lock.json integrity)"
else
    warn "package-lock.json missing — falling back to npm install"
    npm install --no-audit --no-fund --silent \
        || die "npm install failed (check network and package.json)"
fi
# vite build writes progress to stderr, TS errors as well. Filter minimally:
# strip the "transforming" and "rendering" noise.
UI_LOG="$REPO_ROOT/ui/build.log"
if ! npm run build 2>&1 | tee "$UI_LOG" | grep --line-buffered -E \
        "^> |vite v|building|✓|error|Error|dist/|modules transformed"; then
    err "UI build failed. Tail of ui/build.log:"
    tail -80 "$UI_LOG" >&2
    die "npm run build aborted. Full log: $UI_LOG"
fi
[[ -d dist ]] || die "ui/dist/ not created after npm run build"
ok "UI built: ui/dist/"
cd "$REPO_ROOT"

# ── Install ─────────────────────────────────────────────────────────────────
step "Running install.sh"
INSTALL_ARGS=(install)
[[ "$ASSUME_YES" == "1" ]] && INSTALL_ARGS+=(-y)
bash "$REPO_ROOT/packaging/install.sh" "${INSTALL_ARGS[@]}" \
    || die "install.sh failed (see output above; also: sudo journalctl -u liveqx -n 80 --no-pager)"

echo
ok "All stages completed successfully."
ok "Bootstrap finished. The service should be running."
