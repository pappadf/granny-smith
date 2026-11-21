#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) pappadf
"""repo.py -- the repository root and its tracked files, for the lint scripts.

The doc and hygiene linters (check-doc-inventories, check-doc-rules,
check-external-refs, check-links, check-spdx) each wrapped `git ls-files`,
some splitting its output on whitespace, which breaks on a path with a
space.  tracked() reads it NUL-separated.
"""

import pathlib
import subprocess

# The repository root (this file is scripts/lib/repo.py).
ROOT = pathlib.Path(__file__).resolve().parents[2]


def tracked(*patterns, cwd=None):
    """The tracked paths matching `patterns` (all with none), as git prints
    them: relative to `cwd` (default: the current directory)."""
    cmd = ["git", "ls-files", "-z"] + (["--", *patterns] if patterns else [])
    out = subprocess.run(cmd, cwd=cwd, capture_output=True, check=True).stdout
    return [p for p in out.decode().split("\0") if p]
