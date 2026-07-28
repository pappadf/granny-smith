#!/usr/bin/env python3
"""check-goldens.py — every screen assertion in a test must guard a DISTINCT frame.

A `check("a.png")` and a `check("b.png")` in the same script are two separate
claims about two separate user-visible states. If a.png and b.png are
byte-identical, they are one claim written twice: any frame satisfying one
satisfies the other, so a regression that breaks only the state b.png describes
still passes. The test stays green while its coverage silently halves.

That is not hypothetical. The §7 re-host of iicx-mactest to the IIci landed with
all seven of its checkpoints recaptured to the same frame — and that frame was
MacTest's "SUSPECTED PROBLEM: Logic board" dialog, so a golden named
floppy-test-success.png asserted a hardware failure. It passed CI. The
golden-audit pass that followed missed it because it counted distinct hashes per
DIRECTORY (that one had three, which looked plausible) rather than asking
whether the frames the check() calls actually target differ from each other.
This script asks the second question, which is the one that matters.

Recapturing (REGEN=1) is what makes this reachable at all: it turns whatever is
on screen into the expected result. A collision after a recapture nearly always
means the choreography stopped advancing and every later checkpoint settled on
one stuck frame.

Usage:  scripts/check-goldens.py [tests/integration]
Exit:   0 clean, 1 collisions found.
"""

import hashlib
import re
import sys
from collections import defaultdict
from pathlib import Path

# check("x.png") / check_ex("x.png", ...) / machine.screen.match x.png
PATTERNS = [
    re.compile(r'\bcheck(?:_ex)?\(\s*"([^"]+\.png)"'),
    re.compile(r'\bmachine\.screen\.match\s+"?([^\s"]+\.png)"?'),
]

# A row may legitimately re-assert the SAME golden at two points — the IIci
# checkpoint row matches its pre-save desktop again after the restore, which is
# the entire assertion. Repeating one path is a deliberate round-trip; two
# DIFFERENT paths holding identical bytes is the defect.

# ...usually. Some collisions are real and correct: the "Welcome to Macintosh"
# splash is pure black-and-white, so it scans out to identical pixels at 1 bpp
# and 2 bpp, and to identical pixels from two different cards at the same
# geometry. Those rows prove the state actually changed with asserts on the
# hardware (PRAM savedMode, CLUT PBCR, RowWords, card sister byte) and use the
# golden only to pin that the raster still scans out. That is sound.
#
# The distinction cannot be drawn mechanically, so it is drawn by review: put
#
#     # golden-collision-ok: <a.png> <b.png> — why the frames are identical
#                            and what proves the states differ
#
# in the script. Naming both files makes the waiver specific — it cannot
# silently start covering a third golden that collides later — and forces the
# author to write down the separate proof. A waiver with no such proof behind it
# is the mactest failure mode wearing a comment.
WAIVER = re.compile(r"#\s*golden-collision-ok:\s*(.+)$")


def scan(script: Path):
    """Return (assertion targets, waived basename-sets) for one script."""
    text = script.read_text(encoding="utf-8", errors="replace")
    out, waivers = [], []
    for line in text.splitlines():
        waiver = WAIVER.search(line)
        if waiver:
            # Everything up to the em-dash is the file list; the rest is prose.
            names = re.split(r"[—-]{1,2}\s", waiver.group(1), maxsplit=1)[0]
            waived = {Path(n).name for n in names.split() if n.endswith(".png")}
            if len(waived) > 1:
                waivers.append(waived)
            continue
        if line.lstrip().startswith("#"):
            continue
        for pat in PATTERNS:
            out.extend(pat.findall(line))
    return out, waivers


def main() -> int:
    root = Path(sys.argv[1] if len(sys.argv) > 1 else "tests/integration")
    if not root.is_dir():
        print(f"check-goldens: no such directory: {root}", file=sys.stderr)
        return 1

    collisions = 0
    checked = 0
    waived_count = 0
    for script in sorted(root.glob("*/*.script")):
        if script.parent.name == "lib":
            continue
        refs, waivers = scan(script)
        by_digest = defaultdict(set)
        for ref in refs:
            golden = (script.parent / ref).resolve()
            if not golden.is_file():
                # Missing goldens are the runner's problem, not this lint's:
                # a REGEN run creates them and a verify run fails loudly.
                continue
            digest = hashlib.md5(golden.read_bytes()).hexdigest()
            by_digest[digest].add(ref)
            checked += 1

        for digest, group in sorted(by_digest.items()):
            if len(group) < 2:
                continue
            names = {Path(r).name for r in group}
            # A waiver must name every file in the group: a waiver covering
            # only part of a collision leaves the rest unexplained.
            if any(names <= waived for waived in waivers):
                waived_count += 1
                continue
            collisions += 1
            rel = script.relative_to(root)
            print(f"COLLISION {rel}: {len(group)} distinct assertions share "
                  f"one frame ({digest[:8]})")
            for ref in sorted(group):
                print(f"    {ref}")

    if collisions:
        print()
        print(f"{collisions} golden collision(s). Each is one frame doing the work "
              f"of several assertions.")
        print("Re-tune the waits so every checkpoint reaches its intended state, "
              "then recapture.")
        print("If the frames are genuinely identical and something else proves "
              "the states differ,")
        print("waive it explicitly:  # golden-collision-ok: a.png b.png - "
              "<why, and what proves it>")
        return 1

    note = f", {waived_count} reviewed collision(s) waived" if waived_count else ""
    print(f"goldens ok: {checked} screen assertions, no unexplained "
          f"collisions{note}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
