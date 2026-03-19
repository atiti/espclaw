#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build/emscripten/site/runtime"
CACHE_DIR="${ROOT_DIR}/wasm/cache"
OUTPUT_DIR="${ROOT_DIR}/site/wasm"

if ! command -v emcmake >/dev/null 2>&1; then
  echo "emcmake is required to build the browser runtime." >&2
  exit 1
fi

# Homebrew-packaged Emscripten generates relative source paths for some system
# libraries. Keeping a repo-local opt/homebrew symlink makes those paths
# resolvable when EM_CACHE lives under this repository.
if [ -d /opt/homebrew ]; then
  mkdir -p "${ROOT_DIR}/opt"
  ln -sfn /opt/homebrew "${ROOT_DIR}/opt/homebrew"
fi

mkdir -p "${CACHE_DIR}"
emcmake cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}"
EM_CACHE="${CACHE_DIR}" cmake --build "${BUILD_DIR}" --target espclaw_browser_runtime -j4

mkdir -p "${OUTPUT_DIR}"
cp "${BUILD_DIR}/espclaw-browser-runtime.js" "${OUTPUT_DIR}/espclaw-browser-runtime.js"
cp "${BUILD_DIR}/espclaw-browser-runtime.wasm" "${OUTPUT_DIR}/espclaw-browser-runtime.wasm"

echo "Built browser runtime into ${OUTPUT_DIR}"
