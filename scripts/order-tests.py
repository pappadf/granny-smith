#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) pappadf
"""Order integration tests longest-first, and optionally pick one shard.

Usage: order-tests.py WEIGHTS.json [--shard K/N] TEST_DIR...
       order-tests.py WEIGHTS.json --deps TEST_DIR...

Prints the given test directories, one per line, in descending order of
their recorded wall seconds (tests/integration/test-weights.json). make -j
starts prerequisites in list order, so this is longest-processing-time-first
scheduling: the long tests start at once instead of whenever the alphabet
reaches them, and the short ones fill the tail.

A test with no recorded weight sorts FIRST: a new test is the one we know
least about, and starting it early is what bounds the damage if it is long.

--shard K/N (1-based) bin-packs the tests greedily onto N shards -- each
test, longest first, onto the currently lightest shard -- and prints only
shard K's, still longest-first.  The assignment depends only on the weights
file and the test list, so every shard job computes the same partition
without talking to the others, and a local `SHARD=K/N` reproduces CI's.

Checkpoint fixtures: a test whose config.mk says `TEST_PROVIDES := name`
saves that fixture, and one that says `TEST_NEEDS := name` loads it (both
take a space-separated list; lib/mac.script's fixture_load / fixture_save).
A producer and its consumers are bin-packed as ONE item of their summed
weight, so they always land on the same shard.  --deps prints the make
edges for the given tests, one `consumer:producer` per line (the Makefile
turns them into order-only prerequisites).  Run from tests/integration.
"""

import json
import sys


def config_list(test, key):
    """The names a test's config.mk lists under `key :=` (empty if none)."""
    try:
        with open(f"{test}/config.mk") as f:
            for line in f:
                if line.startswith(key) and ":=" in line and line.split(":=")[0].strip() == key:
                    return line.split(":=", 1)[1].split()
    except FileNotFoundError:
        pass
    return []


def fixture_edges(tests):
    """(consumer, producer) for each fixture a listed test needs and a listed test provides."""
    provider = {}
    for t in tests:
        for name in config_list(t, "TEST_PROVIDES"):
            provider[name] = t
    edges = []
    for t in tests:
        for name in config_list(t, "TEST_NEEDS"):
            p = provider.get(name)
            if p is not None and p != t:
                edges.append((t, p))
    return edges


def groups_of(tests):
    """Tests joined by fixture edges, as lists (union-find over the edges)."""
    parent = {t: t for t in tests}

    def find(t):
        while parent[t] != t:
            parent[t] = parent[parent[t]]
            t = parent[t]
        return t

    for c, p in fixture_edges(tests):
        parent[find(c)] = find(p)
    groups = {}
    for t in tests:
        groups.setdefault(find(t), []).append(t)
    return list(groups.values())


def main(argv):
    if len(argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    with open(argv[1]) as f:
        weights = json.load(f).get("weights", {})
    args = argv[2:]
    if args[:1] == ["--deps"]:
        for c, p in fixture_edges(args[1:]):
            print(f"{c}:{p}")
        return 0
    shard = None
    if len(args) >= 2 and args[0] == "--shard":
        k, n = (int(x) for x in args[1].split("/"))
        if not 1 <= k <= n:
            print(f"order-tests: bad shard {args[1]}", file=sys.stderr)
            return 2
        shard = (k, n)
        args = args[2:]

    unknown = float("inf")
    # Ties (most unit-tier tests weigh 0) break on name, so the order is stable.
    ordered = sorted(args, key=lambda t: (-weights.get(t, unknown), t))

    if shard is not None:
        k, n = shard
        # An unknown test counts as one minute: heavy enough to spread, not
        # so heavy that it skews the packing of the known ones.  A fixture
        # group packs as one item.
        groups = groups_of(ordered)
        gw = lambda g: sum(weights.get(t, 60) for t in g)
        groups.sort(key=lambda g: (-gw(g), min(g)))
        loads = [0.0] * n
        mine = set()
        for g in groups:
            i = min(range(n), key=lambda j: (loads[j], j))
            loads[i] += gw(g)
            if i == k - 1:
                mine.update(g)
        ordered = [t for t in ordered if t in mine]

    print("\n".join(ordered))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
