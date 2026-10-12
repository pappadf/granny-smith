#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) pappadf
"""page_heat.py -- where the PowerPC rows spend their instructions, per page.

Boots each row, profiles a window on the predecoded tier, and reports what a
page-granular translation tier would see: how few pages carry most of the
instructions, how often those pages are written, how long a run inside one
page lasts, how often a memory access leaves the fast path, and how many
translations a heat threshold would make.

Needs a gs-headless built with the page-heat profiler:

  make -f Makefile.headless MODE=fast BUILD_DIR=build/headless-prof \\
       EXTRA_CFLAGS=-DPD_PROFILE
  scripts/bench/page_heat.py --bin build/headless-prof/gs-headless

The rows are tests/bench/<row>.script plus pm6100-desktop (below), each
with its own boot; the window starts where the row's boot ends.
"""

import argparse
import concurrent.futures
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import run as bench  # noqa: E402

# The 601 at the same System 7.6 desktop as pm8500-finder (the browser
# perf spec's pm6100desk): the ROM's interpretive 68K emulator.
PM6100_DESKTOP = """
def bench_boot() {
    machine.boot model="pm6100" ram=24576 rom="${$TEST_DATA}/roms/pm6100-pm7100-pm8100-9feb69b3.rom"
    machine.rtc.time = 3000000000
    machine.scsi.attach_hd "${$TEST_DATA}/systems/system_7_6_170mb_24ac.img" 0
    scheduler.run 4000000000
    bench_guard()
}

def bench_guard() {
    bench_assert_68k_emulator("pm6100-desktop")
}
"""

EXTRA_ROWS = {
    "pm6100-desktop": {
        "rom": "roms/pm6100-pm7100-pm8100-9feb69b3.rom", "model": "pm6100", "ram": 24576,
        "script": PM6100_DESKTOP,
    },
}


def load_rows(names):
    with open(os.path.join(bench.BENCH_DIR, "rows.json")) as f:
        table = json.load(f)["rows"]
    rows = []
    for n in names:
        row = dict(EXTRA_ROWS[n]) if n in EXTRA_ROWS else dict(table[n])
        row["name"] = n
        rows.append(row)
    return rows


def profile_row(binary, row, window, test_data, work):
    d = os.path.join(work, row["name"])
    os.makedirs(d, exist_ok=True)
    out = os.path.join(d, "heat.json")
    script = os.path.join(d, "heat.script")
    with open(script, "w") as f:
        f.write('include "%s"\n' % os.path.join(bench.ROOT, "tests", "integration", "lib", "mac.script"))
        f.write('include "%s"\n' % os.path.join(bench.BENCH_DIR, "lib", "bench.script"))
        if "script" in row:
            f.write(row["script"])
        else:
            f.write('include "%s"\n' % os.path.join(bench.BENCH_DIR, row["name"] + ".script"))
        f.write("bench_boot()\n")
        f.write("predecode.profile_reset()\n")
        f.write("let i0 = scheduler.instr_count\n")
        f.write("scheduler.run %d\n" % window)
        f.write('echo "@@HEAT {\\"guest_instr\\":${scheduler.instr_count - $i0}}"\n')
        f.write('predecode.profile("%s")\n' % out)
        f.write("bench_check()\n")
        f.write("quit\n")
    cmd = [
        binary, "rom=" + os.path.join(test_data, row["rom"]), "model=" + row["model"], "ram=%d" % row["ram"],
        "script=" + script, "--var", "TEST_DATA=" + test_data, "--var", "BENCH_DIR=" + bench.BENCH_DIR,
    ] + bench.EMU_FLAGS
    p, _ = bench.run_emu(cmd, d, 7200)
    heat = bench.parse_tagged(p.stdout, "@@HEAT")
    guard = bench.parse_tagged(p.stdout, "@@GUARD")
    if not heat or not os.path.exists(out):
        bench.log("heat: %s failed (see %s/output.log)" % (row["name"], d))
        return None
    with open(out) as f:
        prof = json.load(f)
    prof["guest_instr"] = heat[0]["guest_instr"]
    prof["guard_ok"] = bool(guard) and guard[0].get("ok", False)
    return prof


def pages_for(pages, total, frac):
    acc = 0
    for i, p in enumerate(pages):
        acc += p["instr"]
        if acc >= frac * total:
            return i + 1
    return None  # the predecoded pages carry less than frac of the window


def weighted_median_bucket(hist):
    """The log2 bucket holding the instruction-weighted median."""
    total = sum(h[1] for h in hist)
    acc = 0
    for b, h in enumerate(hist):
        acc += h[1]
        if total and acc >= total / 2:
            return b
    return 0


def bucket_range(b):
    return "%d-%d" % (1 << (b - 1), (1 << b) - 1) if b else "0"


def summarize(name, prof):
    total = prof["guest_instr"]
    pages = prof["pages"]
    pd_instr = sum(p["instr"] for p in pages)
    hot_n = pages_for(pages, total, 0.90)
    hot = pages[:hot_n] if hot_n else pages
    visits = sum(p["visits"] for p in pages)
    s = {
        "row": name,
        "guard_ok": prof["guard_ok"],
        "guest_instr": total,
        "predecoded_share": pd_instr / total if total else 0,
        "generic_share": prof["generic_steps"] / total if total else 0,
        "pages_touched": len(pages),
        "pages_50": pages_for(pages, total, 0.50),
        "pages_90": hot_n,
        "pages_99": pages_for(pages, total, 0.99),
        "hot_pages_written": sum(1 for p in hot if p["invals"]),
        "hot_invals": sum(p["invals"] for p in hot),
        "hot_invals_per_minstr": 1e6 * sum(p["invals"] for p in hot) / total if total else 0,
        "all_invals": sum(p["invals"] for p in pages),
        "code_page_stores": sum(p["stores"] for p in pages),
        "slowpath_per_kinstr": 1e3 * prof["slowpath"] / total if total else 0,
        "slowpath_in_block_per_kinstr": 1e3 * prof["slowpath_in_block"] / total if total else 0,
        "instr_per_visit": pd_instr / visits if visits else 0,
        "hops_per_kinstr": 1e3 * prof["hops"] / total if total else 0,
        "hops_64k_share": prof["hops_64k"] / prof["hops"] if prof["hops"] else 0,
        "hops_1m_share": prof["hops_1m"] / prof["hops"] if prof["hops"] else 0,
        "visit_median_bucket": bucket_range(weighted_median_bucket(prof["visit_hist"])),
        "sprint_median_bucket": bucket_range(weighted_median_bucket(prof["sprint_hist"])),
        "sprints_per_minstr": 1e6 * sum(h[0] for h in prof["sprint_hist"]) / total if total else 0,
        "thresholds": [
            {"heat": t["heat"], "translations": t["translations"],
             "covered_share": t["covered"] / total if total else 0}
            for t in prof["thresholds"]
        ],
        "top_pages": [
            {"guest": "%08x" % p["guest"], "share": p["instr"] / total, "invals": p["invals"],
             "instr_per_visit": p["instr"] / p["visits"] if p["visits"] else 0}
            for p in pages[:10]
        ],
    }
    return s


def print_summary(s):
    print("== %s: %.0f M instructions%s" % (s["row"], s["guest_instr"] / 1e6, "" if s["guard_ok"] else " (GUARD FAILED)"))
    print("  predecoded %.1f %%, generic %.1f %%; %d pages touched" % (
        100 * s["predecoded_share"], 100 * s["generic_share"], s["pages_touched"]))
    print("  pages carrying 50/90/99 %% of instructions: %s / %s / %s" % (s["pages_50"], s["pages_90"], s["pages_99"]))
    print("  hot set (90 %%): %d pages written, %d invalidations (%.2f per M instructions); all pages %d, %d code-page stores" % (
        s["hot_pages_written"], s["hot_invals"], s["hot_invals_per_minstr"], s["all_invals"], s["code_page_stores"]))
    print("  slow-path accesses per 1000 instructions: %.2f (%.2f from a predecoded page)" % (
        s["slowpath_per_kinstr"], s["slowpath_in_block_per_kinstr"]))
    print("  instructions per page visit: %.1f mean; instruction-weighted median visit %s" % (
        s["instr_per_visit"], s["visit_median_bucket"]))
    print("  page-to-page transitions: %.1f per 1000 instructions; %.0f %% within 64 KB, %.0f %% within 1 MB" % (
        s["hops_per_kinstr"], 100 * s["hops_64k_share"], 100 * s["hops_1m_share"]))
    print("  sprints: %.1f per M instructions; instruction-weighted median budget %s" % (
        s["sprints_per_minstr"], s["sprint_median_bucket"]))
    for t in s["thresholds"]:
        print("  heat %7d: %6d translations, %.1f %% of instructions after translation" % (
            t["heat"], t["translations"], 100 * t["covered_share"]))
    for p in s["top_pages"]:
        print("    page %s  %5.1f %%  invals %d  %.0f instr/visit" % (
            p["guest"], 100 * p["share"], p["invals"], p["instr_per_visit"]))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", required=True, help="gs-headless built with -DPD_PROFILE")
    ap.add_argument("--rows", default="pm8500-finder,g3dt-boot,pm6100-desktop")
    ap.add_argument("--window", type=int, default=300000000, help="profiled instructions after the boot")
    ap.add_argument("--work", default=os.path.join(bench.ROOT, "tmp", "page-heat"))
    ap.add_argument("--test-data", default=os.path.join(bench.ROOT, "tests", "data"))
    ap.add_argument("--jobs", type=int, default=2)
    ap.add_argument("--out", help="write the summaries as JSON here")
    args = ap.parse_args()
    binary = os.path.abspath(args.bin)
    test_data = os.path.abspath(args.test_data)
    rows = load_rows(args.rows.split(","))
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as ex:
        profs = list(ex.map(lambda r: profile_row(binary, r, args.window, test_data, args.work), rows))
    sums = []
    for row, prof in zip(rows, profs):
        if prof is None:
            continue
        s = summarize(row["name"], prof)
        sums.append(s)
        print_summary(s)
    if args.out:
        with open(args.out, "w") as f:
            json.dump(sums, f, indent=1)
    return 0 if len(sums) == len(rows) else 1


if __name__ == "__main__":
    sys.exit(main())
