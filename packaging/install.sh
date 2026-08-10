#!/usr/bin/env bash
#
# LiveQX Engine — installer for Ubuntu/Debian.
#
# Modes:
#   install    — install (default)
#   uninstall  — remove EVERYTHING (binaries, units, DB, configs, user)
#   check      — check current installation state
#
# Usage: sudo ./packaging/install.sh [install|uninstall|check] [-y]
#
set -euo pipefail
IFS=$'\n\t'

# On minimal Debian /usr/sbin and /sbin are missing from PATH for regular
# users — this causes groupadd/useradd/nologin to fail, even under sudo.
export PATH="/usr/local/sbin:/usr/sbin:/sbin:${PATH}"

# ── Path constants ──────────────────────────────────────────────────────────
LIVEQX_USER="liveqx"
LIVEQX_GROUP="liveqx"
BIN_DIR="/usr/local/bin"
STATE_DIR="/var/lib/liveqx"
CONF_DIR="/etc/liveqx"
CONF_FILE="${CONF_DIR}/config.json"
UI_DIR="/usr/share/liveqx/ui"
DOC_DIR="/usr/share/doc/liveqx"
SYSTEMD_DIR="/etc/systemd/system"
MOUNT_ROOT="/mnt/liveqx"
RUNTIME_DIR="/run/liveqx"

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"

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
step(){ echo; echo "${C_BLD}── $* ──${C_RST}"; }

usage() {
    cat <<EOF
LiveQX Engine — installer

Usage:
    sudo $0 [install|uninstall|check] [options]

Commands:
    install     Install LiveQX (default)
    uninstall   Fully remove LiveQX (DB, configs, liveqx user)
    check       Check installation state

Options:
    -y, --yes       Assume yes for all prompts
    -h, --help      Show this help

Before running install, make sure the following are built:
    build/liveqx               — main binary
    build/liveqx-mountd        — share-mount helper
    ui/dist/                   — Web UI

Build command:
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \\
        -DENABLE_NVENC=ON -DENABLE_QSV=ON -DENABLE_VAAPI=ON \\
        -DENABLE_WEBRTC_PREVIEW=ON -DENABLE_SYSTEMD=ON
    cmake --build build -j\$(nproc) --target liveqx liveqx-mountd
    (cd ui && npm ci && npm run build)
EOF
}

# ── Argument parsing ────────────────────────────────────────────────────────
MODE="install"
ASSUME_YES=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        install|uninstall|check) MODE="$1"; shift ;;
        -y|--yes)  ASSUME_YES=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) die "Unknown argument: $1 (see --help)" ;;
    esac
done

# ── Utilities ───────────────────────────────────────────────────────────────
ask_yn() {
    local q="$1" def="${2:-n}" prompt ans
    if [[ "$ASSUME_YES" == "1" ]]; then
        [[ "$def" == "y" ]]; return $?
    fi
    [[ "$def" == "y" ]] && prompt="[Y/n]" || prompt="[y/N]"
    while true; do
        read -r -p "${C_BLD}?${C_RST} ${q} ${prompt} " ans || ans=""
        ans="${ans:-$def}"
        case "$ans" in
            [Yy]|[Yy][Ee][Ss]) return 0 ;;
            [Nn]|[Nn][Oo])     return 1 ;;
            *) echo "Enter y or n" ;;
        esac
    done
}

detect_os() {
    [[ -f /etc/os-release ]] || die "/etc/os-release not found"
    # shellcheck disable=SC1091
    . /etc/os-release
    case "${ID:-}" in
        ubuntu|debian) ;;
        *) die "Only Ubuntu and Debian are supported (found: ${ID:-unknown})" ;;
    esac
    info "Distribution: ${PRETTY_NAME:-$ID}"
}

is_wsl() { grep -qiE "microsoft|wsl" /proc/version 2>/dev/null; }

require_root() {
    [[ $EUID -eq 0 ]] || die "Run via sudo: sudo $0 $MODE"
}

# ════════════════════════════════════════════════════════════════════════════
# INSTALL
# ════════════════════════════════════════════════════════════════════════════
cmd_install() {
    detect_os
    info "Repository root: $REPO_ROOT"

    # ── Artifacts ──
    step "Checking artifacts"
    [[ -x "$REPO_ROOT/build/liveqx" ]]        || die "Missing build/liveqx — build the binary (see --help)"
    [[ -x "$REPO_ROOT/build/liveqx-mountd" ]] || die "Missing build/liveqx-mountd — build the helper"
    [[ -d "$REPO_ROOT/ui/dist" ]]             || die "Missing ui/dist/ — build the UI: cd ui && npm ci && npm run build"
    [[ -f "$REPO_ROOT/config/default.json" ]] || die "Missing config/default.json"
    [[ -f "$REPO_ROOT/packaging/liveqx.service" ]] || die "Missing packaging/liveqx.service"
    ok "All artifacts present"

    # ── Apt dependencies ──
    # Runtime shared objects that liveqx + liveqx-mountd link against.
    # spdlog/fmt/cpp-httplib are built statically (their ABI between LTS releases is unstable).
    step "System packages"
    info "Refreshing apt index..."
    apt-get update -qq
    info "Installing runtime dependencies..."
    # libldap package name varies between distributions:
    #   Debian 12 / Ubuntu 22.04 → libldap-2.5-0
    #   Debian 13 / Ubuntu 24.04 → libldap-2.6-0
    # Look up which one is actually available in apt-cache — otherwise install fails.
    LIBLDAP_PKG=""
    for cand in libldap-2.5-0 libldap-2.6-0 libldap2; do
        if apt-cache show "$cand" >/dev/null 2>&1; then
            LIBLDAP_PKG="$cand"
            break
        fi
    done
    [[ -z "$LIBLDAP_PKG" ]] && die "No libldap package found (tried: libldap-2.5-0, libldap-2.6-0, libldap2)"
    info "libldap package: $LIBLDAP_PKG"

    apt-get install -y --no-install-recommends \
        ca-certificates curl python3 cifs-utils \
        libssl3 libsqlite3-0 libsodium23 libsrtp2-1 \
        "$LIBLDAP_PKG" libcurl4 libnss3 libgnutls30 \
        libx264-164 \
        libva2 libva-drm2 libdrm2 libmfx1 \
        || die "apt-get install failed. Check that the universe (Ubuntu) or non-free (Debian) repositories are enabled — libmfx1 and libx264-164 live there."
    ok "All runtime packages installed"

    # ── HW encoder prompts ──
    step "HW encoders (H.264/H.265 via GPU)"
    cat <<EOF
    LiveQX supports three hardware encoders. Mark the ones
    you plan to use. Drivers and SDKs are installed
    separately (manually) — for now the script only records the choice.

EOF

    USE_NVENC=0; USE_VAAPI=0; USE_QSV=0
    ask_yn "Will you use NVENC (NVIDIA GPU)?" "n" && USE_NVENC=1 || true
    ask_yn "Will you use VAAPI (Intel iGPU or AMD GPU)?" "n" && USE_VAAPI=1 || true
    ask_yn "Will you use QSV (Intel Quick Sync Video)?" "n" && USE_QSV=1 || true

    # ── Web UI + API port ──
    # Only ask on first install; otherwise pick the port from the existing config.
    API_PORT=8080
    if [[ -f "$CONF_FILE" ]]; then
        API_PORT=$(python3 -c "import json; print(json.load(open('$CONF_FILE')).get('api_port', 8080))" 2>/dev/null || echo 8080)
    elif [[ "$ASSUME_YES" != "1" ]]; then
        step "Web UI and API port"
        while true; do
            read -r -p "${C_BLD}?${C_RST} Port for Web UI and HTTP API [$API_PORT]: " ans || ans=""
            ans="${ans:-$API_PORT}"
            if [[ "$ans" =~ ^[0-9]+$ ]] && (( ans >= 1 && ans <= 65535 )); then
                API_PORT="$ans"
                (( API_PORT < 1024 )) && warn "Port <1024 requires CAP_NET_BIND_SERVICE in the unit — check after startup"
                break
            fi
            warn "Enter a number between 1 and 65535"
        done
    fi

    # ── User and group ──
    step "System user $LIVEQX_USER"
    if ! getent group "$LIVEQX_GROUP" >/dev/null; then
        groupadd --system "$LIVEQX_GROUP"
        ok "Created group $LIVEQX_GROUP"
    else
        ok "Group $LIVEQX_GROUP already exists"
    fi
    if ! getent passwd "$LIVEQX_USER" >/dev/null; then
        useradd --system --gid "$LIVEQX_GROUP" \
                --home-dir "$STATE_DIR" --no-create-home \
                --shell /usr/sbin/nologin \
                "$LIVEQX_USER"
        ok "Created user $LIVEQX_USER"
    else
        ok "User $LIVEQX_USER already exists"
    fi

    # ── Directories ──
    step "Directories and permissions"
    install -d -o "$LIVEQX_USER" -g "$LIVEQX_GROUP" -m 0750 "$STATE_DIR"
    install -d -o root           -g root           -m 0755 "$CONF_DIR"
    install -d -o root           -g root           -m 0755 "$UI_DIR"
    install -d -o root           -g root           -m 0755 "$DOC_DIR"
    install -d -o root           -g root           -m 0755 "$MOUNT_ROOT"
    ok "Directories created"

    # ── Binaries ──
    step "Binaries → $BIN_DIR"
    # Stop before copying: otherwise ETXTBSY on the running binary.
    if systemctl is-active --quiet liveqx 2>/dev/null; then
        info "Stopping running liveqx..."
        systemctl stop liveqx.service        || true
        systemctl stop liveqx-mountd.service || true
    fi
    install -m 0755 "$REPO_ROOT/build/liveqx"        "$BIN_DIR/liveqx"
    install -m 0755 "$REPO_ROOT/build/liveqx-mountd" "$BIN_DIR/liveqx-mountd"
    ok "Installed /usr/local/bin/liveqx and liveqx-mountd"

    # ldd check: without it a missing shared library gives an endless systemd
    # restart loop with no clear cause — better to fail here with an explicit list.
    local missing
    missing=$(ldd "$BIN_DIR/liveqx" 2>/dev/null | awk '/not found/ {print $1}' | sort -u || true)
    missing+=$'\n'$(ldd "$BIN_DIR/liveqx-mountd" 2>/dev/null | awk '/not found/ {print $1}' | sort -u || true)
    missing=$(echo "$missing" | grep -v '^$' | sort -u || true)
    if [[ -n "$missing" ]]; then
        err "Missing shared libraries — the binary will not start:"
        echo "$missing" | sed 's/^/      /'
        echo
        echo "  Look up packages for them:"
        echo "    apt-file search <library_name>"
        echo "  and install via apt-get install."
        exit 1
    fi
    ok "All shared libraries resolved (ldd clean)"

    # ── UI ──
    step "Web interface → $UI_DIR"
    rm -rf "${UI_DIR:?}"/*
    cp -r "$REPO_ROOT/ui/dist/." "$UI_DIR/"
    chown -R root:root "$UI_DIR"
    ok "UI installed"

    # ── Config ──
    step "Configuration"
    if [[ ! -f "$CONF_FILE" ]]; then
        install -m 0644 "$REPO_ROOT/config/default.json" "$CONF_FILE"
        API_PORT="$API_PORT" python3 - "$CONF_FILE" <<'PY'
import json, os, sys
p = sys.argv[1]
with open(p) as f: cfg = json.load(f)
cfg['ui_dir']       = '/usr/share/liveqx/ui'
cfg['state_dir']    = '/var/lib/liveqx/state'
cfg['log_dir']      = '/var/lib/liveqx/logs'
cfg['channels_dir'] = '/var/lib/liveqx/channels'
cfg['api_port']     = int(os.environ['API_PORT'])
with open(p, 'w') as f:
    json.dump(cfg, f, indent=2, ensure_ascii=False)
    f.write('\n')
PY
        ok "Created $CONF_FILE (API port $API_PORT)"
    else
        warn "Config $CONF_FILE already exists — leaving it alone (edit manually if needed)"
    fi

    # Documentation= in the units references these files.
    for d in SYSTEMD.md MOUNTS.md AUTH.md; do
        [[ -f "$REPO_ROOT/docs/$d" ]] && install -m 0644 "$REPO_ROOT/docs/$d" "$DOC_DIR/" || true
    done

    # ── Systemd units ──
    step "Systemd units"
    install -m 0644 "$REPO_ROOT/packaging/liveqx.service"            "$SYSTEMD_DIR/"
    install -m 0644 "$REPO_ROOT/packaging/liveqx-mountd.service"     "$SYSTEMD_DIR/"
    install -m 0644 "$REPO_ROOT/packaging/liveqx-mount-init.service" "$SYSTEMD_DIR/"
    systemctl daemon-reload
    ok "Units installed"

    # ── WSL2 ──
    if is_wsl; then
        warn "WSL2 detected — making the root mount shared (needed for CIFS propagation from mountd)"
        mount --make-rshared / || warn "mount --make-rshared / failed; the WSL kernel may not support it — check after startup"
    fi

    # ── Start ──
    step "Starting services"
    systemctl enable --now liveqx-mount-init.service
    systemctl enable --now liveqx-mountd.service
    systemctl enable --now liveqx.service

    info "Waiting for liveqx to be ready (up to 15s)..."
    local ready=0
    for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
        sleep 1
        if systemctl is-active --quiet liveqx; then ready=1; break; fi
    done
    if [[ "$ready" == "1" ]]; then
        ok "liveqx.service: active"
    else
        err "liveqx.service failed to start"
        echo "    Log:  sudo journalctl -u liveqx -n 80 --no-pager"
        exit 1
    fi

    # The API defaults to HTTPS (auto), but TLS may be disabled — try both
    # and remember which scheme responded to print the correct URL to the user.
    local api_port api_scheme=""
    api_port=$(python3 -c "import json; print(json.load(open('$CONF_FILE')).get('api_port', 8080))" 2>/dev/null || echo 8080)
    if curl -fsSk --max-time 3 "https://localhost:${api_port}/api/health" >/dev/null 2>&1; then
        api_scheme="https"
        ok "/api/health: OK (https, port $api_port)"
    elif curl -fsS --max-time 3 "http://localhost:${api_port}/api/health" >/dev/null 2>&1; then
        api_scheme="http"
        ok "/api/health: OK (http, port $api_port)"
    else
        # Our default config produces self-signed HTTPS — assume https.
        api_scheme="https"
        warn "/api/health is not responding yet (may be normal for the first startup)"
    fi

    # ── Final hints ──
    echo
    echo "${C_GRN}════════════════════════════════════════════════════════════════${C_RST}"
    echo "${C_GRN}  Installation complete${C_RST}"
    echo "${C_GRN}════════════════════════════════════════════════════════════════${C_RST}"
    local ip
    ip=$(hostname -I 2>/dev/null | awk '{print $1}')
    echo
    echo "  Web UI:        ${api_scheme}://${ip:-localhost}:${api_port}/"
    if [[ "$api_scheme" == "https" ]]; then
        echo "                 ${C_YEL}(self-signed certificate — the browser will show a warning on first visit)${C_RST}"
    fi
    echo "  Log:           sudo journalctl -u liveqx -f"
    echo "  Status:        systemctl status liveqx"
    echo "  Config:        $CONF_FILE"
    echo "  Data:          $STATE_DIR"
    echo "  Shares:        $MOUNT_ROOT/<id>/"

    # ── Bootstrap password for the first admin ──
    # The file (0600, owner=liveqx) is created by the service on first start
    # and auto-removed on the first password change.
    local pw_file="$STATE_DIR/state/initial_admin_password.txt"
    if [[ -s "$pw_file" ]]; then
        local pw
        pw=$(cat "$pw_file")
        echo
        echo "${C_BLD}── Administrator login ──${C_RST}"
        echo "  User:          admin"
        echo "  Password:      ${C_GRN}${pw}${C_RST}"
        echo "  ${C_YEL}Change it on first login — the file will be auto-removed:${C_RST}"
        echo "    $pw_file"
    fi

    # Recorded HW encoder choice (the user installs the drivers themselves)
    if [[ "$USE_NVENC" == "1" || "$USE_VAAPI" == "1" || "$USE_QSV" == "1" ]]; then
        echo
        echo "  ${C_BLD}Selected HW encoders:${C_RST}"
        [[ "$USE_NVENC" == "1" ]] && echo "    • NVENC"
        [[ "$USE_VAAPI" == "1" ]] && echo "    • VAAPI"
        [[ "$USE_QSV"   == "1" ]] && echo "    • QSV"
        echo "    Install the drivers and SDKs for them manually, then:"
        echo "      sudo systemctl restart liveqx"
    fi

    echo
    echo "${C_BLD}── Next steps ──${C_RST}"
    echo "  1. Open the Web UI and finish the initial setup"
    echo "  2. For production, configure a TPM2 credstore for master.key"
    echo "     (see comments in /etc/systemd/system/liveqx.service)"
    echo "  3. CIFS shares are added via UI → Settings → Shares"
    echo
}

# ════════════════════════════════════════════════════════════════════════════
# UNINSTALL
# ════════════════════════════════════════════════════════════════════════════
cmd_uninstall() {
    detect_os

    echo
    warn "EVERYTHING except apt packages will be removed:"
    echo "    • Services: liveqx, liveqx-mountd, liveqx-mount-init"
    echo "    • Binary:   $BIN_DIR/liveqx, $BIN_DIR/liveqx-mountd"
    echo "    • Config:   $CONF_DIR/  (including config.json)"
    echo "    • Data:     $STATE_DIR/ (DB, master.key, channel state)"
    echo "    • UI:       $UI_DIR/"
    echo "    • Docs:     $DOC_DIR/"
    echo "    • Units:    $SYSTEMD_DIR/liveqx*.service + dynamic mnt-liveqx-*.{mount,automount}"
    echo "    • Shares:   unmount $MOUNT_ROOT/* and remove the directory"
    echo "    • User:     $LIVEQX_USER, group $LIVEQX_GROUP"
    echo "    • Runtime:  $RUNTIME_DIR/"
    echo
    echo "    apt packages (cifs-utils, libsrtp2-1, nvidia-driver, ...) are NOT removed."
    echo

    if ! ask_yn "Really remove?" "n"; then
        info "Cancelled"; exit 0
    fi

    step "Stopping services"
    for u in liveqx liveqx-mountd liveqx-mount-init; do
        systemctl stop    "$u.service" 2>/dev/null || true
        systemctl disable "$u.service" 2>/dev/null || true
    done

    # Dynamic share units (created by mountd)
    info "Stopping dynamic mount units..."
    while IFS= read -r u; do
        [[ -z "$u" ]] && continue
        systemctl stop    "$u" 2>/dev/null || true
        systemctl disable "$u" 2>/dev/null || true
    done < <(systemctl list-unit-files --no-legend 'mnt-liveqx-*' 2>/dev/null | awk '{print $1}')

    step "Unmounting $MOUNT_ROOT"
    if mountpoint -q "$MOUNT_ROOT" 2>/dev/null; then
        # Unmount nested mountpoints first (if there are any CIFS mounts)
        awk -v root="$MOUNT_ROOT/" '$2 ~ "^"root {print $2}' /proc/mounts | tac | while read -r mp; do
            umount "$mp" 2>/dev/null || umount -l "$mp" 2>/dev/null || true
        done
        umount "$MOUNT_ROOT" 2>/dev/null || umount -l "$MOUNT_ROOT" 2>/dev/null || true
    fi

    step "Removing units"
    rm -f "$SYSTEMD_DIR/liveqx.service" \
          "$SYSTEMD_DIR/liveqx-mountd.service" \
          "$SYSTEMD_DIR/liveqx-mount-init.service"
    # Dynamic shares
    rm -f "$SYSTEMD_DIR"/mnt-liveqx-*.mount     2>/dev/null || true
    rm -f "$SYSTEMD_DIR"/mnt-liveqx-*.automount 2>/dev/null || true
    systemctl daemon-reload
    systemctl reset-failed 2>/dev/null || true

    step "Removing binaries"
    rm -f "$BIN_DIR/liveqx" "$BIN_DIR/liveqx-mountd"

    step "Removing data, configs, UI"
    rm -rf "$STATE_DIR" "$CONF_DIR" "$UI_DIR" "$DOC_DIR" "$RUNTIME_DIR"

    # /mnt/liveqx directory — empty after umount, remove it
    if [[ -d "$MOUNT_ROOT" ]]; then
        rmdir "$MOUNT_ROOT" 2>/dev/null || warn "$MOUNT_ROOT is not empty — not removed"
    fi
    # Parent /usr/share/liveqx if empty
    [[ -d /usr/share/liveqx ]] && rmdir /usr/share/liveqx 2>/dev/null || true

    step "Removing user and group"
    if getent passwd "$LIVEQX_USER" >/dev/null; then
        userdel "$LIVEQX_USER" 2>/dev/null || warn "userdel $LIVEQX_USER returned an error (there may be open processes)"
    fi
    if getent group "$LIVEQX_GROUP" >/dev/null; then
        groupdel "$LIVEQX_GROUP" 2>/dev/null || warn "groupdel $LIVEQX_GROUP returned an error"
    fi

    echo
    ok "Full removal complete"
    echo
    echo "  apt packages left installed. To remove them manually if desired:"
    echo "    sudo apt-get autoremove --purge cifs-utils libsrtp2-1"
    echo
}

# ════════════════════════════════════════════════════════════════════════════
# CHECK
# ════════════════════════════════════════════════════════════════════════════
cmd_check() {
    detect_os
    echo
    info "LiveQX installation state"

    step "User and group"
    getent passwd "$LIVEQX_USER" >/dev/null && ok "user $LIVEQX_USER" || err "user $LIVEQX_USER is missing"
    getent group  "$LIVEQX_GROUP" >/dev/null && ok "group $LIVEQX_GROUP" || err "group $LIVEQX_GROUP is missing"

    step "Files"
    [[ -x "$BIN_DIR/liveqx" ]]        && ok "$BIN_DIR/liveqx"        || err "missing $BIN_DIR/liveqx"
    [[ -x "$BIN_DIR/liveqx-mountd" ]] && ok "$BIN_DIR/liveqx-mountd" || err "missing $BIN_DIR/liveqx-mountd"
    [[ -f "$CONF_FILE" ]]             && ok "$CONF_FILE"             || err "missing $CONF_FILE"
    [[ -d "$STATE_DIR" ]]             && ok "$STATE_DIR"              || err "missing $STATE_DIR"
    if [[ -d "$UI_DIR" ]] && [[ -n "$(ls -A "$UI_DIR" 2>/dev/null)" ]]; then
        ok "$UI_DIR (has files)"
    else
        err "UI is empty or missing"
    fi

    step "Systemd units"
    for u in liveqx-mount-init liveqx-mountd liveqx; do
        if [[ -f "$SYSTEMD_DIR/$u.service" ]]; then
            local state
            state=$(systemctl is-active "$u.service" 2>/dev/null || echo inactive)
            if [[ "$state" == "active" ]]; then
                ok "$u.service: $state"
            else
                warn "$u.service: $state"
            fi
        else
            err "$u.service: unit not installed"
        fi
    done

    step "Mount propagation"
    if mountpoint -q "$MOUNT_ROOT" 2>/dev/null; then
        ok "$MOUNT_ROOT: bind-mount active"
    else
        warn "$MOUNT_ROOT: not a bind-mount (did liveqx-mount-init run?)"
    fi

    step "HTTP health"
    local api_port=8080
    [[ -f "$CONF_FILE" ]] && api_port=$(python3 -c "import json; print(json.load(open('$CONF_FILE')).get('api_port', 8080))" 2>/dev/null || echo 8080)
    if curl -fsSk --max-time 3 "https://localhost:${api_port}/api/health" >/dev/null 2>&1 \
       || curl -fsS  --max-time 3 "http://localhost:${api_port}/api/health"  >/dev/null 2>&1; then
        ok "/api/health responds on port $api_port"
    else
        err "/api/health does not respond on port $api_port"
    fi
    echo
}

# ── Dispatcher ──────────────────────────────────────────────────────────────
require_root
case "$MODE" in
    install)   cmd_install ;;
    uninstall) cmd_uninstall ;;
    check)     cmd_check ;;
    *)         die "Unknown mode: $MODE" ;;
esac
