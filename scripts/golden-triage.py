#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) pappadf
"""golden-triage.py — rank reference images by how much is actually ON them.

NOT a matching rule. Goldens are compared byte-exactly by machine.screen.match
and always will be; there is no tolerance, no perceptual diff, no fuzzy compare
anywhere in this suite. This script only helps a human FIND reference images
worth a second look, which is a different job from deciding whether a frame
matches.

Why it exists. scripts/check-goldens.py catches two goldens that are identical
to each other. It cannot catch a LONE bad golden — one row whose reference is a
blank screen, with nothing to collide against. Four of those were found in this
tree, all the same shape: a wait settled on the desktop pattern, which is
painted long before the Finder draws its menu bar and icons and is static for
tens of millions of instructions, so REGEN and verify agreed on a picture of an
empty screen and the row claimed a coverage cell against it.

The measure is the count of distinct 8x8 tiles. A dither pattern is a handful;
a Welcome splash is ~64; a Finder desktop with a menu bar and icons is 100+.

Read it two ways, the second being much the stronger:

  1. The absolute cliff. In this tree: 1, 9, 25, then 56 and up. Anything in
     the low tens is worth opening.
  2. AGAINST ITS SIBLINGS. Within one depth sweep every cell lands on the same
     splash, so one cell reading 8 where its three siblings read 64 is wrong
     regardless of any threshold. Every real defect found so far was obvious
     this way and only suggestive by absolute value. Sort by name and compare
     within a group before trusting the global ranking.

There is no pass/fail here on purpose: "how sparse may a legitimate screen be"
is a judgement, and a threshold would either miss real ones or cry wolf. Run it
after any recapture, look at what it puts on top, and decide.

Usage:  scripts/golden-triage.py [tests/integration]
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "lib"))
import png  # noqa: E402

# Confirmed legitimate by review (2026-07-28) — genuinely near-empty screens,
# listed so a later pass does not re-litigate them. Anything else near the top
# of the ranking has not been looked at yet.
REVIEWED_SPARSE = {
    "lisa-los-profile/expected_poweroff.png":
        "powered-off Lisa: the screen really is blank",
    "suite-lisa/goldens/lisa-xenix3.0-720x364x1-hdinit-bootprompt.png":
        "sparse text boot prompt on black",
    "suite-se30/goldens/se30-nomedia-512x342.png":
        "no-media blinking '?' floppy on the desktop pattern",
}


def read_png(path: Path):
    """Decode a PNG to (width, height, bytes-per-pixel-ish, rows). Stdlib only."""
    chs = png.chunks(path.read_bytes())
    w, h, bitd, ctype = png.ihdr(chs)[:4]
    channels = png.CHANNELS[ctype]
    lines = png.rows(png.inflate(chs), w, h, channels, bitd)
    return w, h, channels * bitd // 8, [line for _, line in lines]


def distinct_tiles(path: Path):
    w, h, px, rows = read_png(path)
    seen = set()
    for y in range(0, h - 7, 8):
        for x in range(0, w - 7, 8):
            off, span = x * px, 8 * px
            seen.add(tuple(rows[y + dy][off:off + span] for dy in range(8)))
    return w, h, len(seen)


def main() -> int:
    root = Path(sys.argv[1] if len(sys.argv) > 1 else "tests/integration")
    results = []
    for png in sorted(root.rglob("*.png")):
        if "test-results" in png.parts:
            continue  # run artifacts, not committed references
        try:
            w, h, n = distinct_tiles(png)
        except Exception as exc:  # a malformed golden is its own problem
            print(f"  ERR {png}: {exc}", file=sys.stderr)
            continue
        results.append((n, w, h, str(png.relative_to(root))))

    for n, w, h, rel in sorted(results):
        note = REVIEWED_SPARSE.get(rel)
        mark = f"   [reviewed: {note}]" if note else ""
        print(f"{n:6d} tiles  {w}x{h:<5} {rel}{mark}")

    print(f"\n{len(results)} reference images. Sparse ones sort first; compare "
          f"each against its siblings before concluding anything.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
