#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) pappadf
"""run.py -- the CPU benchmark harness (docs/guide/BENCHMARKS.md).

Runs the rows of tests/bench/rows.json against one or two gs-headless
binaries (the PR's head and its merge base) and prints one @@BENCH JSON line
per measurement; scripts/bench/compare.py turns them into the verdict.

  Boot     each binary boots each row once and saves a checkpoint (the
           checkpoint format refuses a file from another build, so every
           binary needs its own).  The two checkpoints are compared for guest
           state as a side benefit (ckptdiff.py).
  A        host instructions per guest instruction under callgrind, the
           difference of two windows after the checkpoint, collected only
           inside the CPU's run function.  Deterministic.  With A2 the same
           runs also simulate caches and branch prediction.
  B        native MIPS of head and base, at the same time on two pinned
           cores (or alternated on one), rounds of each; compare.py takes the
           ratio of medians and its confidence interval.
  C        gs-headless --calibrate before and after the measurements; head's
           MIPS over the calibration score is the runner-normalized trend
           value.

Usage:
  run.py --head build/headless/gs-headless [--base base/gs-headless]
         [--rows pr|all|a,b,...] [--metrics A,B] [--work tmp/bench]
         [--out tmp/bench/raw.jsonl]
"""

import argparse
import concurrent.futures
import json
import os
import re
import shutil
import statistics
import subprocess
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
BENCH_DIR = os.path.join(ROOT, "tests", "bench")
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ckptdiff  # noqa: E402

# Every emulator run: deterministic pacing, no job thread and no I/O worker.
# Under Valgrind the threads are serialized, and a job thread polling for the
# emulator costs 15x the run itself; natively it removes a source of jitter.
EMU_FLAGS = ["--speed=turbo", "--jobs=inline", "--io=sync"]

CALLGRIND = ["valgrind", "--tool=callgrind"]


def log(msg):
    print(msg, file=sys.stderr, flush=True)


def emit(out, rec):
    """One measurement: an @@BENCH line on stdout and a line in the raw file."""
    line = json.dumps(rec, sort_keys=True)
    print("@@BENCH " + line, flush=True)
    out.write(line + "\n")
    out.flush()


def load_rows(path):
    with open(path) as f:
        doc = json.load(f)
    rows = {}
    for name, r in doc["rows"].items():
        merged = dict(doc["defaults"])
        merged.update(r)
        merged["name"] = name
        rows[name] = merged
    return rows, doc.get("thresholds", {})


def cpu_model():
    try:
        with open("/proc/cpuinfo") as f:
            for line in f:
                if line.startswith("model name"):
                    return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return "unknown"


def parse_tagged(text, tag):
    """The JSON payloads of every `tag {...}` line in emulator output."""
    out = []
    for line in text.splitlines():
        i = line.find(tag + " ")
        if i >= 0:
            try:
                out.append(json.loads(line[i + len(tag) + 1 :]))
            except ValueError:
                pass
    return out


class Binary:
    """One gs-headless under test: its label (head/base), path, executors."""

    def __init__(self, label, path, work):
        self.label = label
        self.path = os.path.abspath(path)
        self.work = os.path.join(work, label)
        self.executors = ["switch"]
        self.has_predecode = False

    def probe(self, rom, test_data):
        """Which executors this build has: the predecoded one is a run-time
        switch (predecode.enabled) where the build carries it."""
        d = os.path.join(self.work, "probe")
        os.makedirs(d, exist_ok=True)
        script = os.path.join(d, "probe.script")
        with open(script, "w") as f:
            f.write('echo "@@PROBE {\\"predecode\\":${try(predecode.enabled, -1)}}"\nquit\n')
        p = subprocess.run(
            [self.path, "rom=" + os.path.join(test_data, rom), "script=" + script] + EMU_FLAGS,
            cwd=d, capture_output=True, text=True, timeout=120,
        )
        probes = parse_tagged(p.stdout, "@@PROBE")
        self.has_predecode = bool(probes) and probes[0].get("predecode", -1) >= 0
        self.executors = ["switch", "predecode"] if self.has_predecode else ["switch"]


def exec_setup(binary, executor):
    """Driver lines that select `executor` on `binary`."""
    if not binary.has_predecode:
        return ""
    return "predecode.enabled = %d\n" % (1 if executor == "predecode" else 0)


def driver_prologue(row):
    return (
        'include "%s"\n' % os.path.join(ROOT, "tests", "integration", "lib", "mac.script")
        + 'include "%s"\n' % os.path.join(BENCH_DIR, "lib", "bench.script")
        + 'include "%s"\n' % os.path.join(BENCH_DIR, row["name"] + ".script")
    )


def emu_cmd(binary, row, script, test_data):
    return [
        binary.path,
        "rom=" + os.path.join(test_data, row["rom"]),
        "model=" + row["model"],
        "ram=%d" % row["ram"],
        "script=" + script,
        "--var", "TEST_DATA=" + test_data,
        "--var", "BENCH_DIR=" + BENCH_DIR,
    ] + EMU_FLAGS


def run_dir(binary, row, name):
    """A fresh directory for one run, at a path of the same length for head
    and base (the labels are both four letters), with its own storage cache:
    nothing a run writes is shared with another run."""
    d = os.path.join(binary.work, row["name"], name)
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    return d


def run_emu(cmd, d, timeout, wrapper=None):
    env = dict(os.environ)
    env["GS_STORAGE_CACHE"] = os.path.join(d, "storage-cache")
    full = (wrapper or []) + cmd
    t0 = time.time()
    p = subprocess.run(full, cwd=d, capture_output=True, text=True, timeout=timeout, env=env)
    with open(os.path.join(d, "output.log"), "w") as f:
        f.write("$ " + " ".join(full) + "\n" + p.stdout + p.stderr)
    return p, time.time() - t0


# ---- boot -----------------------------------------------------------------


def checkpoint_path(binary, row):
    return os.path.join(binary.work, row["name"], "state.gsc")


def boot_row(binary, row, test_data):
    """Boot `row` on `binary` to its state and save the checkpoint."""
    d = run_dir(binary, row, "boot")
    ck = checkpoint_path(binary, row)
    script = os.path.join(d, "boot.script")
    with open(script, "w") as f:
        f.write(driver_prologue(row))
        f.write("bench_boot()\n")
        f.write('assert checkpoint.save("%s")\n' % ck)
        f.write('echo "@@BOOTED {\\"guest_instr\\":${scheduler.instr_count}}"\n')
        f.write("quit\n")
    p, secs = run_emu(emu_cmd(binary, row, script, test_data), d, 1800)
    booted = parse_tagged(p.stdout, "@@BOOTED")
    ok = p.returncode == 0 and bool(booted) and os.path.exists(ck)
    return {
        "ok": ok,
        "secs": round(secs, 1),
        "guest_instr": booted[0]["guest_instr"] if booted else None,
        "log": os.path.join(d, "output.log"),
    }


# ---- one measured window ---------------------------------------------------


def window_script(d, binary, row, executor, warm, n):
    script = os.path.join(d, "measure.script")
    with open(script, "w") as f:
        f.write(driver_prologue(row))
        f.write('assert checkpoint.load("%s")\n' % checkpoint_path(binary, row))
        f.write(exec_setup(binary, executor))
        if warm:
            f.write("scheduler.run %d\n" % warm)
        f.write("bench_window(%d)\n" % n)
        f.write("bench_check()\n")
        f.write("quit\n")
    return script


def window_result(p):
    wins = parse_tagged(p.stdout, "@@WIN")
    guards = parse_tagged(p.stdout, "@@GUARD")
    if p.returncode != 0 or not wins:
        return None
    w = wins[0]
    w["guard_ok"] = bool(guards) and guards[0].get("ok", False)
    return w


def window_validity(row, w, planned):
    """Why a window does not measure what the row claims, or None."""
    if w is None:
        return "run failed"
    if not w["guard_ok"]:
        return "state guard failed"
    if w["guest_instr"] < 0.9 * planned:
        return "stuck: %d of %d instructions retired" % (w["guest_instr"], planned)
    frac = w["slowpath"] / max(w["guest_instr"], 1)
    if frac > row["slowpath_max"]:
        return "not CPU-bound: %.1f%% slow-path accesses (ceiling %.1f%%)" % (
            100 * frac, 100 * row["slowpath_max"])
    return None


# ---- metric A (and A2) ------------------------------------------------------

CG_EVENTS_RE = re.compile(r"^events:\s*(.*)$", re.M)
CG_TOTALS_RE = re.compile(r"^(?:totals|summary):\s*(.*)$", re.M)


def callgrind_totals(path):
    """{event: count} from a callgrind output file."""
    with open(path) as f:
        text = f.read()
    ev = CG_EVENTS_RE.search(text).group(1).split()
    m = CG_TOTALS_RE.findall(text)
    vals = [int(x) for x in m[-1].split()]
    return dict(zip(ev, vals + [0] * (len(ev) - len(vals))))


def callgrind_window(binary, row, executor, n, sim, test_data):
    d = run_dir(binary, row, "A-%s-%d" % (executor, n))
    script = window_script(d, binary, row, executor, 0, n)
    out = os.path.join(d, "callgrind.out")
    wrapper = CALLGRIND + [
        "--cache-sim=" + ("yes" if sim else "no"),
        "--branch-sim=" + ("yes" if sim else "no"),
        "--toggle-collect=" + row["run_fn"],
        "--callgrind-out-file=" + out,
    ]
    p, secs = run_emu(emu_cmd(binary, row, script, test_data), d, 3600, wrapper)
    w = window_result(p)
    if w is not None and os.path.exists(out):
        w["events"] = callgrind_totals(out)
    elif w is not None:
        w = None
    return w, secs


def metric_a(row, windows, sim):
    """A = (Ir(N2) - Ir(N1)) / (G(N2) - G(N1)), and the A2 counts likewise,
    from the two callgrind_window() results."""
    n2 = row["a_n2"]
    (w1, s1), (w2, s2) = windows
    rec = {"secs": round(s1 + s2, 1)}
    why = window_validity(row, w2, n2) or (None if w1 else "run failed")
    if w1 and w2:
        dg = w2["guest_instr"] - w1["guest_instr"]
        de = {k: w2["events"].get(k, 0) - w1["events"].get(k, 0) for k in w2["events"]}
        rec["guest_instr"] = dg
        rec["slowpath_frac"] = round((w2["slowpath"] - w1["slowpath"]) / max(dg, 1), 5)
        if de.get("Ir", 0) <= 0 and why is None:
            why = "callgrind collected nothing inside %s (renamed?)" % row["run_fn"]
        if dg > 0:
            rec["value"] = de.get("Ir", 0) / dg
            rec["ir"] = de.get("Ir", 0)
            if sim:
                rec["a2"] = {
                    "l1_miss": (de.get("I1mr", 0) + de.get("D1mr", 0) + de.get("D1mw", 0)) / dg,
                    "ll_miss": (de.get("ILmr", 0) + de.get("DLmr", 0) + de.get("DLmw", 0)) / dg,
                    "mispredict": (de.get("Bcm", 0) + de.get("Bim", 0)) / dg,
                }
    rec["valid"] = why is None
    if why:
        rec["invalid"] = why
    return rec


# ---- metric B ---------------------------------------------------------------


def taskset(core):
    if core is None or not shutil.which("taskset"):
        return None
    return ["taskset", "-c", str(core)]


def native_window(binary, row, executor, rnd, core, test_data):
    d = run_dir(binary, row, "B-%s-%d" % (executor, rnd))
    script = window_script(d, binary, row, executor, row["b_warm"], row["b_window"])
    p, _ = run_emu(emu_cmd(binary, row, script, test_data), d, 1800, taskset(core))
    w = window_result(p)
    why = window_validity(row, w, row["b_window"])
    if w is None:
        return {"valid": False, "invalid": why}
    mips = w["guest_instr"] * 1000.0 / max(w["host_ns"], 1)
    rec = {"value": mips, "guest_instr": w["guest_instr"], "host_ns": w["host_ns"],
           "slowpath_frac": round(w["slowpath"] / max(w["guest_instr"], 1), 5),
           "valid": why is None}
    if why:
        rec["invalid"] = why
    return rec


def metric_b(binaries, row, executor, rounds, pairing, flip, core, test_data):
    """`rounds` timed windows per binary.  alternate: H B H B ... on one core,
    starting with base in every other row; concurrent: H and B at the same
    time on cores core and core+1, so both see the same instant of the
    machine."""
    results = {b.label: [] for b in binaries}
    order = list(reversed(binaries)) if flip else list(binaries)
    for rnd in range(rounds):
        if pairing == "concurrent" and len(binaries) == 2:
            with concurrent.futures.ThreadPoolExecutor(2) as ex:
                futs = {b.label: ex.submit(native_window, b, row, executor, rnd,
                                           None if core is None else core + i, test_data)
                        for i, b in enumerate(order)}
                for label, f in futs.items():
                    results[label].append(f.result())
        else:
            for b in order:
                results[b.label].append(native_window(b, row, executor, rnd, core, test_data))
    return results


# ---- metric C ---------------------------------------------------------------


def calibrate(binary):
    p = subprocess.run([binary.path, "--calibrate"], capture_output=True, text=True, timeout=600)
    recs = parse_tagged(p.stdout, "@@CALIB")
    return recs[0] if recs else None


# ---- main -------------------------------------------------------------------


def select_rows(rows, spec):
    if spec == "all":
        return list(rows)
    if spec == "pr":
        return [n for n, r in rows.items() if r.get("pr", True)]
    names = [s.strip() for s in spec.split(",") if s.strip()]
    unknown = [n for n in names if n not in rows]
    if unknown:
        sys.exit("unknown rows: %s (have: %s)" % (", ".join(unknown), ", ".join(rows)))
    return names


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--head", required=True, help="gs-headless under test")
    ap.add_argument("--base", help="gs-headless of the merge base (omit for head-only runs)")
    ap.add_argument("--rows", default="pr", help="pr (default), all, or a comma list")
    ap.add_argument("--metrics", default="A,B", help="any of A,A2,B,C (default A,B)")
    ap.add_argument("--executors", default="auto", help="auto (what both builds have) or a comma list")
    ap.add_argument("--work", default=os.path.join(ROOT, "tmp", "bench"))
    ap.add_argument("--out", help="raw JSON lines (default <work>/raw.jsonl)")
    ap.add_argument("--test-data", default=os.path.join(ROOT, "tests", "data"))
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 1, help="parallel boots and callgrind runs")
    ap.add_argument("--rounds", type=int, default=5, help="metric B rounds per binary")
    ap.add_argument("--pairing", choices=["alternate", "concurrent"], default="concurrent",
                    help="metric B: head and base at once on two cores (default) or alternated on one")
    ap.add_argument("--core", type=int, default=0, help="first host core metric B pins to (-1: no pinning)")
    args = ap.parse_args()

    rows, _ = load_rows(os.path.join(BENCH_DIR, "rows.json"))
    names = select_rows(rows, args.rows)
    metrics = {m.strip().upper() for m in args.metrics.split(",") if m.strip()}
    test_data = os.path.abspath(args.test_data)
    work = os.path.abspath(args.work)
    os.makedirs(work, exist_ok=True)
    core = None if args.core < 0 else args.core
    host_cpu = cpu_model()

    binaries = [Binary("head", args.head, work)]
    if args.base:
        binaries.append(Binary("base", args.base, work))
    first_rom = rows[names[0]]["rom"]
    for b in binaries:
        b.probe(first_rom, test_data)
    if args.executors == "auto":
        executors = [e for e in binaries[0].executors if all(e in b.executors for b in binaries)]
        # A build that adds an executor still gets it measured (no base).
        executors += [e for e in binaries[0].executors if e not in executors]
    else:
        executors = [e.strip() for e in args.executors.split(",")]

    out = open(args.out or os.path.join(work, "raw.jsonl"), "w")
    common = {"cpu": host_cpu}
    emit(out, dict(common, metric="meta", rows=names, metrics=sorted(metrics), executors=executors,
                   binaries={b.label: {"executors": b.executors} for b in binaries},
                   pairing=args.pairing, rounds=args.rounds))

    # C: calibration before anything else runs.
    calib = []
    if "C" in metrics:
        c = calibrate(binaries[0])
        if c:
            calib.append(c)
            emit(out, dict(common, metric="calib", phase="before", **c))

    # Boot: every binary x row, in parallel.
    log("bench: booting %d row(s) on %d binary(s)" % (len(names), len(binaries)))
    booted = {}
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as ex:
        futs = {(b.label, n): ex.submit(boot_row, b, rows[n], test_data) for b in binaries for n in names}
        for (label, n), f in futs.items():
            r = f.result()
            booted[(label, n)] = r["ok"]
            emit(out, dict(common, metric="boot", row=n, binary=label, **r))
            log("bench: boot %-18s %-4s %s (%.0fs)" % (n, label, "ok" if r["ok"] else "FAILED", r["secs"]))

    # The two checkpoints of each row should hold the same guest state.
    if len(binaries) == 2:
        for n in names:
            if booted[("head", n)] and booted[("base", n)]:
                same, diffs = ckptdiff.compare_files(checkpoint_path(binaries[0], rows[n]),
                                                     checkpoint_path(binaries[1], rows[n]))
                emit(out, dict(common, metric="ckpt", row=n, same=same, diffs=diffs[:20]))

    def runnable(b, n, e):
        return booted[(b.label, n)] and e in b.executors

    # A (A2): callgrind, every binary x row x executor, in parallel.
    if "A" in metrics or "A2" in metrics:
        sim = "A2" in metrics
        log("bench: metric A%s" % (" + A2" if sim else ""))
        # The N1 and N2 windows are independent runs: each its own job.
        with concurrent.futures.ThreadPoolExecutor(args.jobs) as ex:
            futs = {}
            for n in names:
                for e in executors:
                    for b in binaries:
                        if runnable(b, n, e):
                            futs[(n, e, b.label)] = [
                                ex.submit(callgrind_window, b, rows[n], e, rows[n][k], sim, test_data)
                                for k in ("a_n1", "a_n2")]
            for (n, e, label), fs in futs.items():
                r = metric_a(rows[n], [f.result() for f in fs], sim)
                emit(out, dict(common, metric="A", row=n, exec=e, binary=label, **r))
                log("bench: A %-18s %-9s %-4s %s" % (n, e, label,
                    "%.3f" % r["value"] if r.get("valid") else r.get("invalid", "?")))

    # B: native timing, serially, nothing else running.
    if "B" in metrics or "C" in metrics:
        log("bench: metric B (%s, %d rounds)" % (args.pairing, args.rounds))
        flip = False
        for n in names:
            for e in executors:
                bs = [b for b in binaries if runnable(b, n, e)]
                if not bs:
                    continue
                res = metric_b(bs, rows[n], e, args.rounds, args.pairing, flip, core, test_data)
                flip = not flip
                for label, recs in res.items():
                    for i, r in enumerate(recs):
                        emit(out, dict(common, metric="B", row=n, exec=e, binary=label, round=i, **r))
                    vals = [r["value"] for r in recs if r.get("valid")]
                    log("bench: B %-18s %-9s %-4s median %.1f MIPS" % (n, e, label,
                        statistics.median(vals) if vals else float("nan")))

    # C: calibration again; compare.py checks the spread.
    if "C" in metrics:
        c = calibrate(binaries[0])
        if c:
            calib.append(c)
            emit(out, dict(common, metric="calib", phase="after", **c))
    out.close()


if __name__ == "__main__":
    main()
