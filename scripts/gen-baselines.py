#!/usr/bin/env python3
"""Seed perf-baselines.json and matrix-targets.json from a suite run log.

Reads @@PERF and @@COV records (proposal-integration-test-rework §5.6,
§5.8) from one or more test logs and (re)writes the two committed JSON
files. Existing entries for rows/cells not present in the logs are
preserved, so suites can be added incrementally:

    python3 scripts/gen-baselines.py --suite suite-quadra tmp/sq-full.log

Baselines update deliberately: run this, review the git diff, commit it
in the PR that legitimately changed guest timing.
"""

import argparse
import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
BASELINES = ROOT / "tests/integration/perf-baselines.json"
TARGETS = ROOT / "tests/integration/matrix-targets.json"
DEFAULT_TOLERANCE = 0.20


def load(path, key):
    if path.exists():
        return json.loads(path.read_text())
    return {key: {} if key == "rows" else []}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("logs", nargs="+", help="test log files containing @@PERF/@@COV lines")
    ap.add_argument("--suite", required=True, help="suite name owning the @@COV cells")
    args = ap.parse_args()

    perfs, covs = {}, []
    for logf in args.logs:
        for line in pathlib.Path(logf).read_text(errors="replace").splitlines():
            m = re.search(r"@@(PERF|COV) (\{.*\})", line)
            if not m:
                continue
            rec = json.loads(m.group(2))
            if m.group(1) == "PERF":
                perfs[rec["row"]] = rec["instr"]
            else:
                covs.append(rec)

    baselines = load(BASELINES, "rows")
    for row, instr in sorted(perfs.items()):
        prev = baselines["rows"].get(row, {})
        baselines["rows"][row] = {
            "instr": instr,
            "tolerance": prev.get("tolerance", DEFAULT_TOLERANCE),
        }
    BASELINES.write_text(json.dumps(baselines, indent=2, sort_keys=True) + "\n")

    targets = load(TARGETS, "cells")
    # Replace this suite's cells wholesale; other suites' cells survive.
    cells = [c for c in targets["cells"] if c.get("suite") != args.suite]
    seen = set()
    for rec in covs:
        cell = {
            "machine": rec["machine"],
            "system": rec["system"],
            "card": rec["card"],
            "width": rec["width"],
            "height": rec["height"],
            "addr32": rec["addr32"],
            "suite": args.suite,
        }
        key = json.dumps(cell, sort_keys=True)
        if key not in seen:
            seen.add(key)
            cells.append(cell)
    targets["cells"] = sorted(cells, key=lambda c: (c["suite"], c["machine"], c["system"]))
    TARGETS.write_text(json.dumps(targets, indent=2, sort_keys=True) + "\n")

    print(f"perf-baselines.json: {len(baselines['rows'])} row(s)")
    print(f"matrix-targets.json: {len(targets['cells'])} cell(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
