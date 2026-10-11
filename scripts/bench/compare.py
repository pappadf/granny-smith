#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) pappadf
"""compare.py -- the CPU benchmarks' verdict (docs/guide/BENCHMARKS.md).

Reads the raw JSON lines scripts/bench/run.py wrote and produces:

  * a Markdown report: one table per metric (rows x executors: base, head,
    delta, verdict, guard), appended to --summary (the step summary in CI)
    and printed;
  * bench.json (--json): the same numbers, machine-readable, for the artifact
    and the bench-data history;
  * the exit status: 1 when metric A regressed past its threshold on any
    valid row and executor, unless --accept (the PR's perf-accepted label).

Metric B only ever warns, and C is reported.  Thresholds come from
tests/bench/rows.json ("thresholds"), overridable on the command line.

Usage: compare.py RAW.jsonl [--summary FILE] [--json FILE] [--accept]
"""

import argparse
import json
import math
import os
import random
import statistics
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
ROWS_JSON = os.path.join(ROOT, "tests", "bench", "rows.json")


def load(path):
    recs = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line.startswith("@@BENCH "):
                line = line[8:]
            if line.startswith("{"):
                recs.append(json.loads(line))
    return recs


def thresholds(args):
    t = {"a_fail": 0.015, "b_warn": 0.03, "calib_spread": 0.05, "b_confidence": 0.95}
    try:
        with open(ROWS_JSON) as f:
            t.update(json.load(f).get("thresholds", {}))
    except (OSError, ValueError):
        pass
    for k in ("a_fail", "b_warn", "calib_spread"):
        v = getattr(args, k)
        if v is not None:
            t[k] = v
    return t


# ---- statistics for B ---------------------------------------------------------


def hodges_lehmann_log_ratio(h, b):
    """The Hodges-Lehmann estimate of log(head/base): the median of every
    pairwise log ratio.  Robust to one wild round on either side."""
    return statistics.median(math.log(x / y) for x in h for y in b)


def bootstrap_ci(h, b, conf, n=4000, seed=1):
    """A percentile-bootstrap interval for median(h)/median(b), resampling the
    rounds of each binary.  Seeded: the same data gives the same interval."""
    rng = random.Random(seed)
    ratios = []
    for _ in range(n):
        hs = [rng.choice(h) for _ in h]
        bs = [rng.choice(b) for _ in b]
        ratios.append(statistics.median(hs) / statistics.median(bs))
    ratios.sort()
    lo = ratios[int((1 - conf) / 2 * n)]
    hi = ratios[min(n - 1, int((1 + conf) / 2 * n))]
    return lo, hi


def cv(vals):
    return statistics.pstdev(vals) / statistics.mean(vals) if len(vals) > 1 and statistics.mean(vals) else 0.0


# ---- the report -----------------------------------------------------------------


def pct(x):
    return "%+.2f %%" % (100 * x)


def analyse(recs, t, accept):
    meta = next((r for r in recs if r.get("metric") == "meta"), {})
    rows = meta.get("rows", [])
    execs = meta.get("executors", ["switch"])
    has_base = "base" in meta.get("binaries", {})
    res = {"meta": meta, "thresholds": t, "accepted": accept, "A": [], "B": [], "C": [],
           "boot": [], "ckpt": [], "calib": [], "failures": [], "warnings": []}

    for r in recs:
        if r.get("metric") == "boot":
            res["boot"].append({k: r.get(k) for k in ("row", "binary", "ok", "secs", "guest_instr")})
            if not r.get("ok"):
                res["warnings"].append("%s: the %s build could not boot the row (see %s)" % (
                    r["row"], r["binary"], r.get("log")))
        elif r.get("metric") == "ckpt":
            res["ckpt"].append({"row": r["row"], "same": r["same"], "diffs": r.get("diffs", [])})
        elif r.get("metric") == "calib":
            res["calib"].append(r)

    def find(metric, row, ex, binary):
        return [r for r in recs if r.get("metric") == metric and r.get("row") == row
                and r.get("exec") == ex and r.get("binary") == binary]

    # A (and A2).
    for row in rows:
        for ex in execs:
            h = find("A", row, ex, "head")
            b = find("A", row, ex, "base")
            if not h and not b:
                continue
            h = h[0] if h else None
            b = b[0] if b else None
            e = {"row": row, "exec": ex,
                 "head": h.get("value") if h and h.get("valid") else None,
                 "base": b.get("value") if b and b.get("valid") else None,
                 "slowpath_frac": (h or b or {}).get("slowpath_frac"),
                 "guest_instr": {"head": h and h.get("guest_instr"), "base": b and b.get("guest_instr")},
                 "a2": {"head": h and h.get("a2"), "base": b and b.get("a2")}}
            bad = [x.get("invalid") for x in (h, b) if x is not None and not x.get("valid")]
            if bad:
                e["verdict"] = "invalid"
                e["guard"] = "; ".join(bad)
                res["warnings"].append("A %s/%s invalid: %s" % (row, ex, e["guard"]))
            elif e["base"] is None and has_base:
                e["verdict"] = "new"
            elif e["base"] is None:
                e["verdict"] = "-"
            else:
                d = e["head"] / e["base"] - 1
                e["delta"] = d
                if d > t["a_fail"]:
                    e["verdict"] = "accepted" if accept else "FAIL"
                    if not accept:
                        res["failures"].append("A %s/%s: %s host instructions per guest instruction "
                                               "(%.3f -> %.3f, threshold %s)" % (
                                                   row, ex, pct(d), e["base"], e["head"], pct(t["a_fail"])))
                elif d < -0.001:
                    e["verdict"] = "improved"
                else:
                    e["verdict"] = "ok"
            e.setdefault("guard", "ok")
            res["A"].append(e)

    # B.
    for row in rows:
        for ex in execs:
            h = find("B", row, ex, "head")
            b = find("B", row, ex, "base")
            if not h:
                continue
            hv = [x["value"] for x in h if x.get("valid")]
            bv = [x["value"] for x in b if x.get("valid")]
            bad = sorted({x.get("invalid") for x in h + b if not x.get("valid")})
            e = {"row": row, "exec": ex, "rounds": {"head": hv, "base": bv},
                 "head": statistics.median(hv) if hv else None,
                 "base": statistics.median(bv) if bv else None,
                 "cv": {"head": cv(hv), "base": cv(bv)}, "guard": "; ".join(bad) or "ok"}
            if hv and bv and len(hv) >= 2 and len(bv) >= 2:
                e["ratio"] = e["head"] / e["base"]
                e["hl_ratio"] = math.exp(hodges_lehmann_log_ratio(hv, bv))
                e["ci"] = bootstrap_ci(hv, bv, t["b_confidence"])
                if e["ci"][1] < 1 - t["b_warn"]:
                    e["verdict"] = "WARN"
                    res["warnings"].append("B %s/%s: head is %.1f %% slower (CI %.3f-%.3f)" % (
                        row, ex, 100 * (1 - e["ratio"]), e["ci"][0], e["ci"][1]))
                elif e["ci"][0] > 1 + t["b_warn"]:
                    e["verdict"] = "faster"
                else:
                    e["verdict"] = "ok"
            else:
                e["verdict"] = "invalid" if bad else "-"
            res["B"].append(e)

    # C: head's MIPS over the calibration score; the run is noisy when the
    # score moved by more than calib_spread between before and after.
    scores = [c["score"] for c in res["calib"] if c.get("score")]
    if scores:
        score = statistics.mean(scores)
        spread = (max(scores) - min(scores)) / score if len(scores) > 1 else 0.0
        noisy = spread > t["calib_spread"]
        res["calib_summary"] = {"score": score, "spread": spread, "noisy": noisy,
                                "calib_v": res["calib"][0].get("calib_v"), "cpu": meta.get("cpu")}
        if noisy:
            res["warnings"].append("calibration moved %.1f %% during the run: C values are noisy" % (100 * spread))
        # Concurrent pairing runs head and base at once: the runner is shared,
        # so the MIPS are fair to compare with each other but not absolute.
        if meta.get("pairing") == "concurrent" and has_base:
            noisy = res["calib_summary"]["noisy"] = True
            res["warnings"].append("C from a concurrent-pairing run: head and base shared the runner, "
                                   "so its MIPS are not absolute (C is for head-only runs)")
        for e in res["B"]:
            if e["head"] is not None:
                res["C"].append({"row": e["row"], "exec": e["exec"], "mips": e["head"],
                                 "value": e["head"] / score, "noisy": noisy,
                                 "cpu": meta.get("cpu"), "calib_v": res["calib_summary"]["calib_v"]})
    return res


def fmt(x, nd=3):
    return "-" if x is None else ("%.*f" % (nd, x))


def report(res):
    t = res["thresholds"]
    out = ["## CPU benchmarks", ""]
    meta = res["meta"]
    out.append("Runner CPU: `%s` · executors: %s · rows: %s" % (
        meta.get("cpu", "?"), ", ".join(meta.get("executors", [])), ", ".join(meta.get("rows", []))))
    out.append("")
    if res["failures"]:
        out.append("**Metric A regressed** (threshold %s; a deliberate trade-off carries the "
                   "`perf-accepted` label):" % pct(t["a_fail"]))
        out += ["- " + f for f in res["failures"]]
        out.append("")
    elif res["accepted"] and any(e["verdict"] == "accepted" for e in res["A"]):
        out.append("**Metric A regressions accepted** (`perf-accepted`): see the table.")
        out.append("")

    if res["A"]:
        out += ["### A — host instructions per guest instruction (callgrind, deterministic)", "",
                "| Row | Executor | Base | Head | Δ | Verdict | Slow path | Guard |",
                "|---|---|---:|---:|---:|---|---:|---|"]
        for e in res["A"]:
            sp = e.get("slowpath_frac")
            out.append("| %s | %s | %s | %s | %s | %s | %s | %s |" % (
                e["row"], e["exec"], fmt(e["base"]), fmt(e["head"]),
                pct(e["delta"]) if "delta" in e else "-", e["verdict"],
                "-" if sp is None else "%.2f %%" % (100 * sp), e["guard"]))
        out.append("")
        a2 = [e for e in res["A"] if e["a2"]["head"]]
        if a2:
            out += ["#### A2 — simulated cache misses and mispredicts per guest instruction (report only)", "",
                    "| Row | Executor | L1 miss base → head | LL miss base → head | Mispredict base → head |",
                    "|---|---|---|---|---|"]
            for e in a2:
                h, b = e["a2"]["head"], e["a2"]["base"] or {}
                out.append("| %s | %s | %s → %s | %s → %s | %s → %s |" % (
                    e["row"], e["exec"], fmt(b.get("l1_miss"), 4), fmt(h["l1_miss"], 4),
                    fmt(b.get("ll_miss"), 5), fmt(h["ll_miss"], 5),
                    fmt(b.get("mispredict"), 4), fmt(h["mispredict"], 4)))
            out.append("")

    if res["B"]:
        out += ["### B — paired wall-clock MIPS, head vs base on one runner (warn only)", "",
                "| Row | Executor | Base MIPS | Head MIPS | Ratio | %d %% CI | CV base/head | Verdict | Guard |"
                % int(100 * t["b_confidence"]),
                "|---|---|---:|---:|---:|---|---|---|---|"]
        for e in res["B"]:
            ci = e.get("ci")
            out.append("| %s | %s | %s | %s | %s | %s | %.1f / %.1f %% | %s | %s |" % (
                e["row"], e["exec"], fmt(e["base"], 1), fmt(e["head"], 1), fmt(e.get("ratio")),
                "%.3f–%.3f" % ci if ci else "-", 100 * e["cv"]["base"], 100 * e["cv"]["head"],
                e["verdict"], e["guard"]))
        out.append("")

    if res["C"]:
        cs = res["calib_summary"]
        out += ["### C — MIPS per calibration score (trend)", "",
                "Calibration v%s: score %.3f, spread %.1f %%%s" % (
                    cs["calib_v"], cs["score"], 100 * cs["spread"], " — **noisy**" if cs["noisy"] else ""), "",
                "| Row | Executor | MIPS | C |", "|---|---|---:|---:|"]
        for e in res["C"]:
            out.append("| %s | %s | %.1f | %.1f |" % (e["row"], e["exec"], e["mips"], e["value"]))
        out.append("")

    if res["ckpt"]:
        diff = [c for c in res["ckpt"] if not c["same"]]
        out.append("### Guest state at the checkpoints")
        out.append("")
        if not diff:
            out.append("Head and base reach the same guest state on every row.")
        else:
            out.append("The guest state differs between head and base (the PR changes guest behaviour "
                       "or timing; A stays valid per guest instruction):")
            for c in diff:
                out.append("- %s: %s" % (c["row"], "; ".join(c["diffs"][:3]) or "differs"))
        out.append("")

    if res["warnings"]:
        out.append("### Warnings")
        out.append("")
        out += ["- " + w for w in res["warnings"]]
        out.append("")
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("raw", help="raw JSON lines from run.py")
    ap.add_argument("--summary", help="append the Markdown report here (e.g. $GITHUB_STEP_SUMMARY)")
    ap.add_argument("--json", help="write bench.json here")
    ap.add_argument("--accept", action="store_true", help="metric-A regressions are accepted (perf-accepted)")
    ap.add_argument("--a-fail", dest="a_fail", type=float, help="metric A failure threshold (fraction)")
    ap.add_argument("--b-warn", dest="b_warn", type=float, help="metric B warning threshold (fraction)")
    ap.add_argument("--calib-spread", dest="calib_spread", type=float, help="calibration noise threshold")
    args = ap.parse_args()

    res = analyse(load(args.raw), thresholds(args), args.accept)
    text = report(res)
    print(text)
    if args.summary:
        with open(args.summary, "a") as f:
            f.write(text + "\n")
    if args.json:
        with open(args.json, "w") as f:
            json.dump(res, f, indent=1, sort_keys=True)
    sys.exit(1 if res["failures"] else 0)


if __name__ == "__main__":
    main()
