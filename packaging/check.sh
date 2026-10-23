#!/usr/bin/env bash
# Static checks of the installation files that nothing else runs until the installer
# does as root: they catch a typo, or two files that stopped agreeing with each other
# (the unit, the installer, the template, the server). Run by ctest as "packaging-static".
set -uo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
fail=0
bad() { echo "FAIL: $*" >&2; fail=1; }

# Syntax.
for f in install.sh mosquitto/build.sh; do
    bash -n "$here/$f" || bad "$f does not parse"
done

# The unit files, as systemd reads them. The one complaint expected is that the broker
# binary is not installed on the machine that runs this check.
if command -v systemd-analyze >/dev/null 2>&1; then
    out="$(systemd-analyze verify --man=no "$here/liveqx-broker.service" "$here/liveqx-broker.path" 2>&1 \
           | grep -v 'is not executable' || true)"
    [[ -z "$out" ]] || bad "systemd-analyze verify: $out"
else
    echo "note: systemd-analyze not found, the units were not verified"
fi

# Every place in the template is filled in by the installer, and the installer fills in no other.
tmpl="$here/mosquitto/mosquitto.conf.in"
want="$(grep -o '@[A-Z_]*@' "$tmpl" | sort -u | tr -d '@' | tr '\n' ' ')"
for w in $want; do
    grep -q "s|@${w}@|" "$here/install.sh" || bad "install.sh does not fill in @${w}@ of the template"
done
for w in BROKER_DIR RUN_DIR PORT PLUGIN; do
    grep -q "@${w}@" "$tmpl" || bad "the template has no @${w}@ although the installer fills it in"
done

# The files the server writes, the directory the unit lets the broker write to, and the file the path unit waits for.
grep -q '^PathExists=/var/lib/liveqx/state/broker/ready$' "$here/liveqx-broker.path" \
    || bad "liveqx-broker.path does not wait for /var/lib/liveqx/state/broker/ready"
grep -q '^ReadWritePaths=/var/lib/liveqx/state/broker$' "$here/liveqx-broker.service" \
    || bad "liveqx-broker.service does not give the broker /var/lib/liveqx/state/broker"
grep -q 'BROKER_STATE_DIR="${STATE_DIR}/state/broker"' "$here/install.sh" \
    || bad "install.sh keeps the broker somewhere else than /var/lib/liveqx/state/broker"
grep -q 'brokerDir() const .*"broker"' "$here/../src/utils/Paths.h" \
    || bad "the server no longer keeps the broker's files in <state>/broker"
grep -q 'brokerReadyFile() const .*"ready"' "$here/../src/utils/Paths.h" \
    || bad "the server no longer calls the file the broker waits for 'ready'"

# The server signals the broker through the pid file the template names, and only a process called mosquitto.
grep -q '^pid_file @RUN_DIR@/mosquitto.pid$' "$tmpl" || bad "the template's pid_file is not @RUN_DIR@/mosquitto.pid"
grep -q 'BROKER_RUN_DIR="/run/liveqx-broker"' "$here/install.sh" || bad "install.sh: the run directory is not /run/liveqx-broker"
grep -q '^RuntimeDirectory=liveqx-broker$' "$here/liveqx-broker.service" || bad "the unit's RuntimeDirectory is not liveqx-broker"
grep -q 'BROKER_BIN="${BROKER_BIN_DIR}/mosquitto"' "$here/install.sh" || bad "install.sh installs the broker under another name than mosquitto"
grep -q '^ExecStart=/usr/local/libexec/liveqx/mosquitto ' "$here/liveqx-broker.service" || bad "the unit starts another file than the installed broker"

# The documentation the units point to is installed.
grep -q 'DOC_DIR/BROKER.md' "$here/install.sh" || bad "install.sh does not install BROKER.md"

# The checksum build.sh trusts is the one of the version it builds.
grep -q '^VERSION="2.0.18"$' "$here/mosquitto/build.sh" || bad "build.sh: unexpected version"

[[ "$fail" == "0" ]] && echo "packaging checks passed"
exit "$fail"
