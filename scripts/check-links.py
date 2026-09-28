#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) pappadf
"""Relative links in the repository's Markdown resolve.

For every tracked *.md file, each Markdown link or image whose target is a
relative path ([text](../guide/TESTING.md), [text](src/main.c#L10), ![](shot.png)) must name
a file or directory that exists in the checkout.  External URLs, mailto: and
same-page #anchors are not checked; nor are links inside code spans or fenced
code blocks, which are examples, not links.

Usage: scripts/check-links.py [files...]     Exit: 0 clean, 1 broken links.
"""

import os
import re
import subprocess
import sys

LINK = re.compile(r"!?\[(?:[^\]\[]|\[[^\]]*\])*\]\(\s*<?([^)\s>]+)>?(?:\s+\"[^\"]*\")?\s*\)")
FENCE = re.compile(r"^\s*(```|~~~)")
CODE_SPAN = re.compile(r"`+[^`]*`+")
SKIP = re.compile(r"^(?:[a-z][a-z0-9+.-]*:|#|//)", re.I)


def tracked_md():
    out = subprocess.run(["git", "ls-files", "--", "*.md"], capture_output=True, text=True, check=True)
    return out.stdout.split()


def broken_links(path):
    base = os.path.dirname(path)
    in_fence = False
    with open(path, encoding="utf-8", errors="replace") as f:
        for n, line in enumerate(f, 1):
            if FENCE.match(line):
                in_fence = not in_fence
                continue
            if in_fence:
                continue
            for m in LINK.finditer(CODE_SPAN.sub("", line)):
                target = m.group(1)
                if SKIP.match(target):
                    continue
                rel = target.split("#", 1)[0].split("?", 1)[0]
                if not rel:
                    continue
                if not os.path.exists(os.path.normpath(os.path.join(base, rel))):
                    yield n, target


def main():
    files = sys.argv[1:] or tracked_md()
    bad = 0
    for path in files:
        if not path.endswith(".md") or not os.path.isfile(path):
            continue
        for n, target in broken_links(path):
            print(f"{path}:{n}: {target}")
            bad += 1
    if bad:
        print(f"\n{bad} broken relative link(s).")
        return 1
    print("links ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
