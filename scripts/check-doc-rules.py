#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) pappadf
"""The docs content rules that are machine-checkable (docs/README.md, R1/R2).

Scanned: every tracked *.md under docs/, plus the root README.md,
CONTRIBUTING.md and USAGE.md.

Checked:

1. R1/R2 denylist — no naming or referencing other emulators, no leaked
   Apple source.  A hit fails unless it is covered by the BASELINE below
   (the known, not-yet-fixed violations — the PR that fixes one removes its
   entry) or carries an inline allow annotation on the same line:
   `<!-- lint-allow: SuperMario -->` suppresses that term on that line (the
   mechanism for Apple codenames, which R2 permits as names for ROMs,
   boards and projects).

2. Stub consistency — every placeholder stub carries both the banner and
   the machine-readable marker, and its R5 structure holds: exactly one H1,
   `##` sections numbered (`## 3. Title`), `## References` as the last
   section.  R5 layout for the tree at large is not linted yet: it lands
   doc-by-doc as existing pages are normalized (docs/README.md).

Usage: scripts/check-doc-rules.py     Exit: 0 clean, 1 violations.
"""

import os
import re
import subprocess
import sys

BANNER = re.compile(r"^> 🚧 \*\*Placeholder\*\*", re.M)
MARKER = re.compile(r"^<!-- gs-doc-status: stub -->\s*$", re.M)
ALLOW = re.compile(r"<!--\s*lint-allow:\s*([^>]+?)\s*-->")

# Terms never allowed in docs prose (rule R1: standalone work; rule R2: no
# leaked source).  Word-bounded, case-insensitive.  The label is what a
# lint-allow annotation names.
DENYLIST = [
    ("MAME", r"MAME"),
    ("Basilisk", r"Basilisk"),
    ("SheepShaver", r"SheepShaver"),
    ("Mini vMac", r"Mini\s?v\s?Mac"),
    ("QEMU", r"QEMU"),
    ("PearPC", r"PearPC"),
    ("LisaEm", r"LisaEm"),
    ("PCE", r"PCE"),
    ("other emulator", r"other\semulators?"),
    ("leaked", r"leaked"),
    ("SuperMario", r"SuperMario"),
]
DENY = [(label, re.compile(r"(?<![A-Za-z0-9-])" + pat + r"(?![A-Za-z0-9-])", re.I))
        for label, pat in DENYLIST]

# Known violations, with the hit count each file is allowed until its fix
# lands (the PR that fixes a violation removes its baseline entry).  This is
# what makes the lint fail on NEW violations without failing on the old
# ones.  None of these may grow.
BASELINE = {
    ("docs/core/peripherals/mouse_control.md", "MAME"): 2,
    ("docs/core/peripherals/mouse_control.md", "Basilisk"): 5,
    ("docs/core/peripherals/mouse_control.md", "SheepShaver"): 2,
    ("docs/core/peripherals/mouse_control.md", "Mini vMac"): 3,
    ("docs/core/peripherals/mouse_control.md", "QEMU"): 1,
    ("docs/core/peripherals/mouse_control.md", "PCE"): 1,
    ("docs/core/peripherals/mouse_control.md", "other emulator"): 2,
    ("docs/core/peripherals/pci/cards/voodoo2.md", "other emulator"): 1,
    ("docs/reference/hardware/iwm-floppy.md", "MAME"): 1,
    ("docs/reference/hardware/rtc.md", "MAME"): 1,
    ("docs/reference/hardware/rtc.md", "Mini vMac"): 1,
    ("docs/reference/hardware/rtc.md", "PCE"): 1,
    ("docs/reference/machines/glue/se30.md", "MAME"): 1,
    ("docs/reference/machines/lisa/pram.md", "other emulator"): 1,
}

ROOT_DOCS = ["README.md", "CONTRIBUTING.md", "USAGE.md"]

H1 = re.compile(r"^#(?!#)\s*\S")
H2 = re.compile(r"^##\s+(\S+)")
H2_NUM = re.compile(r"^##\s+\d+\.\s+\S")
FENCE = re.compile(r"^\s*(```|~~~)")


def scanned():
    out = subprocess.run(["git", "ls-files", "--", "*.md"], capture_output=True, text=True, check=True)
    return [p for p in out.stdout.split() if p.startswith("docs/") or p in ROOT_DOCS]


def denylist_violations(path):
    # Count hits per term across the file (skip fences and annotated lines),
    # then compare the totals against the baseline: a violation is any term
    # over its allowed count — or not in the baseline at all.
    with open(path, encoding="utf-8", errors="replace") as f:
        lines = f.readlines()
    in_fence = False
    counts = {}
    first_line = {}
    for n, line in enumerate(lines, 1):
        if FENCE.match(line):
            in_fence = not in_fence
            continue
        if in_fence:
            continue
        m = ALLOW.search(line)
        allowed = {t.strip() for t in m.group(1).split(",")} if m else set()
        # count on the line with the annotation itself stripped out
        plain = ALLOW.sub("", line)
        for term, rx in DENY:
            if term in allowed:
                continue
            hits = len(rx.findall(plain))
            if hits:
                counts[term] = counts.get(term, 0) + hits
                first_line.setdefault(term, n)
    for term, total in sorted(counts.items()):
        if BASELINE.get((path, term), 0) < total:
            yield f"{path}:{first_line[term]}: denylist term '{term}' " \
                  f"({total} hits, {BASELINE.get((path, term), 0)} allowed) (rule R1/R2)"


def stub_violations(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        text = f.read()
    has_banner = bool(BANNER.search(text))
    # both count only on their own line — a doc describing the stub format
    # (docs/README.md) mentions them in prose
    has_marker = bool(MARKER.search(text))
    if has_banner and not has_marker:
        yield f"{path}: stub banner without the machine-readable marker"
    if has_marker and not has_banner:
        yield f"{path}: stub marker without the banner"
    if not has_marker:
        return
    lines = text.splitlines()
    headings = [l for l in lines if l.startswith("#")]
    h1s = [l for l in headings if H1.match(l)]
    if len(h1s) != 1:
        yield f"{path}: stub must have exactly one H1, has {len(h1s)}"
    h2s = [l for l in headings if H2.match(l) and not l.startswith("## #")]
    for h in h2s:
        if not H2_NUM.match(h) and not h.startswith("## References"):
            yield f"{path}: unnumbered or malformed ## section in a stub: {h!r}"
    if not any(l.strip() == "## References" for l in lines):
        yield f"{path}: stub must end with a ## References section"
    else:
        last = [l for l in lines if l.strip() == "## References"][-1]
        tail = [l for l in lines[lines.index(last) + 1:] if l.strip().startswith("#")]
        if tail:
            yield f"{path}: ## References must be the final section"


def main():
    bad = 0
    for path in scanned():
        for v in denylist_violations(path):
            print(v)
            bad += 1
        for v in stub_violations(path):
            print(v)
            bad += 1
    if bad:
        print(f"\n{bad} docs rule violation(s): see docs/README.md (R1/R2/R5).")
        return 1
    print("docs rules ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
