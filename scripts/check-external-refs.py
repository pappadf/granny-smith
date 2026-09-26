#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) pappadf
"""Fail when tracked files cite documents or paths outside this repository.

A comment or doc that says "see proposal-foo §3", "the work order", or names a
review finding sends the reader somewhere a clean checkout cannot follow.  The
reason belongs next to the code, stated in full; the history belongs in git.

Checked patterns (generic shapes, not names):
  - design-document names:  proposal-<name>, "proposal §N"
  - NN-UPPER-UPPER.md work documents
  - paths into the untracked local/ tree (local/platen/, the fetched
    interpreter cache, is allowed)
  - review provenance: "NN-area F-NN" tags and bare F-NN / N-NN labels

A developer checkout may add patterns of its own: every
local/*/forbidden-refs.txt (one regex per line, '#' comments) is read when
present.  Nothing under local/ is tracked, so CI checks the generic set only.

Usage:
  scripts/check-external-refs.py            # check the tracked tree
  scripts/check-external-refs.py FILE...    # check these files (pre-commit)
  scripts/check-external-refs.py --list     # print every match
"""

import glob
import os
import re
import subprocess
import sys

# Lines allowed to match in the tracked tree.  The tree is being cleaned in
# steps; this ceiling only ever goes down, and the check fails above it.
MAX_MATCHES = 1693

PATTERNS = [
    ("design-document name", r"proposal-[a-z0-9]"),
    ("design-document section", r"\b[Pp]roposal\s+§"),
    ("work document", r"\b[0-9]{2}-[A-Z]+-[A-Z]+\.md\b"),
    ("path into local/", r"local/(?!platen/)[a-z][a-z0-9_-]*/"),
    ("review tag", r"\b[01][0-9]-[a-z]+(?:-[a-z]+)*[ ,]+[A-Z]-?[0-9]"),
    ("review label", r"\b[FN]-[0-9]{2}\b"),
]

ROOT = subprocess.run(["git", "rev-parse", "--show-toplevel"], capture_output=True,
                      text=True, check=True).stdout.strip()
SELF = "scripts/check-external-refs.py"


def local_patterns():
    pats = []
    for path in sorted(glob.glob(os.path.join(ROOT, "local", "*", "forbidden-refs.txt"))):
        with open(path, encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if line and not line.startswith("#"):
                    pats.append(("local pattern", line))
    return pats


def tracked_files():
    out = subprocess.run(["git", "ls-files", "-z"], cwd=ROOT, capture_output=True,
                         check=True).stdout
    return [p for p in out.decode().split("\0") if p]


def scan(files, patterns):
    compiled = [(label, re.compile(rx)) for label, rx in patterns]
    hits = []
    for rel in files:
        if rel == SELF or rel.startswith("local/"):
            continue
        path = os.path.join(ROOT, rel)
        try:
            with open(path, "rb") as f:
                data = f.read()
        except (IsADirectoryError, FileNotFoundError):
            continue
        if b"\0" in data[:8192]:
            continue  # binary
        text = data.decode("utf-8", errors="replace")
        for n, line in enumerate(text.splitlines(), 1):
            for label, rx in compiled:
                if rx.search(line):
                    hits.append((rel, n, label, line.strip()))
                    break
    return hits


def main(argv):
    list_all = "--list" in argv
    args = [a for a in argv if a != "--list"]
    patterns = PATTERNS + local_patterns()
    whole_tree = not args
    files = tracked_files() if whole_tree else [os.path.relpath(os.path.abspath(a), ROOT) for a in args]
    hits = scan(files, patterns)
    if list_all:
        for rel, n, label, line in hits:
            print(f"{rel}:{n}: [{label}] {line}")
    if not whole_tree:
        # Per-file mode (pre-commit): a file may not gain matches.  Compare
        # against the committed version so untouched legacy lines pass.
        grown = []
        for rel in sorted({h[0] for h in hits}):
            now = sum(1 for h in hits if h[0] == rel)
            old = subprocess.run(["git", "show", f"HEAD:{rel}"], cwd=ROOT, capture_output=True)
            before = 0
            if old.returncode == 0 and b"\0" not in old.stdout[:8192]:
                before = len(scan_text(old.stdout.decode("utf-8", errors="replace"), patterns))
            if now > before:
                grown.append((rel, before, now))
        if grown:
            print("References to documents or paths outside this repository were added:")
            for rel, before, now in grown:
                print(f"  {rel}: {before} -> {now}")
                for h in hits:
                    if h[0] == rel:
                        print(f"    {h[1]}: [{h[2]}] {h[3]}")
            print("State the reason in full instead; see scripts/check-external-refs.py.")
            return 1
        return 0
    # Whole tree: the ceiling covers the generic set, which is all CI can see.
    # A checkout's own patterns are reported, and must be clear once the
    # ceiling reaches zero.
    generic = [h for h in hits if h[2] != "local pattern"]
    extra = len(hits) - len(generic)
    status = 0
    if len(generic) > MAX_MATCHES:
        print(f"{len(generic)} lines cite documents or paths outside this repository "
              f"(allowed: {MAX_MATCHES}).  Run with --list to see them.")
        status = 1
    elif len(generic) < MAX_MATCHES:
        print(f"ok: {len(generic)} lines (ceiling {MAX_MATCHES}); lower MAX_MATCHES to {len(generic)}.")
    else:
        print(f"ok: {len(generic)} lines (ceiling {MAX_MATCHES}).")
    if extra:
        print(f"{extra} more lines match this checkout's local patterns.")
        if MAX_MATCHES == 0:
            status = 1
    return status


def scan_text(text, patterns):
    compiled = [re.compile(rx) for _, rx in patterns]
    return [n for n, line in enumerate(text.splitlines(), 1) if any(rx.search(line) for rx in compiled)]


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
