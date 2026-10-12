#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) pappadf
"""Split the web2 e2e spec files across CI's runners by measured duration.

Usage: e2e-shard.py K/N PROJECT        (PROJECT is parallel or serial)

Prints the spec files (relative to tests/e2e) shard K of N runs in that
project, one per line; nothing if it runs none.  Every runner computes the
same partition from the same inputs, so the shards cover every spec once.

Playwright's own --shard splits the files by count, which put the four
real-time AV specs (264 s of serial work) on one runner while another got
7 s.  This packs them by duration instead, from tests/e2e/e2e-weights.json
(per spec file: the seconds its tests took and its longest test, written by
scripts/gen-e2e-weights.py from a CI run's JSON reports).  A shard costs its
serial specs (one at a time, after the parallel pass) plus its parallel
pass: the parallel specs' seconds over the workers, or the longest single
file if that is longer -- a file runs on one worker unless it configures
`mode: 'parallel'`, in which case only its longest test is indivisible.
Specs go heaviest first to the shard that is cheapest with them added (LPT).  A spec
with no recorded weight counts as the median.
"""

import json
import pathlib
import re
import statistics
import sys

E2E = pathlib.Path(__file__).resolve().parent.parent / "tests" / "e2e"
CONFIG = E2E / "playwright.web2.config.ts"
WEIGHTS = E2E / "e2e-weights.json"
SPECS = E2E / "web2-specs"
WORKERS = 3  # GS_E2E_WORKERS in the workflow


def serial_specs():
    # The config's SERIAL list: the specs that run in the `serial` project
    text = CONFIG.read_text()
    block = re.search(r"const SERIAL = \[(.*?)\];", text, re.S).group(1)
    return set(re.findall(r"'([^']+\.spec\.ts)'", block))


def main(argv):
    if len(argv) != 3 or "/" not in argv[1] or argv[2] not in ("parallel", "serial"):
        sys.exit(__doc__)
    k, n = (int(x) for x in argv[1].split("/"))
    serial = serial_specs()
    weights = json.loads(WEIGHTS.read_text())["files"] if WEIGHTS.exists() else {}
    known = [w["secs"] for w in weights.values()] or [30]
    default = statistics.median(known)

    specs = []
    for path in sorted(SPECS.rglob("*.spec.ts")):
        rel = str(path.relative_to(E2E))
        w = weights.get(rel, {})
        secs = w.get("secs", default)
        # Whole file on one worker, unless it runs its tests side by side
        split = re.search(r"describe\.configure\(\{\s*mode:\s*['\"]parallel", path.read_text())
        unit = w.get("max_test", secs) if split else secs
        specs.append((rel, path.name in serial, secs, unit))

    shards = [{"serial": 0.0, "par": 0.0, "unit": 0.0, "files": []} for _ in range(n)]

    def cost(s):
        return s["serial"] + max(s["par"] / WORKERS, s["unit"])

    for rel, is_serial, secs, unit in sorted(specs, key=lambda x: (-x[2], x[0])):
        def after(s):
            # The shard's cost with this spec added
            t = dict(s)
            if is_serial:
                t["serial"] += secs
            else:
                t["par"] += secs
                t["unit"] = max(t["unit"], unit)
            return cost(t)

        best = min(range(n), key=lambda i: (after(shards[i]), i))
        s = shards[best]
        if is_serial:
            s["serial"] += secs
        else:
            s["par"] += secs
            s["unit"] = max(s["unit"], unit)
        s["files"].append((rel, is_serial))

    for rel, is_serial in sorted(shards[k - 1]["files"]):
        if is_serial == (argv[2] == "serial"):
            print(rel)
    for i, s in enumerate(shards, 1):
        print(f"shard {i}/{n}: ~{cost(s):.0f} s ({s['serial']:.0f} s serial, "
              f"{s['par']:.0f} s parallel over {WORKERS} workers)", file=sys.stderr)


if __name__ == "__main__":
    main(sys.argv)
