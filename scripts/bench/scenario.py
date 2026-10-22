#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) pappadf
"""scenario.py -- interactive scenarios (docs/guide/BENCHMARKS.md).

Times what a user waits for -- a menu opening, a window's genie into the
Dock -- rather than a CPU-bound window.  A scenario is a script in
tests/bench/ (osx-interactive.script, g3-boot-desktop.script) that boots to a state and saves a
checkpoint (MODE=boot), then loads it and plays its interactions, each a
fixed stretch of scheduler slots, printing one @@SCEN line per interaction
with its host wall time and the guest instructions it retired (MODE=run).

For every binary and configuration this runs the scenario --runs times, in
separate processes, the configurations interleaved, and prints the median
milliseconds per interaction.  Every run of every configuration must retire
the same guest instructions in each interaction: the executors and the
translation tier never change the guest's timeline, so a difference is a
correctness bug and is reported as one.

Configurations (each a name and the lines its driver runs after the
checkpoint loads):
  predecode            the predecoded executor (the default)
  switch               the switch core
  jit                  the translation tier, user code (wasm builds)
  jit-sup              the translation tier, supervisor code too

Usage:
  scenario.py --bin native=build/headless-fast/gs-headless \\
              --bin node=build/headless-fast-node/gs-headless \\
              [--configs predecode,jit,jit-sup] [--runs 3] \\
              [--scenario osx-interactive] [--work tmp/scenario] [--json out.json]

A configuration a binary does not have (jit on a native build) is skipped
for it.  Node runs of the wasm build stand in for the browser's CPU work;
compiles there are synchronous, so the cold round charges the tier for
compiling in a way the browser, which compiles on other threads, does not.
"""

import argparse
import json
import os
import shutil
import statistics
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
BENCH_DIR = os.path.join(ROOT, "tests", "bench")
EMU_FLAGS = ["--speed=turbo", "--jobs=inline", "--io=sync"]

# The machine each scenario starts with (its script attaches the media)
SCENARIOS = {
    "osx-interactive": {
        "rom": "roms/pmg3dt-pmg3mt-78f57389.rom",
        "model": "pmg3dt",
        "ram": 262144,
        "config": "systems/mac_os_x_10_0_3_2gb_g3_minimal_aqua.json",
    },
    "g3-boot-desktop": {
        "rom": "roms/pmg3dt-pmg3mt-78f57389.rom",
        "model": "pmg3dt",
        "ram": 262144,
        "disk": "systems/mac_os_9_2_1_516mb_g3.img",
    },
}

CONFIGS = {
    "switch": ("predecode.enabled = 0\njit.enabled = 0\n", False),
    "predecode": ("predecode.enabled = 1\njit.enabled = 0\n", False),
    "jit": ("predecode.enabled = 1\njit.enabled = 1\njit.supervisor = 0\n", True),
    "jit-sup": ("predecode.enabled = 1\njit.enabled = 1\njit.supervisor = 1\n", True),
}


def log(msg):
    print(msg, file=sys.stderr, flush=True)


def parse_tagged(text, tag):
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
    def __init__(self, label, path, work):
        self.label = label
        self.path = os.path.abspath(path)
        self.work = os.path.join(work, label)
        self.has_jit = False

    def probe(self, rom):
        d = os.path.join(self.work, "probe")
        os.makedirs(d, exist_ok=True)
        script = os.path.join(d, "probe.script")
        with open(script, "w") as f:
            f.write('echo "@@PROBE {\\"jit\\":${try(jit.available, 0)}}"\nquit\n')
        p = subprocess.run([self.path, "rom=" + rom, "script=" + script] + EMU_FLAGS, cwd=d,
                           capture_output=True, text=True, timeout=300)
        probes = parse_tagged(p.stdout, "@@PROBE")
        self.has_jit = bool(probes) and probes[0].get("jit", 0) == 1


def run_scenario(binary, scen, name, mode, setup, test_data, tag, timeout):
    """One process: MODE=boot or MODE=run under `setup`; returns its @@SCEN records."""
    d = os.path.join(binary.work, name, tag)
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    driver = os.path.join(d, "driver.script")
    with open(driver, "w") as f:
        body = "".join("    " + l + "\n" for l in setup.splitlines()) or "    let unused = 0\n"
        f.write("def scen_setup() {\n%s}\n" % body)
        f.write('include "%s"\n' % os.path.join(BENCH_DIR, name + ".script"))
    cmd = [
        binary.path,
        "rom=" + os.path.join(test_data, scen["rom"]),
        "model=" + scen["model"],
        "ram=%d" % scen["ram"],
    ] + (["config=" + os.path.join(test_data, scen["config"])] if "config" in scen else []) + [
        "script=" + driver,
        "--var", "TEST_DATA=" + test_data,
        "--var", "CKPT=" + os.path.join(binary.work, name, "state.gsc"),
        "--var", "MODE=" + mode,
    ] + EMU_FLAGS
    env = dict(os.environ)
    env["GS_STORAGE_CACHE"] = os.path.join(d, "storage-cache")
    p = subprocess.run(cmd, cwd=d, capture_output=True, text=True, timeout=timeout, env=env)
    with open(os.path.join(d, "output.log"), "w") as f:
        f.write("$ " + " ".join(cmd) + "\n" + p.stdout + p.stderr)
    shutil.rmtree(env["GS_STORAGE_CACHE"], ignore_errors=True)
    recs = parse_tagged(p.stdout, "@@SCEN")
    failed = [l for l in (p.stdout + p.stderr).splitlines() if "ASSERT FAILED" in l]
    if failed or not recs:
        raise RuntimeError("%s %s %s failed (%s): %s" % (binary.label, name, tag, d, failed[:1] or "no @@SCEN lines"))
    return recs


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", action="append", required=True, help="label=path of a gs-headless (repeatable)")
    ap.add_argument("--configs", default="predecode,jit,jit-sup")
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--scenario", default="osx-interactive", choices=sorted(SCENARIOS))
    ap.add_argument("--work", default=os.path.join(ROOT, "tmp", "scenario"))
    ap.add_argument("--test-data", default=os.path.join(ROOT, "tests", "data"))
    ap.add_argument("--json", help="write every measurement here")
    ap.add_argument("--timeout", type=int, default=3600)
    a = ap.parse_args()

    scen = SCENARIOS[a.scenario]
    test_data = os.path.abspath(a.test_data)
    for k in ("rom", "config", "disk"):
        if k in scen and not os.path.exists(os.path.join(test_data, scen[k])):
            sys.exit("missing test data: %s (fetch it: scripts/fetch-test-data.sh)" % scen[k])
    configs = [c.strip() for c in a.configs.split(",") if c.strip()]
    for c in configs:
        if c not in CONFIGS:
            sys.exit("unknown configuration %r (have: %s)" % (c, ", ".join(CONFIGS)))

    bins = []
    for spec in a.bin:
        label, _, path = spec.partition("=")
        if not path:
            sys.exit("--bin wants label=path, got %r" % spec)
        b = Binary(label, path, os.path.abspath(a.work))
        b.probe(os.path.join(test_data, scen["rom"]))
        bins.append(b)

    # Boot: one checkpoint per binary (a checkpoint refuses another build)
    for b in bins:
        log("[%s] booting %s" % (b.label, a.scenario))
        rec = run_scenario(b, scen, a.scenario, "boot", "", test_data, "boot", a.timeout)
        log("[%s] checkpoint at %d instructions" % (b.label, rec[-1]["instr"]))

    # Runs: the configurations interleaved, so a slow stretch of the host
    # lands on all of them
    cells = [(b, c) for b in bins for c in configs if not CONFIGS[c][1] or b.has_jit]
    results = {}  # (label, config) -> phase -> [ms...]
    instr = {}  # phase -> {(label, config, run): instr}
    all_recs = []
    for r in range(a.runs):
        for b, c in cells:
            log("[%s] %s run %d/%d" % (b.label, c, r + 1, a.runs))
            recs = run_scenario(b, scen, a.scenario, "run", CONFIGS[c][0], test_data, "%s-%d" % (c, r), a.timeout)
            for rec in recs:
                ph = rec["phase"]
                results.setdefault((b.label, c), {}).setdefault(ph, []).append(rec["ns"] / 1e6)
                instr.setdefault(ph, {})[(b.label, c, r)] = rec["instr"]
                all_recs.append(dict(rec, bin=b.label, config=c, run=r))

    phases = list(results[(cells[0][0].label, cells[0][1])].keys())
    cols = ["%s/%s" % (b.label, c) for b, c in cells]
    w = max(12, max(len(x) for x in cols) + 2)
    print("\n%s: median ms over %d runs (lower is better)\n" % (a.scenario, a.runs))
    print("%-14s" % "interaction" + "".join("%*s" % (w, x) for x in cols))
    totals = {}
    for ph in phases:
        row = "%-14s" % ph
        for b, c in cells:
            m = statistics.median(results[(b.label, c)][ph])
            rnd = ph[-1] if ph[-1] in "12" else "all"  # a scenario without rounds has one total
            totals.setdefault((b.label, c, rnd), 0.0)
            totals[(b.label, c, rnd)] += m
            row += "%*.0f" % (w, m)
        print(row)
    for rnd, label in (("1", "cold total"), ("2", "warm total"), ("all", "total")):
        if not any(k[2] == rnd for k in totals):
            continue
        print("%-14s" % label + "".join("%*.0f" % (w, totals.get((b.label, c, rnd), 0)) for b, c in cells))

    bad = [ph for ph in phases if len(set(instr[ph].values())) != 1]
    if bad:
        print("\nGUEST TIMELINE DIFFERS (a correctness bug) in: %s" % ", ".join(bad))
        for ph in bad:
            for k, v in sorted(instr[ph].items()):
                print("  %s %s/%s run %d: %d" % (ph, k[0], k[1], k[2], v))
    else:
        print("\nguest instructions identical in every run and configuration")

    if a.json:
        with open(a.json, "w") as f:
            json.dump({"scenario": a.scenario, "runs": a.runs, "records": all_recs}, f, indent=1)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
