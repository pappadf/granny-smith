#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) pappadf
"""Docs that list a directory's contents name everything in it.

Each check pairs a set of tracked files with the document that inventories
them, and fails for every file the document does not mention by name:

    tests/unit/support/stub_*.c    tests/unit/README.md (the stub table)

Usage: scripts/check-doc-inventories.py      Exit: 0 clean, 1 drift.
"""

import os
import subprocess
import sys

# (pattern of tracked files, the document that must name each one)
INVENTORIES = [
    ("tests/unit/support/stub_*.c", "tests/unit/README.md"),
]


def tracked(pattern):
    out = subprocess.run(["git", "ls-files", "--", pattern], capture_output=True, text=True, check=True)
    return out.stdout.split()


def main():
    missing = []
    for pattern, doc in INVENTORIES:
        with open(doc, encoding="utf-8") as f:
            text = f.read()
        files = tracked(pattern)
        if not files:
            missing.append(f"{pattern}: matches no tracked file (stale inventory entry?)")
        for path in files:
            name = os.path.basename(path)
            if f"`{name}`" not in text:
                missing.append(f"{doc}: does not name `{name}` ({path})")
    for m in missing:
        print(m)
    if missing:
        print(f"\n{len(missing)} inventory gap(s): add each file to its document.")
        return 1
    print("doc inventories ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
