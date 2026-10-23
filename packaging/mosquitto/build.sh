#!/usr/bin/env bash
# Builds the Mosquitto broker that LiveQX needs: the upstream release plus the
# patches in ./patches (see README.md for why the stock broker is not enough).
#
#   packaging/mosquitto/build.sh [PREFIX]
#
# PREFIX defaults to <repo>/build/mosquitto-patched. The broker binary lands in
# PREFIX/sbin/mosquitto. The Dynamic Security plugin and mosquitto_ctrl are not
# rebuilt: take them from the distribution's mosquitto package of the SAME
# version (the patches touch only the broker core).
#
# Needs: curl, tar, patch, make, a C compiler, libssl-dev.
set -euo pipefail

VERSION="2.0.18"
SHA256="d665fe7d0032881b1371a47f34169ee4edab67903b2cd2b4c083822823f4448a"
URL="https://mosquitto.org/files/source/mosquitto-${VERSION}.tar.gz"

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
prefix="${1:-$repo/build/mosquitto-patched}"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

echo "fetching mosquitto ${VERSION}"
curl -fsSL -o "$work/src.tar.gz" "$URL"
echo "${SHA256}  $work/src.tar.gz" | sha256sum -c - >/dev/null \
    || { echo "checksum of the downloaded source does not match; refusing to build" >&2; exit 1; }

tar xzf "$work/src.tar.gz" -C "$work"
cd "$work/mosquitto-${VERSION}"

for p in "$here"/patches/*.patch; do
    echo "applying $(basename "$p")"
    patch -p1 --quiet < "$p"
done

echo "building"
make -j2 binary WITH_DOCS=no WITH_CJSON=no WITH_WEBSOCKETS=no WITH_SRV=no WITH_TLS=yes \
     WITH_APPS=no WITH_PLUGINS=no >"$work/build.log" 2>&1 \
    || { tail -30 "$work/build.log" >&2; exit 1; }

install -D -m 0755 src/mosquitto "$prefix/sbin/mosquitto"
echo "installed $prefix/sbin/mosquitto"
