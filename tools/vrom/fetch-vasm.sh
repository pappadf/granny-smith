#!/usr/bin/env bash
# Fetch and build the pinned vasm assembler used to build the GS generic vROM.
# vasm's license permits free use but restricts redistribution of its source,
# so the tarball is fetched (and checksum-pinned) rather than vendored (§8 of
# the generic-nubus-vrom proposal).
set -euo pipefail

# Pinned release: vasm 2.0f (M68k backend 2.8c, mot syntax 3.19f).
VASM_URL="http://sun.hasenbraten.de/vasm/release/vasm.tar.gz"
VASM_SHA256="c84b2de1cbb87831795fe64a85c5d9a7002a766e3a7c30b0a2d7d5e99d878f49"

# Destination: build/vasm/ under the repo root (cached across builds).
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DEST="$ROOT/build/vasm"
BIN="$DEST/vasmm68k_mot"

# Already built and executable → nothing to do.
if [ -x "$BIN" ]; then
    echo "$BIN"
    exit 0
fi

mkdir -p "$DEST"
TARBALL="$DEST/vasm.tar.gz"

# Download unless a checksum-matching tarball is already cached.
if ! echo "$VASM_SHA256  $TARBALL" | sha256sum -c - >/dev/null 2>&1; then
    curl -fsSL -o "$TARBALL" "$VASM_URL"
    echo "$VASM_SHA256  $TARBALL" | sha256sum -c - >/dev/null
fi

# Unpack and build the m68k/mot-syntax variant (plain gcc, no dependencies).
tar -xzf "$TARBALL" -C "$DEST" --strip-components=1
make -C "$DEST" CPU=m68k SYNTAX=mot >/dev/null

[ -x "$BIN" ] || { echo "fetch-vasm.sh: build produced no $BIN" >&2; exit 1; }
echo "$BIN"
