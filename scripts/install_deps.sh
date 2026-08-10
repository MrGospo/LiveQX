#!/usr/bin/env bash
set -euo pipefail

# Install all build and runtime dependencies for liveqx on Ubuntu 24.04.
# Run once on a fresh machine before cmake configure.
#
# Usage: bash scripts/install_deps.sh

BOLD='\033[1m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RESET='\033[0m'

info()  { echo -e "${BOLD}[deps]${RESET} $*"; }
ok()    { echo -e "${GREEN}[ok]${RESET}   $*"; }
warn()  { echo -e "${YELLOW}[warn]${RESET} $*"; }

# ── Sanity checks ─────────────────────────────────────────────────────────────

if [[ "$EUID" -eq 0 ]]; then
    warn "Running as root. Consider running as a regular user with sudo."
fi

if ! command -v apt &>/dev/null; then
    echo "ERROR: apt not found. This script targets Ubuntu/Debian only." >&2
    exit 1
fi

UBUNTU_VERSION=$(lsb_release -rs 2>/dev/null || echo "unknown")
info "Detected Ubuntu ${UBUNTU_VERSION}"
if [[ "$UBUNTU_VERSION" != "24.04" ]]; then
    warn "This project was developed on Ubuntu 24.04. Other versions may have different package versions."
fi

# ── System update ─────────────────────────────────────────────────────────────

info "Updating package lists…"
sudo apt update -qq

# ── Build tools ───────────────────────────────────────────────────────────────

info "Installing build tools…"
sudo apt install -y \
    build-essential \
    cmake \
    ninja-build \
    pkg-config \
    git \
    nasm \
    yasm

# ── C++ library dependencies ──────────────────────────────────────────────────

info "Installing C++ library dependencies…"
sudo apt install -y \
    libsrt-openssl-dev \
    libspdlog-dev \
    nlohmann-json3-dev \
    libsqlite3-dev \
    libsodium-dev \
    libssl-dev \
    libcurl4-openssl-dev \
    libcpp-httplib-dev \
    libsrtp2-dev \
    libgtest-dev \
    libgmock-dev \
    libnuma-dev

# ── Node.js (UI build) ────────────────────────────────────────────────────────

info "Checking Node.js…"
if command -v node &>/dev/null; then
    NODE_VER=$(node --version)
    ok "Node.js already installed: ${NODE_VER}"
else
    info "Installing Node.js via NodeSource (LTS)…"
    curl -fsSL https://deb.nodesource.com/setup_lts.x | sudo -E bash -
    sudo apt install -y nodejs
fi

# ── Optional: NVENC (GPU encoder) ─────────────────────────────────────────────

info "Checking CUDA/NVENC headers…"
if dpkg -l | grep -q cuda-toolkit 2>/dev/null; then
    ok "CUDA toolkit found — NVENC encoder available."
else
    warn "CUDA toolkit not found. GPU encoder (NVENC) will not be available."
    warn "To enable: install nvidia-cuda-toolkit or cuda-toolkit from NVIDIA repo."
fi

# ── UI dependencies ───────────────────────────────────────────────────────────

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
UI_DIR="${ROOT_DIR}/ui"

if [[ -d "$UI_DIR" ]]; then
    info "Installing UI npm dependencies…"
    cd "$UI_DIR"
    npm install --prefer-offline 2>/dev/null || npm install
    ok "UI dependencies installed."
else
    warn "ui/ directory not found, skipping npm install."
fi

# ── Summary ───────────────────────────────────────────────────────────────────

echo ""
echo -e "${BOLD}─────────────────────────────────────────${RESET}"
ok "All dependencies installed."
echo ""
echo "Next steps:"
echo "  1. cd liveqx"
echo "  2. mkdir build && cd build"
echo "  3. cmake .. -DCMAKE_BUILD_TYPE=Release"
echo "  4. cmake --build . -j\$(nproc)"
echo ""
echo "  UI build (if needed):"
echo "  5. cd ui && npm run build"
echo -e "${BOLD}─────────────────────────────────────────${RESET}"
