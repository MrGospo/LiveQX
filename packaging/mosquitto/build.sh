#!/usr/bin/env bash
# Builds the MQTT broker LiveQX needs: upstream Mosquitto, unmodified, together with its Dynamic
# Security plugin and `mosquitto_ctrl` from the SAME source (see README.md for why the distribution's
# package is not used).
#
#   packaging/mosquitto/build.sh [PREFIX]
#
# PREFIX defaults to <repo>/build/mosquitto. Everything lands in one directory, PREFIX/libexec/:
#
#   mosquitto                       the broker (the name matters: the server signals only "mosquitto")
#   mosquitto_dynamic_security.so   its Dynamic Security plugin
#   mosquitto_ctrl                  administration tool (used once, by the installer)
#   libmosquitto.so.1               what mosquitto_ctrl links against; found next to it (rpath $ORIGIN)
#
# Needs: curl, tar, make, a C compiler, libssl-dev, libcjson-dev. At run time the plugin and
# mosquitto_ctrl need libcjson1 and libssl.
set -euo pipefail

VERSION="2.0.23"
SHA256="47d66fc98d9f8714bdc700dfe690dbe80c9fbe93c00a21b04dc61d00e1667b34"
URL="https://mosquitto.org/files/source/mosquitto-${VERSION}.tar.gz"

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
prefix="${1:-$repo/build/mosquitto}"

# The Dynamic Security plugin and mosquitto_ctrl are written against cJSON.
if ! echo '#include <cjson/cJSON.h>' | ${CC:-cc} -E -x c - >/dev/null 2>&1; then
    echo "the cJSON headers are missing: install libcjson-dev" >&2
    exit 1
fi

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

echo "fetching mosquitto ${VERSION}"
curl -fsSL -o "$work/src.tar.gz" "$URL"
echo "${SHA256}  $work/src.tar.gz" | sha256sum -c - >/dev/null \
    || { echo "checksum of the downloaded source does not match; refusing to build" >&2; exit 1; }

tar xzf "$work/src.tar.gz" -C "$work"
cd "$work/mosquitto-${VERSION}"

echo "building"
# The libraries a binary needs are looked up next to it: \$$ reaches the linker as a literal $.
make -j2 binary WITH_DOCS=no WITH_CLIENTS=no WITH_APPS=yes WITH_PLUGINS=yes WITH_CJSON=yes \
     WITH_WEBSOCKETS=no WITH_SRV=no WITH_TLS=yes WITH_STATIC_LIBRARIES=no \
     LDFLAGS='-Wl,-rpath,\$$ORIGIN' >"$work/build.log" 2>&1 \
    || { tail -30 "$work/build.log" >&2; exit 1; }

dest="$prefix/libexec"
mkdir -p "$dest"
install -m 0755 src/mosquitto                                          "$dest/mosquitto"
install -m 0755 plugins/dynamic-security/mosquitto_dynamic_security.so "$dest/mosquitto_dynamic_security.so"
install -m 0755 apps/mosquitto_ctrl/mosquitto_ctrl                     "$dest/mosquitto_ctrl"
install -m 0755 lib/libmosquitto.so.1                                  "$dest/libmosquitto.so.1"
echo "${VERSION}" > "$prefix/VERSION"
echo "installed $dest (${VERSION})"
