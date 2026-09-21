#!/usr/bin/env bash
# fetch_platen.sh — fetch EfterScript's prebuilt session library for a
# pinned release into the local cache, verified against the release's
# SHA256SUMS.
#
#   scripts/fetch_platen.sh <version> <cache-dir> <asset> [<asset>...]
#
# Every EfterScript tag v<version> attaches to its GitHub release:
#   libplaten-<version>-<host triple>.a           the host archive
#   libplaten-<version>-wasm32-unknown-emscripten-<emsdk>.a
#   platen-<version>.h                            the C header
#   SHA256SUMS                                    checksums of the above
# (crates/efterscript-platen/docs/embedding.md, "Prebuilt archives").
#
# Each asset lands at <cache-dir>/<asset> only after its checksum matched;
# a present asset is re-verified, not re-downloaded.  Idempotent, so the
# makefiles can name the files as prerequisites.  Needs curl and sha256sum.
set -euo pipefail

if [ $# -lt 3 ]; then
    echo "usage: $0 <version> <cache-dir> <asset> [<asset>...]" >&2
    exit 2
fi
version=$1
cache=$2
shift 2

base="${PLATEN_RELEASE_BASE:-https://github.com/efterscript/efterscript/releases/download}/v${version}"
mkdir -p "$cache"
sums="$cache/SHA256SUMS"

# Temp files carry the PID.  Make runs the asset rules in parallel -- the
# header and the native archive are separate targets of one `make -j` -- and
# every invocation refreshes SHA256SUMS, so a shared "$sums.tmp" is two
# writers and two renames of one path: whichever process renames first wins
# and the other's `mv` dies with "cannot stat .../SHA256SUMS.tmp".  Observed
# in CI on this repo twice in a row, and reproduces 5 times out of 5 by
# running two fetches for the same cache concurrently.  The rename onto the
# final name stays atomic, so concurrent winners are harmless.
tmp_suffix=".tmp.$$"
cleanup() { rm -f "$sums$tmp_suffix"; }
trap cleanup EXIT

# Retry hard, including on connection-level errors.
#
# Plain `--retry` only covers transient HTTP responses and timeouts, not a
# reset mid-transfer, and a reset is exactly what a parallel build provokes:
# `make -j` starts the asset rules together, so several curls open on the
# same host in the same instant and one of them draws
# `curl: (35) Recv failure: Connection reset by peer`.  Seen in CI on this
# repo once the earlier temp-file race was out of the way.
#
# --retry-all-errors is curl 7.71+ (2020); probe for it rather than assume,
# because an unknown option would fail every build instead of some.
retry="--retry 5 --retry-delay 2"
if curl --help all 2>/dev/null | grep -q -- '--retry-all-errors'; then
    retry="$retry --retry-all-errors"
fi

# The checksum file is small and is what the rest is verified against;
# always take the release's current copy.
curl -fsSL $retry -o "$sums$tmp_suffix" "$base/SHA256SUMS"
mv -f "$sums$tmp_suffix" "$sums"

for asset in "$@"; do
    dest="$cache/$asset"
    if ! grep -q " $asset\$" "$sums"; then
        echo "fetch_platen: $asset is not in the v$version release's SHA256SUMS" >&2
        exit 1
    fi
    if [ -f "$dest" ] && (cd "$cache" && grep " $asset\$" SHA256SUMS | sha256sum -c --quiet - 2>/dev/null); then
        echo "fetch_platen: $asset present and verified"
        continue
    fi
    echo "fetch_platen: downloading $asset (v$version)"
    curl -fsSL $retry -o "$dest$tmp_suffix" "$base/$asset"
    mv -f "$dest$tmp_suffix" "$dest"
    if ! (cd "$cache" && grep " $asset\$" SHA256SUMS | sha256sum -c --quiet -); then
        rm -f "$dest"
        echo "fetch_platen: checksum mismatch for $asset; removed" >&2
        exit 1
    fi
    echo "fetch_platen: $asset verified"
done
