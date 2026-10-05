#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) pappadf
"""docs/internals/ mirrors src/: every internals doc lives with its code.

The mirror rule (docs/internals/README.md):

1. Coverage is optional — no src/ file needs an internals doc; a page
   exists only when there is something to say about the implementation.
2. Placement is determined — an internals doc lives at the path mirrored
   from its owning source: its directory is the src/ directory that owns
   the subject, and its basename follows the owning source file (or the
   subsystem/directory name when the doc owns many files).

This script checks rule 2 (rule 1 is a non-duty — there is nothing to
check in that direction).  For every *.md under docs/internals/, except
section READMEs:

  - the mirrored src/ directory must exist (docs/internals/core/peripherals/
    maps to src/core/peripherals/);
  - the doc's stem must match a source-file stem in that directory, or the
    directory's own name, or be listed with a reason in the Exceptions
    table of docs/internals/README.md (the single registry of deliberate
    deviations — never prose elsewhere).

Usage: scripts/check-doc-mirror.py [--list]    Exit: 0 clean, 1 violations.
"""

import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DOCS = os.path.join(ROOT, "docs", "internals")
EXC_DOC = os.path.join(DOCS, "README.md")

ROW = re.compile(r"^\|\s*`([\w./-]+)`\s*\|")


def exceptions():
    # the Exceptions table in docs/internals/README.md is the registry
    exc = {}
    inside = False
    try:
        with open(EXC_DOC, encoding="utf-8") as f:
            for line in f:
                if line.startswith("## "):
                    inside = line.startswith("## Exceptions")
                    continue
                if not inside:
                    continue
                m = ROW.match(line)
                if m:
                    exc[m.group(1)] = True
    except OSError:
        pass
    return exc


def violations(list_only=False):
    exc = exceptions()
    bad = []
    for path in sorted(glob.glob(os.path.join(DOCS, "**", "*.md"), recursive=True)):
        if os.path.basename(path) == "README.md":
            continue
        rel = os.path.relpath(path, DOCS).replace(os.sep, "/")
        src_dir = os.path.join(ROOT, "src", os.path.dirname(rel))
        if not os.path.isdir(src_dir):
            bad.append(f"{rel}: mirrored src/ directory does not exist")
            continue
        stem = os.path.basename(rel)[:-3]
        # source-file stems, and subdirectory stems (pci.md mirrors pci/)
        stems = {os.path.splitext(f)[0] for f in os.listdir(src_dir)}
        if stem in stems or stem == os.path.basename(src_dir):
            continue
        if rel in exc:
            continue
        bad.append(f"{rel}: stem matches no source file or directory in "
                   f"src/{os.path.dirname(rel)}")
    return bad


def main():
    list_only = "--list" in sys.argv
    bad = violations(list_only)
    for b in bad:
        print(b)
    if list_only:
        return 0
    if bad:
        print(f"\n{len(bad)} mirror violation(s): see docs/internals/README.md.")
        return 1
    print("docs mirror ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
