#!/usr/bin/env bash
set -euo pipefail

# Builds a static FFmpeg with the network protocols, muxers and demuxers
# the liveqx engine actually uses in production:
#   • RTMP / RTMPS (fix11 — live ingest + push to CDN)
#   • UDP / RTP / SRTP (fix10 — multicast input + RTP fallback)
#   • HTTP / HTTPS / HLS (fix14 — playlist-based output, REST clients)
#   • RTSP (fix15 — IP-camera ingest)
#   • TLS via OpenSSL (rtmps://, https://, srtp)
#
# Output: third_party/ffmpeg/{include,lib,lib/pkgconfig}
#
# Usage: bash scripts/build_ffmpeg.sh [--force]
# --force triggers a full distclean before reconfigure (use after editing
# this script's flag list to make sure stale build state doesn't leak in).

FFMPEG_VERSION="7.1.1"
FFMPEG_URL="https://ffmpeg.org/releases/ffmpeg-${FFMPEG_VERSION}.tar.xz"

FORCE=0
for arg in "$@"; do
    case "$arg" in
        --force) FORCE=1 ;;
        *) echo "Unknown option: $arg" >&2; exit 2 ;;
    esac
done

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${ROOT_DIR}/third_party/ffmpeg-build"
INSTALL_DIR="${ROOT_DIR}/third_party/ffmpeg"

echo "=== FFmpeg ${FFMPEG_VERSION} static build (with network) ==="
echo "    install → ${INSTALL_DIR}"

# ── Verify build dependencies ────────────────────────────────────────────────
need_pkg() {
    pkg-config --exists "$1" || { echo "ERROR: pkg-config can't find '$1' — install the -dev package" >&2; exit 1; }
}
need_pkg openssl
echo "    openssl: $(pkg-config --modversion openssl)"

# x264 must be available; the existing config already required it.
need_pkg x264 || true   # libx264 may register as 'x264' or via -lx264 only

# ── fix29: optional GPU encoder support (env-driven, OFF by default) ────────
# Enabling these requires the corresponding system packages:
#   ENABLE_NVENC=1  needs nv-codec-headers (https://github.com/FFmpeg/nv-codec-headers)
#   ENABLE_QSV=1    needs libmfx-dev (Intel Media SDK)
#   ENABLE_VAAPI=1  needs libva-dev + libdrm-dev
ENABLE_NVENC="${ENABLE_NVENC:-0}"
ENABLE_QSV="${ENABLE_QSV:-0}"
ENABLE_VAAPI="${ENABLE_VAAPI:-0}"

GPU_CONFIGURE_FLAGS=()
GPU_ENCODERS=()
if [[ "${ENABLE_NVENC}" == "1" ]]; then
    pkg-config --exists ffnvcodec || \
        { echo "ERROR: ENABLE_NVENC=1 but pkg-config can't find ffnvcodec (install nv-codec-headers)" >&2; exit 1; }
    GPU_CONFIGURE_FLAGS+=(--enable-nvenc)
    GPU_ENCODERS+=(h264_nvenc hevc_nvenc)
    echo "    nvenc:   ENABLED (ffnvcodec $(pkg-config --modversion ffnvcodec))"
fi
if [[ "${ENABLE_QSV}" == "1" ]]; then
    pkg-config --exists libmfx || pkg-config --exists mfx || \
        { echo "ERROR: ENABLE_QSV=1 but pkg-config can't find libmfx (install libmfx-dev)" >&2; exit 1; }
    GPU_CONFIGURE_FLAGS+=(--enable-libmfx)
    GPU_ENCODERS+=(h264_qsv hevc_qsv)
    echo "    qsv:     ENABLED"
fi
if [[ "${ENABLE_VAAPI}" == "1" ]]; then
    pkg-config --exists libva || \
        { echo "ERROR: ENABLE_VAAPI=1 but pkg-config can't find libva (install libva-dev)" >&2; exit 1; }
    GPU_CONFIGURE_FLAGS+=(--enable-vaapi)
    GPU_ENCODERS+=(h264_vaapi hevc_vaapi)
    echo "    vaapi:   ENABLED"
fi
if [[ ${#GPU_ENCODERS[@]} -gt 0 ]]; then
    # Append GPU encoders to the existing encoder list. Configure accepts
    # repeated --enable-encoder=NAME, which is more robust than rewriting
    # the comma-separated list when extending.
    for enc in "${GPU_ENCODERS[@]}"; do
        GPU_CONFIGURE_FLAGS+=("--enable-encoder=${enc}")
    done
fi

# ── Download ──────────────────────────────────────────────────────────────────
mkdir -p "${BUILD_DIR}"
TARBALL="${BUILD_DIR}/ffmpeg-${FFMPEG_VERSION}.tar.xz"

if [[ ! -f "${TARBALL}" ]]; then
    echo "[1/4] Downloading FFmpeg ${FFMPEG_VERSION}…"
    curl -L "${FFMPEG_URL}" -o "${TARBALL}"
else
    echo "[1/4] Tarball already downloaded, skipping."
fi

# ── Extract ───────────────────────────────────────────────────────────────────
SRC_DIR="${BUILD_DIR}/ffmpeg-${FFMPEG_VERSION}"
if [[ ! -d "${SRC_DIR}" ]]; then
    echo "[2/4] Extracting…"
    tar -xf "${TARBALL}" -C "${BUILD_DIR}"
else
    echo "[2/4] Source already extracted, skipping."
fi

# ── Configure ─────────────────────────────────────────────────────────────────
echo "[3/4] Configuring…"
cd "${SRC_DIR}"

# Wipe stale install + intermediate state when flags change. Without this,
# new --enable-protocol entries can be ignored because the configure cache
# remembers the old answer.
if [[ "${FORCE}" == "1" ]] || [[ -f "${SRC_DIR}/ffbuild/config.mak" ]]; then
    echo "    cleaning previous build state…"
    make distclean >/dev/null 2>&1 || true
fi
rm -rf "${INSTALL_DIR}"

./configure \
    --prefix="${INSTALL_DIR}" \
    \
    --disable-shared \
    --enable-static \
    --enable-pic \
    \
    --disable-programs \
    --disable-doc \
    --disable-htmlpages \
    --disable-manpages \
    --disable-podpages \
    --disable-txtpages \
    \
    --disable-everything \
    \
    --enable-decoder=h264,hevc,aac,mp3,ac3,pcm_s16le,pcm_s16be,pcm_f32le,pcm_f32be,mjpeg,png,bmp,tiff,gif,rawvideo,ppm,pgm \
    --enable-encoder=libx264,mpeg2video,aac,mp2 \
    --enable-muxer=mpegts,mp4,null,flv,hls,rtp,rtp_mpegts \
    --enable-demuxer=mov,mp4,matroska,avi,flv,image2,mjpeg,mpegts,yuv4mpegpipe,hls,rtsp,sdp,live_flv \
    --enable-parser=h264,hevc,aac,mpegaudio,png \
    --enable-protocol=file,pipe,tcp,udp,rtp,rtmp,rtmps,rtmpt,rtmpts,http,https,tls,hls,rtsp,srtp,crypto,data,async \
    --enable-bsf=aac_adtstoasc,h264_mp4toannexb,hevc_mp4toannexb,extract_extradata \
    --enable-filter=scale,aresample,anull,null \
    \
    --enable-libx264 \
    --enable-gpl \
    --enable-version3 \
    \
    --enable-network \
    --enable-openssl \
    \
    --disable-libsrt \
    --disable-librtmp \
    --disable-gnutls \
    --disable-vaapi \
    --disable-vdpau \
    --disable-xlib \
    \
    --enable-swscale \
    --enable-swresample \
    \
    --extra-cflags="-O3" \
    --extra-ldflags="" \
    --pkg-config-flags="--static" \
    ${GPU_CONFIGURE_FLAGS[@]+"${GPU_CONFIGURE_FLAGS[@]}"}

# ── Build + Install ───────────────────────────────────────────────────────────
echo "[4/4] Building (this takes ~5–10 minutes)…"
make -j"$(nproc)"
make install

# ── Fix pkg-config prefix paths ──────────────────────────────────────────────
# FFmpeg embeds the literal configure --prefix into each .pc file.
# If the source tree path contains spaces (common on WSL/CI), pkg-config
# splits on them and cmake gets garbage include paths.  Rewrite prefix= to the
# actual install dir using a space-safe placeholder so every consumer gets
# a correct absolute path regardless of where the build ran.
echo "    fixing pkg-config paths in .pc files…"
# Use ${pcfiledir}/../.. so the bundle works on any machine after git clone —
# pkg-config substitutes the directory of the .pc file at query time, no
# need for the absolute install path to be the same as on the build host.
# Also fixes libdir/includedir when FFmpeg's configure writes them as literal
# /lib /include (happens when the build path contains spaces).
for pc in "${INSTALL_DIR}/lib/pkgconfig/"*.pc; do
    sed -i \
        -e 's|^prefix=.*|prefix=${pcfiledir}/../..|' \
        -e 's|^libdir=.*|libdir=${prefix}/lib|' \
        -e 's|^includedir=.*|includedir=${prefix}/include|' \
        "$pc"
done

echo ""
echo "=== Done! FFmpeg installed to: ${INSTALL_DIR} ==="
echo "    libs:    ${INSTALL_DIR}/lib/"
echo "    headers: ${INSTALL_DIR}/include/"
echo ""
echo "Sanity check — protocols compiled in:"
grep -E '^#define CONFIG_(NETWORK|TCP|UDP|RTMP|RTMPS|HTTPS|HLS|RTSP|TLS)_PROTOCOL[[:space:]]+1|^#define CONFIG_NETWORK 1' "${BUILD_DIR}/ffmpeg-${FFMPEG_VERSION}/config.h" || true
echo ""
echo "Next: rebuild the project"
echo "    cmake --build build -j\$(nproc)"
