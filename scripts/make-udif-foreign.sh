#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) pappadf
#
# make-udif-foreign.sh - have hdiutil convert the UDIF writer's sample
# images into every compressed format it writes, for the emulator to read
# back (macOS only).
#
# The samples come from the udif_writer unit suite (GS_UDIF_INTEROP_DIR;
# see scripts/check-udif-interop.sh): <name>.dmg beside <name>.img.  For a
# few of them this writes <name>-<FORMAT>.dmg for FORMAT in UDCO (ADC), UDZO
# (zlib), UDBZ (bzip2), ULFO (LZFSE) and ULMO (LZMA), copies <name>.img
# beside them, and lists each pair in OUT/MANIFEST ("<dmg> <img>").  The
# udif_writer suite then reads them back on any host:
#   GS_UDIF_FOREIGN_DIR=OUT make -C tests/unit/suites/udif_writer run
#
# Usage: scripts/make-udif-foreign.sh SAMPLES OUT

set -euo pipefail

SRC="${1:?usage: $0 SAMPLES OUT}"
OUT="${2:?usage: $0 SAMPLES OUT}"
command -v hdiutil >/dev/null || { echo "hdiutil not found (macOS only)" >&2; exit 1; }
mkdir -p "$OUT"
: >"$OUT/MANIFEST"

# Text (dynamic-Huffman-friendly), mixed content with zero runs, a length
# that is not a whole number of chunks, and a blank disk.
for n in text-l6 mixed-l1 odd-length blank; do
    [ -f "$SRC/$n.dmg" ] || { echo "no $SRC/$n.dmg" >&2; exit 1; }
    cp "$SRC/$n.img" "$OUT/$n.img"
    for fmt in UDCO UDZO UDBZ ULFO ULMO; do
        rm -f "$OUT/$n-$fmt.dmg"
        hdiutil convert "$SRC/$n.dmg" -format "$fmt" -o "$OUT/$n-$fmt.dmg" -quiet
        hdiutil imageinfo -format "$OUT/$n-$fmt.dmg" | grep -qx "$fmt" ||
            { echo "$n-$fmt.dmg: hdiutil reports another format" >&2; exit 1; }
        echo "$n-$fmt.dmg $n.img" >>"$OUT/MANIFEST"
        printf '%-20s %s\n' "$n-$fmt.dmg" "$(stat -f %z "$OUT/$n-$fmt.dmg") bytes"
    done
done
