#!/usr/bin/env bash

set -euo pipefail

# macOS local builder for VROOM
# - Stages the repository to avoid modifying your working tree
# - Replaces std::jthread with std::thread in the staged copy only
# - Builds a minimal binary (no routing backends) suitable for matrix-only tests
# - Outputs binary to bin/vroom-macos in your repo

usage() {
  cat <<EOF
Usage: $(basename "$0") [--without-routing] [--keep-stage]

Options:
  --without-routing  Build without routing backends (matrix-only)
  --keep-stage     Keep the staging directory after build (for debugging)
  -h, --help       Show this help

Notes:
  - Default build enables routing wrappers (HTTP/SSL) like devbox.
  - If libosrm is not installed, the libosrm wrapper is skipped automatically.
  - The replacement of std::jthread is applied only to the staged copy.
EOF
}

WITH_ROUTING=true
KEEP_STAGE=false
while [[ $# -gt 0 ]]; do
  case "$1" in
    --without-routing) WITH_ROUTING=false; shift ;;
    --keep-stage)   KEEP_STAGE=true; shift ;;
    -h|--help)      usage; exit 0 ;;
    *) echo "Unknown option: $1" >&2; usage; exit 1 ;;
  esac
done

ROOT_DIR="$(cd "$(dirname "$0")"/.. && pwd)"
STAGE_DIR="${ROOT_DIR}/.build-macos-stage"
OUT_BIN_DIR="${ROOT_DIR}/bin"
OUT_BIN="${OUT_BIN_DIR}/vroom-macos"

echo "[build-macos] Root: ${ROOT_DIR}"
echo "[build-macos] Staging into: ${STAGE_DIR}"

rm -rf "${STAGE_DIR}"
mkdir -p "${STAGE_DIR}"

echo "[build-macos] Copying repository to stage..."
rsync -a --delete \
  --exclude ".git" \
  --exclude ".build-macos-stage" \
  --exclude "build" \
  --exclude "node_modules" \
  "${ROOT_DIR}/" "${STAGE_DIR}/"

pushd "${STAGE_DIR}" >/dev/null

echo "[build-macos] Initializing submodules (if any)..."
git submodule update --init --recursive || true

echo "[build-macos] Replacing std::jthread -> std::thread in staged copy..."
find src -type f \( -name "*.cpp" -o -name "*.hpp" -o -name "*.h" \) -print0 \
  | xargs -0 sed -i '' -e 's/std::jthread/std::thread/g'

# Try to wire Homebrew headers/libs for OpenSSL/asio/etc. if available
if command -v brew >/dev/null 2>&1; then
  BREW_PREFIX="$(brew --prefix)"
  OPENSSL_PREFIX="$(brew --prefix openssl@3 2>/dev/null || true)"
  [[ -z "$OPENSSL_PREFIX" ]] && OPENSSL_PREFIX="$BREW_PREFIX"
  export CPATH="${BREW_PREFIX}/include:${OPENSSL_PREFIX}/include:${CPATH:-}"
  export LIBRARY_PATH="${BREW_PREFIX}/lib:${OPENSSL_PREFIX}/lib:${LIBRARY_PATH:-}"
  export PKG_CONFIG_PATH="${BREW_PREFIX}/lib/pkgconfig:${OPENSSL_PREFIX}/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
  echo "[build-macos] Using Homebrew includes/libs from: $BREW_PREFIX (openssl at $OPENSSL_PREFIX)"
else
  echo "[build-macos] Homebrew not found; assuming system headers/libs are sufficient"
fi

JOBS=$(sysctl -n hw.ncpu 2>/dev/null || echo 4)

MAKE_FLAGS=("-C" "src" "-j${JOBS}")
if [[ "${WITH_ROUTING}" != "true" ]]; then
  DUSE="false"
else
  DUSE="true"
fi
# Match devbox’s -DNDEBUG to avoid assertions in release-like builds
CXXFLAGS_OVERRIDE="-MMD -MP -I. -std=c++20 -Wextra -Wpedantic -Wall -O3 -DASIO_STANDALONE -DUSE_ROUTING=${DUSE} -DNDEBUG"
if [[ "${WITH_ROUTING}" != "true" ]]; then
  echo "[build-macos] Building without routing backends"
  MAKE_FLAGS+=("USE_ROUTING=false" "CXXFLAGS=${CXXFLAGS_OVERRIDE}")
else
  echo "[build-macos] Building with routing wrappers (HTTP/SSL). If libosrm is missing it will be skipped."
  MAKE_FLAGS+=("USE_ROUTING=true" "CXXFLAGS=${CXXFLAGS_OVERRIDE}")
fi

echo "[build-macos] Running make ${MAKE_FLAGS[*]}"
make "${MAKE_FLAGS[@]}"

mkdir -p "${OUT_BIN_DIR}"
cp "${STAGE_DIR}/bin/vroom" "${OUT_BIN}"
chmod +x "${OUT_BIN}"

popd >/dev/null

if [[ "${KEEP_STAGE}" == "false" ]]; then
  rm -rf "${STAGE_DIR}"
fi

echo "[build-macos] Done. Binary at: ${OUT_BIN}"
echo "[build-macos] Try: ${OUT_BIN} -v"


