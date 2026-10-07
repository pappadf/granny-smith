#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) pappadf
"""Regenerate tests/integration/test-weights.json from runner durations.

Usage: gen-test-weights.py DURATIONS.jsonl... > tests/integration/test-weights.json

Each input line is one record the integration runner appends
({"test": "suite-av", "secs": 700, "status": "PASS"}); several files (CI
shards, several runs) are merged and each test's weight is the median of
its records.  Tests already in the committed file but absent from the
inputs keep their old weight, so a partial run (one tier, one shard) never
drops a test back to "unknown".
"""

import json
import statistics
import sys

WEIGHTS = "tests/integration/test-weights.json"


def main(argv):
    try:
        with open(WEIGHTS) as f:
            old = json.load(f)
    except FileNotFoundError:
        old = {"weights": {}}
    samples = {}
    for path in argv[1:]:
        with open(path) as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                rec = json.loads(line)
                samples.setdefault(rec["test"], []).append(rec["secs"])
    weights = dict(old.get("weights", {}))
    for test, secs in samples.items():
        weights[test] = int(round(statistics.median(secs)))
    out = {k: v for k, v in old.items() if k != "weights"}
    out["source"] = "gen-test-weights.py over " + ", ".join(argv[1:])
    out["weights"] = dict(sorted(weights.items()))
    json.dump(out, sys.stdout, indent=2)
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
