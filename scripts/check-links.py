#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) pappadf
"""Relative links in the repository's Markdown resolve, and citations check out.

For every tracked *.md file:

1. Each Markdown link or image whose target is a relative path
   ([text](../guide/TESTING.md), [text](src/main.c#L10), ![](shot.png)) must
   name a file or directory that exists in the checkout.

2. A link's heading anchor ([text](guide/TESTING.md#section),
   [text](#section)) must match a heading of the target file, GitHub-style
   slugs included (the -1, -2 … disambiguation suffixes are accepted;
   #L10 source-line anchors are not checked).

3. A section citation — `lisa.md §6.1` in prose, or a link trailed by
   `§3.2` — must name a section number the target file carries (a `## 3.`
   / `### 3.2` heading).  Section numbers are a stable API (docs/README.md,
   rule R5): renumbering a section without fixing inbound citations fails
   here.  A bare `lisa.md` citation resolves against the citing file's
   directory, then by unique basename among tracked Markdown.

External URLs, mailto:, and links and citations inside code spans or fenced
code blocks are not checked; those are examples, not links.

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
# a doc#anchor link — a source-line anchor is not a heading anchor
SRC_LINE = re.compile(r"#L\d+$")
HEADING = re.compile(r"^(#{1,6})\s+(.*?)\s*$")
NUM_SECTION = re.compile(r"^#{2,6}\s+(\d+(?:\.\d+)*)(?:[.\s]|$)")
# §N citations: adjacent prose, and a markdown link trailed by one
CITE_ADJ = re.compile(r"([\w./-]+\.md)\s*§(\d+(?:\.\d+)*)")
CITE_LINK = re.compile(r"\]\(([^)#]+\.md)(?:#[^)]*)?\)\s*§(\d+(?:\.\d+)*)")

_cache = {}


def tracked_md():
    out = subprocess.run(["git", "ls-files", "--", "*.md"], capture_output=True, text=True, check=True)
    return out.stdout.split()


def read(path):
    if path not in _cache:
        try:
            with open(path, encoding="utf-8", errors="replace") as f:
                _cache[path] = f.read()
        except OSError:
            _cache[path] = None
    return _cache[path]


def slugify(text):
    # GitHub's heading anchor: lowercase, drop non-word characters (spaces
    # kept), then spaces to hyphens.
    s = text.strip().lower()
    s = re.sub(r"[^\w\- ]", "", s)
    return s.replace(" ", "-")


def heading_slugs(text):
    slugs = set()
    for line in text.splitlines():
        m = HEADING.match(line)
        if m:
            slugs.add(slugify(m.group(2)))
    return slugs


def section_numbers(text):
    nums = set()
    for line in text.splitlines():
        m = NUM_SECTION.match(line)
        if m:
            nums.add(m.group(1))
    return nums


def resolve_md(name, base):
    # A citation's file: relative to the citing file, else a unique basename
    # among tracked Markdown (so "lisa.md §6.1" resolves from anywhere).
    cand = os.path.normpath(os.path.join(base, name))
    if os.path.isfile(cand):
        return cand
    base_name = os.path.basename(name)
    hits = [p for p in tracked_md() if os.path.basename(p) == base_name]
    if len(hits) == 1:
        return hits[0]
    return None


def broken_links(path):
    base = os.path.dirname(path)
    in_fence = False
    own_slugs = heading_slugs(read(path) or "")
    with open(path, encoding="utf-8", errors="replace") as f:
        for n, line in enumerate(f, 1):
            if FENCE.match(line):
                in_fence = not in_fence
                continue
            if in_fence:
                continue
            plain = CODE_SPAN.sub("", line)
            for m in LINK.finditer(plain):
                target = m.group(1)
                if SKIP.match(target):
                    continue
                file_part, _, anchor = target.partition("#")
                anchor = anchor.split("?", 1)[0]
                if file_part:
                    full = os.path.normpath(os.path.join(base, file_part))
                    if not os.path.exists(full):
                        yield n, f"missing file: {target}"
                        continue
                    # a heading anchor must match a heading of the target
                    if anchor and file_part.endswith(".md") and not SRC_LINE.search(target):
                        text = read(full)
                        if text is not None and anchor not in heading_slugs(text):
                            yield n, f"missing heading: {target}"
                elif anchor and not SRC_LINE.search("#" + anchor):
                    # same-page anchor, checked against this file's headings
                    if anchor not in own_slugs:
                        yield n, f"missing heading: {target}"
            # a § citation must name a section the target file carries
            for m in CITE_LINK.finditer(plain):
                yield from check_cite(path, base, n, m.group(1), m.group(2))
            for m in CITE_ADJ.finditer(plain):
                yield from check_cite(path, base, n, m.group(1), m.group(2))


def check_cite(path, base, n, name, num):
    src = resolve_md(name, base)
    if src is None:
        yield n, f"citation of unknown file: {name} §{num}"
        return
    text = read(src)
    if text is None or num not in section_numbers(text):
        yield n, f"citation: {name} has no section §{num}"


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
