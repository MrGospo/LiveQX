#!/usr/bin/env bash
#
# LiveQX Engine — installer for Ubuntu/Debian.
#
# Modes:
#   install    — install (default)
#   uninstall  — remove EVERYTHING (binaries, units, DB, configs, user)
#   check      — check current installation state
#   migrate-master-key — move an existing master key out of the state directory
#
# Usage: sudo ./packaging/install.sh [install|uninstall|check|migrate-master-key] [-y]
#        [--master-key-store=file|creds|passphrase] [--broker=local|none] [--broker-port=N]
#        [--allow-broker-version-mismatch]
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

# Master key (see packaging/liveqx.service for the three storage modes).
KEY_FILE="${CONF_DIR}/master.key"
CRED_NAME="liveqx_master_key"
CRED_DIR="/etc/credstore.encrypted"
CRED_FILE="${CRED_DIR}/${CRED_NAME}"
DROPIN_DIR="${SYSTEMD_DIR}/liveqx.service.d"
DROPIN_FILE="${DROPIN_DIR}/master-key.conf"

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"

# MQTT broker for LiveQX-Player screens (packaging/mosquitto/README.md): our own build of
# Mosquitto, started by liveqx-broker.path once the server has written the files it reads.
BROKER_MODE="ask"                       # ask | local | none
BROKER_PORT=8883
ALLOW_BROKER_MISMATCH=0
BROKER_BIN_DIR="/usr/local/libexec/liveqx"
BROKER_BIN="${BROKER_BIN_DIR}/mosquitto"     # the name matters: the server signals only "mosquitto"
BROKER_CONF="${CONF_DIR}/mosquitto.conf"
BROKER_STATE_DIR="${STATE_DIR}/state/broker"
BROKER_RUN_DIR="/run/liveqx-broker"
BROKER_BUILD_DIR="${REPO_ROOT}/build/mosquitto-patched"

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
    sudo $0 [install|uninstall|check|migrate-master-key] [options]

Commands:
    install     Install LiveQX (default)
    uninstall   Fully remove LiveQX (DB, configs, liveqx user)
    check       Check installation state
    migrate-master-key
                Move the master key of an existing installation out of the
                state directory into $KEY_FILE (same key, no re-encryption)

Options:
    -y, --yes       Assume yes for all prompts
    -h, --help      Show this help
    --master-key-store=file|creds|passphrase
                    Where a NEW installation keeps the master key (default: file)
                      file        $KEY_FILE (root:liveqx 0640), outside the state directory
                      creds       systemd credential, TPM2-bound when the host has a TPM
                                  (needs systemd 250 or newer)
                      passphrase  key wrapped with a passphrase; the service starts
                                  sealed and waits for the passphrase on the web page
    --broker=local|none
                    Install the MQTT broker for LiveQX-Player screens on this host (local), or
                    leave it out (none: an external broker, or no screens). Default: ask.
    --broker-port=N Port the broker listens on for screens (default $BROKER_PORT)
    --allow-broker-version-mismatch
                    Use the patched broker with a Dynamic Security plugin of another version
                    than the broker (untested; the installer refuses by default)

Before running install, make sure the following are built:
    build/liveqx               — main binary
    build/liveqx-mountd        — share-mount helper
    ui/dist/                   — Web UI
    build/mosquitto-patched/   — MQTT broker (packaging/mosquitto/build.sh), unless --broker=none

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
KEY_STORE="file"
while [[ $# -gt 0 ]]; do
    case "$1" in
        install|uninstall|check|migrate-master-key) MODE="$1"; shift ;;
        --master-key-store=*) KEY_STORE="${1#*=}"; shift ;;
        --broker=*) BROKER_MODE="${1#*=}"; shift ;;
        --broker-port=*) BROKER_PORT="${1#*=}"; shift ;;
        --allow-broker-version-mismatch) ALLOW_BROKER_MISMATCH=1; shift ;;
        -y|--yes)  ASSUME_YES=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) die "Unknown argument: $1 (see --help)" ;;
    esac
done

case "$KEY_STORE" in
    file|creds|passphrase) ;;
    *) die "--master-key-store must be file, creds or passphrase (got: $KEY_STORE)" ;;
esac
case "$BROKER_MODE" in
    ask|local|none) ;;
    *) die "--broker must be local or none (got: $BROKER_MODE)" ;;
esac
[[ "$BROKER_PORT" =~ ^[0-9]+$ ]] && (( BROKER_PORT >= 1 && BROKER_PORT <= 65535 )) \
    || die "--broker-port must be a number between 1 and 65535 (got: $BROKER_PORT)"

# ── Utilities ───────────────────────────────────────────────────────────────
# Sets string keys in the JSON config:  cfg_set key=value [key=value ...]
cfg_set() {
    python3 - "$CONF_FILE" "$@" <<'PY'
import json, sys
p = sys.argv[1]
with open(p) as f: cfg = json.load(f)
for kv in sys.argv[2:]:
    k, v = kv.split('=', 1)
    cfg[k] = v
with open(p, 'w') as f:
    json.dump(cfg, f, indent=2, ensure_ascii=False)
    f.write('\n')
PY
}

# Removes keys from the JSON config:  cfg_unset key [key ...]
cfg_unset() {
    python3 - "$CONF_FILE" "$@" <<'PY'
import json, sys
p = sys.argv[1]
with open(p) as f: cfg = json.load(f)
for k in sys.argv[2:]:
    cfg.pop(k, None)
with open(p, 'w') as f:
    json.dump(cfg, f, indent=2, ensure_ascii=False)
    f.write('\n')
PY
}

# Reads a config value (empty if absent):  cfg_get key
cfg_get() {
    python3 -c "import json,sys; print(json.load(open(sys.argv[1])).get(sys.argv[2], ''))" "$CONF_FILE" "$1" 2>/dev/null || true
}

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
# MASTER KEY
# ════════════════════════════════════════════════════════════════════════════
# Creates the master key for a NEW installation in the chosen store. An
# existing installation keeps whatever it has (changing it is
# `migrate-master-key`, never a side effect of a reinstall).
provision_master_key() {
    step "Master key ($KEY_STORE)"

    if [[ "$CONFIG_CREATED" != "1" ]]; then
        if [[ -f "$STATE_DIR/state/master.key" && -z "$(cfg_get master_key_path)" ]]; then
            warn "The master key sits in the state directory, next to the data it protects:"
            warn "  $STATE_DIR/state/master.key"
            warn "Any backup or snapshot of $STATE_DIR carries the key. Move it out with:"
            warn "  sudo $0 migrate-master-key"
        else
            ok "Existing installation: master key setup left as it is"
        fi
        return 0
    fi

    case "$KEY_STORE" in
        file|passphrase)
            if [[ -e "$KEY_FILE" ]]; then
                ok "Using the existing $KEY_FILE"
            else
                ( umask 077; head -c 32 /dev/urandom > "$KEY_FILE" )
                chown root:"$LIVEQX_GROUP" "$KEY_FILE"
                chmod 0640 "$KEY_FILE"
                ok "Created $KEY_FILE (root:$LIVEQX_GROUP 0640, outside the state directory)"
            fi
            cfg_set "master_key_path=$KEY_FILE" "master_key_store=file"
            if [[ "$KEY_STORE" == "passphrase" ]]; then
                [[ -t 0 ]] || die "--master-key-store=passphrase needs a terminal to ask for the passphrase"
                info "Choose a passphrase for the master key (at least 12 characters)."
                info "The service will start sealed and ask for it on the web page after every restart."
                "$BIN_DIR/liveqx" --config "$CONF_FILE" --protect-master-key \
                    || die "Could not protect the master key with a passphrase"
            fi
            ;;
        creds)
            command -v systemd-creds >/dev/null 2>&1 \
                || die "systemd-creds not found (needs systemd 250 or newer)"
            local with_key="host"
            if systemd-creds has-tpm2 >/dev/null 2>&1; then
                with_key="host+tpm2"
                info "TPM2 found: the credential will be bound to it"
            else
                warn "No TPM2: the credential is bound to this host's key stored under /var/lib/systemd."
                warn "That protects a copied /etc and a copied state directory, NOT a copy of the whole disk."
            fi
            install -d -m 0700 "$CRED_DIR"
            head -c 32 /dev/urandom \
                | systemd-creds encrypt --with-key="$with_key" --name="$CRED_NAME" - "$CRED_FILE" \
                || die "systemd-creds encrypt failed"
            chmod 0600 "$CRED_FILE"
            install -d -m 0755 "$DROPIN_DIR"
            cat > "$DROPIN_FILE" <<UNIT
[Service]
LoadCredentialEncrypted=${CRED_NAME}:${CRED_FILE}
Environment=LIVEQX_MASTER_KEY_FILE=%d/${CRED_NAME}
UNIT
            cfg_set "master_key_store=creds"
            ok "Master key stored as a systemd credential: $CRED_FILE"
            warn "The key exists nowhere else. To be able to restore on another host, keep an offline copy:"
            warn "  sudo systemd-creds decrypt $CRED_FILE - | base64    # store the output safely"
            ;;
    esac
}

# ════════════════════════════════════════════════════════════════════════════
# MQTT BROKER (for LiveQX-Player screens)
# ════════════════════════════════════════════════════════════════════════════
# The broker is our own build of Mosquitto (packaging/mosquitto/README.md says why the
# stock one is not safe). The distribution's package supplies only the Dynamic Security
# plugin and mosquitto_ctrl. The server writes everything the broker reads (CA file of
# the devices, its own certificate, the revocation list) after the device CA ceremony;
# liveqx-broker.path starts the broker when that is done.

decide_broker() {
    if [[ "$BROKER_MODE" == "ask" ]]; then
        if ask_yn "Install the MQTT broker for LiveQX-Player screens on this host?" "y"; then
            BROKER_MODE="local"
        else
            BROKER_MODE="none"
        fi
    fi
    [[ "$BROKER_MODE" == "local" ]] || info "No MQTT broker on this host (--broker=none)"
}

check_broker_artifacts() {
    [[ "$BROKER_MODE" == "local" ]] || return 0
    [[ -x "$BROKER_BUILD_DIR/sbin/mosquitto" ]] \
        || die "Missing build/mosquitto-patched/sbin/mosquitto — build the broker: packaging/mosquitto/build.sh (or use --broker=none)"
    local f
    for f in packaging/liveqx-broker.service packaging/liveqx-broker.path packaging/mosquitto/mosquitto.conf.in \
             packaging/mosquitto/README.md; do
        [[ -f "$REPO_ROOT/$f" ]] || die "Missing $f"
    done
}

# Stops the broker's units (a running binary cannot be replaced, and the path unit would start it again).
stop_broker_units() {
    systemctl stop liveqx-broker.path    2>/dev/null || true
    systemctl stop liveqx-broker.service 2>/dev/null || true
}

find_dynsec_plugin() {
    local p d
    p=$(dpkg -L mosquitto 2>/dev/null | grep -m1 'mosquitto_dynamic_security\.so$' || true)
    if [[ -n "$p" && -f "$p" ]]; then echo "$p"; return 0; fi
    for d in /usr/lib/x86_64-linux-gnu /usr/lib/aarch64-linux-gnu /usr/lib /usr/local/lib; do
        if [[ -f "$d/mosquitto_dynamic_security.so" ]]; then echo "$d/mosquitto_dynamic_security.so"; return 0; fi
    done
    return 1
}

# Adds what the server needs to talk to the broker to config.json; keeps whatever is there.
configure_broker_in_config() {
    BROKER_PORT="$BROKER_PORT" BROKER_RUN_DIR="$BROKER_RUN_DIR" python3 - "$CONF_FILE" <<'PY'
import json, os, sys
p = sys.argv[1]
with open(p) as f: cfg = json.load(f)
players = cfg.setdefault('players', {})
broker = players.setdefault('broker', {})
broker.setdefault('enabled', True)
broker.setdefault('host', '127.0.0.1')
broker.setdefault('port', int(os.environ['BROKER_PORT']))
crl = broker.setdefault('crl', {})
crl.setdefault('mode', 'local')
crl.setdefault('pid_file', os.environ['BROKER_RUN_DIR'] + '/mosquitto.pid')
with open(p, 'w') as f:
    json.dump(cfg, f, indent=2, ensure_ascii=False)
    f.write('\n')
PY
}

provision_broker() {
    [[ "$BROKER_MODE" == "local" ]] || return 0
    step "MQTT broker for screens"

    # Plugin and tools from the distribution. Its own service is not ours to run.
    local had_package=0
    dpkg -s mosquitto >/dev/null 2>&1 && had_package=1
    apt-get install -y --no-install-recommends mosquitto mosquitto-clients \
        || die "apt-get install mosquitto mosquitto-clients failed"
    if [[ "$had_package" == "0" ]]; then
        systemctl disable --now mosquitto.service >/dev/null 2>&1 || true
        info "The distribution's own mosquitto.service (started by the package) is stopped and disabled: LiveQX runs its own broker"
    fi
    local plugin
    plugin=$(find_dynsec_plugin) || die "The Dynamic Security plugin (mosquitto_dynamic_security.so) was not found after installing mosquitto"
    command -v mosquitto_ctrl >/dev/null 2>&1 || die "mosquitto_ctrl not found after installing mosquitto-clients"

    # The plugin is the distribution's, the broker is ours: they must be the same release.
    local distro_ver built_ver
    distro_ver=$(dpkg-query -W -f='${Version}' mosquitto 2>/dev/null | sed -E 's/^[0-9]+://; s/-.*$//')
    built_ver=$(cat "$BROKER_BUILD_DIR/VERSION" 2>/dev/null || echo unknown)
    if [[ "$distro_ver" != "$built_ver" ]]; then
        if [[ "$ALLOW_BROKER_MISMATCH" == "1" ]]; then
            warn "Broker $built_ver with the plugin of Mosquitto $distro_ver (--allow-broker-version-mismatch): untested"
        else
            die "The broker built here is Mosquitto $built_ver, this distribution's plugin is $distro_ver. Rebuild the broker for $distro_ver (packaging/mosquitto/build.sh needs its checksum) or pass --allow-broker-version-mismatch"
        fi
    fi
    ok "Mosquitto $built_ver, plugin $plugin"

    # The broker binary, under the name the server signals.
    stop_broker_units
    install -d -o root -g root -m 0755 "$BROKER_BIN_DIR"
    install -m 0755 "$BROKER_BUILD_DIR/sbin/mosquitto" "$BROKER_BIN"
    local missing
    missing=$(ldd "$BROKER_BIN" 2>/dev/null | awk '/not found/ {print $1}' | sort -u || true)
    if [[ -n "$missing" ]]; then
        err "Missing shared libraries for the broker:"; echo "$missing" | sed 's/^/      /'
        die "Install them with apt-get and run the installer again"
    fi
    ok "Broker installed: $BROKER_BIN"

    # Directories: the server writes the broker's files here, the broker keeps its rights and sessions here.
    install -d -o "$LIVEQX_USER" -g "$LIVEQX_GROUP" -m 0750 "$STATE_DIR/state" "$BROKER_STATE_DIR" "$BROKER_STATE_DIR/data"

    # Configuration from the template. An existing file is the administrator's: left alone.
    local tmp
    tmp=$(mktemp)
    sed -e "s|@BROKER_DIR@|${BROKER_STATE_DIR}|g" -e "s|@RUN_DIR@|${BROKER_RUN_DIR}|g" \
        -e "s|@PORT@|${BROKER_PORT}|g" -e "s|@PLUGIN@|${plugin}|g" \
        "$REPO_ROOT/packaging/mosquitto/mosquitto.conf.in" > "$tmp"
    grep -q '@[A-Z_]*@' "$tmp" && { rm -f "$tmp"; die "A place in packaging/mosquitto/mosquitto.conf.in was not filled in"; }
    if [[ ! -f "$BROKER_CONF" ]]; then
        install -m 0644 "$tmp" "$BROKER_CONF"
        ok "Created $BROKER_CONF (port $BROKER_PORT)"
    elif cmp -s "$tmp" "$BROKER_CONF"; then
        ok "$BROKER_CONF is up to date"
    else
        install -m 0644 "$tmp" "$BROKER_CONF.dist"
        warn "$BROKER_CONF exists and differs from the current template — left as it is; the new one is $BROKER_CONF.dist"
    fi
    rm -f "$tmp"

    # The broker's administrator is the server's own certificate identity; the password
    # asked for here is never used and never kept. Never initialised twice: that would drop every right.
    local dyn="$BROKER_STATE_DIR/dynamic-security.json"
    if [[ -f "$dyn" ]]; then
        ok "Dynamic Security: keeping $dyn"
    else
        local pw
        pw=$(head -c 24 /dev/urandom | base64 | tr -d '/+=\n')
        printf '%s\n%s\n' "$pw" "$pw" \
            | runuser -u "$LIVEQX_USER" -- mosquitto_ctrl dynsec init "$dyn" liveqx-server >/dev/null 2>&1 \
            || die "mosquitto_ctrl dynsec init failed"
        chmod 0600 "$dyn"
        chown "$LIVEQX_USER:$LIVEQX_GROUP" "$dyn"
        ok "Dynamic Security initialised: $dyn"
    fi

    # The server connects to the broker and writes its files.
    configure_broker_in_config
    ok "players.broker set in $CONF_FILE (anything already there was kept)"

    install -m 0644 "$REPO_ROOT/packaging/mosquitto/README.md" "$DOC_DIR/BROKER.md"
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
    decide_broker
    check_broker_artifacts

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
    if [[ "$BROKER_MODE" == "local" ]]; then stop_broker_units; fi
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
    CONFIG_CREATED=0
    if [[ ! -f "$CONF_FILE" ]]; then
        CONFIG_CREATED=1
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

    # ── Master key ──
    provision_master_key

    # ── MQTT broker ──
    provision_broker

    # ── Systemd units ──
    step "Systemd units"
    install -m 0644 "$REPO_ROOT/packaging/liveqx.service"            "$SYSTEMD_DIR/"
    install -m 0644 "$REPO_ROOT/packaging/liveqx-mountd.service"     "$SYSTEMD_DIR/"
    install -m 0644 "$REPO_ROOT/packaging/liveqx-mount-init.service" "$SYSTEMD_DIR/"
    if [[ "$BROKER_MODE" == "local" ]]; then
        install -m 0644 "$REPO_ROOT/packaging/liveqx-broker.service" "$SYSTEMD_DIR/"
        install -m 0644 "$REPO_ROOT/packaging/liveqx-broker.path"    "$SYSTEMD_DIR/"
    fi
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

    # The broker waits for the server's files (liveqx-broker.path), so it can be enabled at once.
    if [[ "$BROKER_MODE" == "local" ]]; then
        systemctl enable --now liveqx-broker.path
        if [[ -e "$BROKER_STATE_DIR/ready" ]]; then
            ok "liveqx-broker.path: the server's files are in place, the broker is starting"
        else
            info "The broker will start by itself when the server has written its files (after the device CA ceremony)"
        fi
    fi

    # A passphrase-protected key makes the service start sealed: it is alive
    # but serves only the unlock page until the passphrase is entered.
    local sealed=0
    if [[ "$KEY_STORE" == "passphrase" && "$CONFIG_CREATED" == "1" ]]; then
        sealed=1
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

    if [[ "$sealed" == "1" ]]; then
        echo
        echo "${C_BLD}── The service is sealed ──${C_RST}"
        echo "  The master key is protected by your passphrase. Open the address above and"
        echo "  enter it; LiveQX starts after that and the administrator password appears"
        echo "  in $STATE_DIR/state/initial_admin_password.txt."
        echo "  Or, on this server without the network:"
        echo "    sudo liveqx --config $CONF_FILE --unseal"
        echo "  This repeats after every restart of the service."
    fi

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
    echo "  2. Back up the master key SEPARATELY from the data (a copy of the data"
    echo "     without the key is unreadable, and a key next to the data protects nothing):"
    case "$KEY_STORE" in
        file|passphrase) echo "       $KEY_FILE" ;;
        creds)           echo "       sudo systemd-creds decrypt $CRED_FILE - | base64" ;;
    esac
    echo "  3. CIFS shares are added via UI → Settings → Shares"
    if [[ "$BROKER_MODE" == "local" ]]; then
        echo "  4. Screens (LiveQX-Player): do the device CA ceremony, then set the three addresses"
        echo "     in Screens → Settings. The broker listens on port ${BROKER_PORT} (open it in the firewall)"
        echo "     and starts by itself once the server has written its files. See $DOC_DIR/BROKER.md"
        echo "     To stop it by hand, stop both: systemctl stop liveqx-broker.path liveqx-broker.service"
    fi
    echo
}

# ════════════════════════════════════════════════════════════════════════════
# UNINSTALL
# ════════════════════════════════════════════════════════════════════════════
cmd_uninstall() {
    detect_os

    echo
    warn "EVERYTHING except apt packages will be removed:"
    echo "    • Services: liveqx, liveqx-mountd, liveqx-mount-init, liveqx-broker (+ .path)"
    echo "    • Binary:   $BIN_DIR/liveqx, $BIN_DIR/liveqx-mountd, $BROKER_BIN_DIR/ (the MQTT broker)"
    echo "    • Config:   $CONF_DIR/  (including config.json and master.key)"
    echo "    • Key:      $CRED_FILE and the unit drop-in, if the key is a systemd credential"
    echo "    • Data:     $STATE_DIR/ (DB, master.key, channel state)"
    echo "    • UI:       $UI_DIR/"
    echo "    • Docs:     $DOC_DIR/"
    echo "    • Units:    $SYSTEMD_DIR/liveqx*.service + dynamic mnt-liveqx-*.{mount,automount}"
    echo "    • Shares:   unmount $MOUNT_ROOT/* and remove the directory"
    echo "    • User:     $LIVEQX_USER, group $LIVEQX_GROUP"
    echo "    • Runtime:  $RUNTIME_DIR/"
    echo
    echo "    apt packages (cifs-utils, libsrtp2-1, mosquitto, nvidia-driver, ...) are NOT removed."
    echo

    if ! ask_yn "Really remove?" "n"; then
        info "Cancelled"; exit 0
    fi

    step "Stopping services"
    # The path unit first: it would start the broker again.
    systemctl stop    liveqx-broker.path 2>/dev/null || true
    systemctl disable liveqx-broker.path 2>/dev/null || true
    for u in liveqx liveqx-broker liveqx-mountd liveqx-mount-init; do
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
          "$SYSTEMD_DIR/liveqx-mount-init.service" \
          "$SYSTEMD_DIR/liveqx-broker.service" \
          "$SYSTEMD_DIR/liveqx-broker.path"
    # Dynamic shares
    rm -f "$SYSTEMD_DIR"/mnt-liveqx-*.mount     2>/dev/null || true
    rm -f "$SYSTEMD_DIR"/mnt-liveqx-*.automount 2>/dev/null || true
    systemctl daemon-reload
    systemctl reset-failed 2>/dev/null || true

    step "Removing binaries"
    rm -f "$BIN_DIR/liveqx" "$BIN_DIR/liveqx-mountd"
    rm -rf "$BROKER_BIN_DIR"

    step "Removing data, configs, UI"
    rm -rf "$STATE_DIR" "$CONF_DIR" "$UI_DIR" "$DOC_DIR" "$RUNTIME_DIR"
    # Master key delivered as a systemd credential, and its unit drop-in.
    rm -f  "$CRED_FILE"
    rm -rf "$DROPIN_DIR"
    systemctl daemon-reload

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

    step "MQTT broker"
    if [[ -f "$SYSTEMD_DIR/liveqx-broker.path" ]]; then
        [[ -x "$BROKER_BIN" ]]   && ok "$BROKER_BIN"   || err "missing $BROKER_BIN"
        [[ -f "$BROKER_CONF" ]]  && ok "$BROKER_CONF"  || err "missing $BROKER_CONF"
        [[ -f "$BROKER_STATE_DIR/dynamic-security.json" ]] && ok "dynamic-security.json" || err "missing $BROKER_STATE_DIR/dynamic-security.json"
        local path_state broker_state
        path_state=$(systemctl is-active liveqx-broker.path 2>/dev/null || echo inactive)
        broker_state=$(systemctl is-active liveqx-broker.service 2>/dev/null || echo inactive)
        [[ "$path_state" == "active" ]] && ok "liveqx-broker.path: $path_state" || warn "liveqx-broker.path: $path_state"
        if [[ "$broker_state" == "active" ]]; then
            ok "liveqx-broker.service: $broker_state"
        elif [[ -e "$BROKER_STATE_DIR/ready" ]]; then
            err "liveqx-broker.service: $broker_state although the server's files are in place — journalctl -u liveqx-broker -n 50"
        else
            info "liveqx-broker.service: $broker_state — waiting for the server's files (after the device CA ceremony)"
        fi
    else
        info "No MQTT broker installed on this host"
    fi

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

# ════════════════════════════════════════════════════════════════════════════
# MIGRATE MASTER KEY
# ════════════════════════════════════════════════════════════════════════════
# Moves the key of an existing installation out of the state directory. The
# SAME 32 bytes are stored in the new place, so nothing in the databases is
# re-encrypted. The old file is removed only after the service has started
# with the new location.
cmd_migrate_master_key() {
    [[ -f "$CONF_FILE" ]] || die "No $CONF_FILE — nothing to migrate"
    [[ -z "$(cfg_get master_key_path)" ]] || die "master_key_path is already set in $CONF_FILE — already migrated?"
    [[ "$(cfg_get master_key_store)" != "creds" ]] || die "The key is a systemd credential already"

    local state_dir old
    state_dir=$(cfg_get state_dir)
    state_dir="${state_dir:-$STATE_DIR/state}"
    old="$state_dir/master.key"
    [[ -f "$old" ]] || die "No $old — the key is not a file in the state directory (nothing to migrate)"
    [[ "$(head -c 7 "$old")" != "LQXKEY1" ]] || info "The key is passphrase-protected; it is moved as it is"

    if [[ -e "$KEY_FILE" ]]; then
        cmp -s "$old" "$KEY_FILE" || die "$KEY_FILE already exists and differs from $old — resolve by hand"
    fi

    step "Moving the master key to $KEY_FILE"
    systemctl stop liveqx.service 2>/dev/null || true
    if [[ ! -e "$KEY_FILE" ]]; then
        ( umask 077; cp --no-preserve=all "$old" "$KEY_FILE" )
        chown root:"$LIVEQX_GROUP" "$KEY_FILE"
        chmod 0640 "$KEY_FILE"
    fi
    cmp -s "$old" "$KEY_FILE" || die "Copy of the key does not match the original; nothing was changed"
    cfg_set "master_key_path=$KEY_FILE"
    ok "Config now points at $KEY_FILE"

    systemctl start liveqx.service || true
    local up=0
    for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
        sleep 1
        if systemctl is-active --quiet liveqx; then up=1; break; fi
    done
    if [[ "$up" != "1" ]]; then
        err "liveqx did not start with the key in the new place — rolling back"
        cfg_unset master_key_path
        systemctl restart liveqx.service || true
        die "Rolled back; the key is still in $old. Look at: sudo journalctl -u liveqx -n 80 --no-pager"
    fi
    ok "liveqx.service is active with the key in $KEY_FILE"

    shred -u "$old" 2>/dev/null || rm -f "$old"
    ok "Removed $old"

    echo
    warn "Older backups and snapshots of $state_dir still contain the OLD copy of the key."
    warn "To make them useless, rotate the key now (stop the service first):"
    warn "  sudo systemctl stop liveqx && sudo liveqx --config $CONF_FILE --rotate-master-key && sudo systemctl start liveqx"
    warn "Then back up $KEY_FILE separately from the data."
}

# ── Dispatcher ──────────────────────────────────────────────────────────────
require_root
case "$MODE" in
    migrate-master-key) cmd_migrate_master_key ;;
    install)   cmd_install ;;
    uninstall) cmd_uninstall ;;
    check)     cmd_check ;;
    *)         die "Unknown mode: $MODE" ;;
esac
