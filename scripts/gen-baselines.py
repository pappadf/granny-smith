#!/usr/bin/env python3
"""Seed perf-baselines.json from a suite run log.

Reads the @@PERF records suite rows emit (proposal-integration-test-rework
§5.8) from one or more test logs and (re)writes tests/integration/
perf-baselines.json. Rows not present in the logs are preserved, so
suites can be added incrementally:

    python3 scripts/gen-baselines.py tmp/sq-full.log

A baseline legitimately records observed behaviour — the gate is drift
(±tolerance), so regenerating it is the intended workflow: run this,
review the git diff, and commit it in the PR that legitimately changed
guest timing.

This script does NOT write matrix-targets.json. That file is the declared
coverage contract, hand-authored from §7; generating it from a run would
make it agree with whatever the run happened to cover, which is exactly
the check it exists to perform. Verify coverage instead with:

    python3 scripts/test-matrix.py --check <run logs>
"""

import argparse
import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
BASELINES = ROOT / "tests/integration/perf-baselines.json"
DEFAULT_TOLERANCE = 0.20


def load(path, key):
    if path.exists():
        return json.loads(path.read_text())
    return {key: {} if key == "rows" else []}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("logs", nargs="+", help="test log files containing @@PERF/@@COV lines")
    ap.add_argument("--suite", help="unused; kept so existing invocations do not break")
    args = ap.parse_args()

    perfs = {}
    for logf in args.logs:
        for line in pathlib.Path(logf).read_text(errors="replace").splitlines():
            m = re.search(r"@@(PERF|COV) (\{.*\})", line)
            if not m:
                continue
            rec = json.loads(m.group(2))
            if m.group(1) == "PERF":
                perfs[rec["row"]] = rec["instr"]

    baselines = load(BASELINES, "rows")
    for row, instr in sorted(perfs.items()):
        prev = baselines["rows"].get(row, {})
        baselines["rows"][row] = {
            "instr": instr,
            "tolerance": prev.get("tolerance", DEFAULT_TOLERANCE),
        }
    BASELINES.write_text(json.dumps(baselines, indent=2, sort_keys=True) + "\n")

    print(f"perf-baselines.json: {len(baselines['rows'])} row(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
