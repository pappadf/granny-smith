#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) pappadf
"""Regenerate tests/e2e/e2e-weights.json from CI's e2e JSON reports.

Usage: gen-e2e-weights.py [--source TEXT] REPORT.json... > tests/e2e/e2e-weights.json

The reports are the web2-e2e-report-shard-* artifacts (parallel.json,
serial.json per shard).  For each spec file: `secs`, the sum of its tests'
durations, and `max_test`, its longest test.  With several runs a file's
figures are the median over the runs that ran it.  Files the reports do not
mention keep their old weights.  scripts/e2e-shard.py reads the result.
"""

import json
import statistics
import sys

WEIGHTS = "tests/e2e/e2e-weights.json"


def walk(suite, file, out):
    # Accumulate each test's duration under the spec file it lives in
    f = suite.get("file", file)
    for spec in suite.get("specs", []):
        for t in spec.get("tests", []):
            d = sum(r.get("duration", 0) for r in t.get("results", [])) / 1000.0
            out.setdefault(f, []).append(d)
    for child in suite.get("suites", []):
        walk(child, f, out)


def main(argv):
    args = argv[1:]
    source = None
    if len(args) >= 2 and args[0] == "--source":
        source, args = args[1], args[2:]
    try:
        with open(WEIGHTS) as fh:
            old = json.load(fh)
    except FileNotFoundError:
        old = {"files": {}}
    runs = {}
    for path in args:
        with open(path) as fh:
            report = json.load(fh)
        tests = {}
        for s in report.get("suites", []):
            walk(s, None, tests)
        for f, durs in tests.items():
            runs.setdefault("web2-specs/" + f.split("/")[-1], []).append((sum(durs), max(durs)))
    files = dict(old.get("files", {}))
    for f, samples in runs.items():
        files[f] = {
            "secs": int(round(statistics.median(s for s, _ in samples))),
            "max_test": int(round(statistics.median(m for _, m in samples))),
        }
    out = {
        "_comment": "Per web2 spec file: the seconds its tests took (secs) and its longest test "
        "(max_test), from CI's e2e JSON reports. scripts/e2e-shard.py packs the files across the "
        "ui-e2e runners with it. Regenerate with scripts/gen-e2e-weights.py; only the balance "
        "depends on it, never which specs run.",
        "source": source or ("gen-e2e-weights.py over " + ", ".join(args)),
        "files": dict(sorted(files.items())),
    }
    json.dump(out, sys.stdout, indent=2)
    sys.stdout.write("\n")


if __name__ == "__main__":
    main(sys.argv)
