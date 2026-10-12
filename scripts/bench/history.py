#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) pappadf
"""history.py -- one JSON line per row, executor and metric from a bench.json
(scripts/bench/compare.py --json), for the bench-data branch's history.jsonl.

Each point carries what a trend line is keyed by: the runner's CPU model and
the calibration version (metric C is comparable only within both), and the
noise flag (a C point from a run whose calibration moved is kept but left
out of trend lines).

Usage: history.py bench.json --sha SHA [--ref REF] [--run RUN_ID] > points.jsonl
"""

import argparse
import datetime
import json


def points(res, stamp):
    cpu = res.get("meta", {}).get("cpu")
    calib = res.get("calib_summary") or {}
    for e in res.get("A", []):
        if e.get("head") is not None:
            yield dict(stamp, metric="A", row=e["row"], exec=e["exec"], value=e["head"], cpu=cpu)
            a2 = (e.get("a2") or {}).get("head")
            if a2:
                yield dict(stamp, metric="A2", row=e["row"], exec=e["exec"], value=a2, cpu=cpu)
    for e in res.get("B", []):
        if e.get("head") is not None:
            yield dict(stamp, metric="MIPS", row=e["row"], exec=e["exec"], value=e["head"], cpu=cpu)
    for e in res.get("C", []):
        yield dict(stamp, metric="C", row=e["row"], exec=e["exec"], value=e["value"], cpu=cpu,
                   calib_v=e.get("calib_v"), score=calib.get("score"), noisy=e.get("noisy", False))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("bench_json")
    ap.add_argument("--sha", required=True)
    ap.add_argument("--ref", default="main")
    ap.add_argument("--run", default="")
    args = ap.parse_args()
    with open(args.bench_json) as f:
        res = json.load(f)
    stamp = {"date": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
             "sha": args.sha, "ref": args.ref, "run": args.run}
    for p in points(res, stamp):
        print(json.dumps(p, sort_keys=True))


if __name__ == "__main__":
    main()
