#!/usr/bin/env bash
set -euo pipefail
REQ=4.0.10
if ! command -v emcc >/dev/null 2>&1; then
  echo "[toolchain] emcc missing (expected $REQ). Do NOT run emsdk install inside agent; base image must supply it." >&2
  exit 2
fi
VER=$(emcc --version 2>/dev/null | head -n1 | sed -nE 's/.*emcc .* ([0-9.]+) .*/\1/p')
if [[ "$VER" != "$REQ" ]]; then
  echo "[toolchain] emcc version $VER (expected $REQ). Proceed but consider rebuilding dev image." >&2
  exit 0
fi
echo "[toolchain] emcc $VER OK"