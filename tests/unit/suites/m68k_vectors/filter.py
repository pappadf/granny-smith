#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) pappadf
"""Build the m68k_vectors tier: the m68k-test smoke tier for the family
members this emulator has a core for, minus the vectors listed in
exclusions.txt (known core-vs-model disagreements, each with its reason).

The reference runner walks directories and caps its path list, so the
exclusions cannot be passed to it; instead this writes a filtered copy of
the tier (plain .json, which the runner reads as readily as .json.gz) and
the suite binary is pointed at it.  Every dropped vector is counted per
rule, so the run's log says exactly what was not tested.

usage: filter.py --tier <vectors/smoke> --exclusions <file> --out <dir>
"""

import argparse
import gzip
import json
import os
import shutil
import sys

MEMBERS = ("68000", "68030", "68040")


def load_exclusions(path):
    """Parse `<member> <kind> <value> -- <reason>` lines; kind is
    `exception` (drop vectors whose exception name matches), `file`
    (drop the mnemonic's whole file) or `prefix` (every file whose
    mnemonic starts with the value)."""
    rules = []
    with open(path) as f:
        for lineno, raw in enumerate(f, 1):
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            head, _, reason = line.partition("--")
            parts = head.split()
            if len(parts) != 3 or parts[1] not in ("exception", "file", "prefix"):
                sys.exit(f"{path}:{lineno}: expected '<member> exception|file|prefix <value> -- <reason>'")
            rules.append({"member": parts[0], "kind": parts[1], "value": parts[2],
                          "reason": reason.strip(), "files": 0, "vectors": 0, "line": lineno})
    return rules


def file_rule(rules, member, mnemonic):
    """The first file-level rule covering this mnemonic, or None."""
    for r in rules:
        if r["member"] != member:
            continue
        if r["kind"] == "file" and r["value"] == mnemonic:
            return r
        if r["kind"] == "prefix" and mnemonic.startswith(r["value"]):
            return r
    return None


def exception_rule(rules, member, mnemonic, vector):
    """The first exception rule matching this vector's outcome, or None.
    A rule value of `mnemonic/exception` applies to that file only."""
    exc = vector.get("exception")
    if not exc:
        return None
    for r in rules:
        if r["member"] != member or r["kind"] != "exception":
            continue
        scope, _, name = r["value"].rpartition("/")
        if name == exc.get("name") and (not scope or scope == mnemonic):
            return r
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tier", required=True)
    ap.add_argument("--exclusions", required=True)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    rules = load_exclusions(args.exclusions)
    if os.path.isdir(args.out):
        shutil.rmtree(args.out)

    kept_files = kept_vectors = 0
    for member in MEMBERS:
        src = os.path.join(args.tier, member)
        dst = os.path.join(args.out, member)
        os.makedirs(dst)
        for name in sorted(os.listdir(src)):
            if not name.endswith(".json.gz"):
                continue
            with gzip.open(os.path.join(src, name)) as f:
                doc = json.load(f)
            # The mnemonic comes from the provenance, not the escaped filename.
            mnemonic = doc["provenance"]["instruction"]
            rule = file_rule(rules, member, mnemonic)
            if rule:
                rule["files"] += 1
                rule["vectors"] += len(doc["vectors"])
                continue
            kept = []
            for v in doc["vectors"]:
                rule = exception_rule(rules, member, mnemonic, v)
                if rule:
                    rule["vectors"] += 1
                else:
                    kept.append(v)
            if not kept:
                continue
            doc["vectors"] = kept
            doc["provenance"]["count"] = len(kept)
            with open(os.path.join(dst, name[: -len(".gz")]), "w") as f:
                json.dump(doc, f, separators=(",", ":"))
            kept_files += 1
            kept_vectors += len(kept)

    print(f"[m68k_vectors] tier: {kept_vectors} vectors in {kept_files} files kept")
    for r in rules:
        what = f"{r['vectors']} vectors" + (f" ({r['files']} files)" if r["files"] else "")
        print(f"[m68k_vectors] excluded {r['member']} {r['kind']} {r['value']}: {what} -- {r['reason']}")
        if r["vectors"] == 0:
            print(f"[m68k_vectors]   note: rule at exclusions.txt:{r['line']} matched nothing (stale?)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
