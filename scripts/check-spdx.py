#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) pappadf
"""Every C source and script starts with the two-line notice (STYLE_GUIDE.md).

C sources and headers in src/, tests/ and tools/ begin with exactly
    // SPDX-License-Identifier: MIT
    // Copyright (c) pappadf
Scripts under scripts/ carry the same two lines in their own comment syntax
(# or //), after a #! line if there is one.

Usage: scripts/check-spdx.py        Exit: 0 clean, 1 files missing it.
"""

import subprocess
import sys

SPDX = "SPDX-License-Identifier: MIT"
COPY = "Copyright (c) pappadf"


def tracked(*patterns):
    out = subprocess.run(["git", "ls-files", "--", *patterns], capture_output=True, text=True, check=True)
    return out.stdout.split()


def c_ok(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        head = [f.readline().rstrip("\n") for _ in range(2)]
    return head == [f"// {SPDX}", f"// {COPY}"]


def script_ok(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        head = [f.readline().rstrip("\n") for _ in range(3)]
    if head and head[0].startswith("#!"):
        head = head[1:]
    head = head[:2]
    return any(head == [f"{c} {SPDX}", f"{c} {COPY}"] for c in ("#", "//"))


def main():
    bad = [p for p in tracked("src/*.c", "src/*.h", "tests/*.c", "tests/*.h", "tools/*.c", "tools/*.h")
           if not c_ok(p)]
    bad += [p for p in tracked("scripts/*.py", "scripts/*.sh", "scripts/*.mjs", "scripts/*.js") if not script_ok(p)]
    if bad:
        print("Files that do not begin with the two-line SPDX / copyright notice:")
        for p in bad:
            print(f"  {p}")
        return 1
    print("spdx: every C source and script carries the notice")
    return 0


if __name__ == "__main__":
    sys.exit(main())
