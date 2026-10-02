#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) pappadf
#
# check-udif-interop.sh - open the UDIF writer's sample images with other
# tools and check each decodes to the disk it was written from.
#
# The samples come from the udif_writer unit suite:
#   GS_UDIF_INTEROP_DIR=DIR make -C tests/unit/suites/udif_writer run
# which leaves <name>.dmg beside <name>.img (the decoded disk, padded to a
# sector) and a MANIFEST of the names.
#
# Usage: scripts/check-udif-interop.sh DIR
#
# With hdiutil (macOS), each image must pass `hdiutil imageinfo` and
# `hdiutil verify`, `hdiutil convert -format UDTO` must yield the disk, and
# `hdiutil attach -readonly -nomount` must present a device that reads as the
# disk.  With 7z, `7z x` must yield the disk.  Fails when neither tool is
# present, or on the first mismatch.

set -euo pipefail

DIR="${1:?usage: $0 DIR}"
[ -f "$DIR/MANIFEST" ] || { echo "no $DIR/MANIFEST" >&2; exit 1; }
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

have() { command -v "$1" >/dev/null 2>&1; }
tools=0
fail=0

check_hdiutil() {
    local n="$1" dmg="$DIR/$1.dmg" img="$DIR/$1.img"
    hdiutil imageinfo "$dmg" >"$WORK/$n.info"
    hdiutil verify "$dmg" >"$WORK/$n.verify" 2>&1 || { cat "$WORK/$n.verify"; return 1; }
    hdiutil convert "$dmg" -format UDTO -o "$WORK/$n" >/dev/null
    cmp "$WORK/$n.cdr" "$img" || return 1
    rm -f "$WORK/$n.cdr"
    local dev
    dev=$(hdiutil attach -readonly -nomount -noverify -noautofsck "$dmg" | awk 'NR==1 {print $1}')
    [ -n "$dev" ] || return 1
    local rc=0
    # The whole device, read raw: the disk the image presents.
    sudo dd if="${dev/disk/rdisk}" bs=1m 2>/dev/null | cmp - "$img" || rc=1
    hdiutil detach "$dev" >/dev/null || hdiutil detach -force "$dev" >/dev/null
    return $rc
}

check_7z() {
    local n="$1" dmg="$DIR/$1.dmg" img="$DIR/$1.img"
    rm -rf "$WORK/x"
    7z x -y -o"$WORK/x" "$dmg" >"$WORK/$n.7z" 2>&1 || { cat "$WORK/$n.7z"; return 1; }
    # One partition-less disk: one extracted file, the whole disk.
    local out
    out=$(find "$WORK/x" -type f | head -1)
    [ -n "$out" ] && cmp "$out" "$img"
}

run() {
    local tool="$1"
    tools=$((tools + 1))
    while read -r n; do
        [ -n "$n" ] || continue
        if "check_$tool" "$n"; then
            printf '%-8s %-12s ok\n' "$tool" "$n"
        else
            printf '%-8s %-12s FAILED\n' "$tool" "$n"
            fail=1
        fi
    done <"$DIR/MANIFEST"
}

have hdiutil && run hdiutil
have 7z && run 7z
[ "$tools" -gt 0 ] || { echo "neither hdiutil nor 7z found" >&2; exit 1; }
exit $fail
