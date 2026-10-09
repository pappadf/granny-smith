#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) pappadf
"""Order integration tests longest-first, and optionally pick one shard.

Usage: order-tests.py WEIGHTS.json [--shard K/N] TEST_DIR...

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
"""

import json
import sys


def main(argv):
    if len(argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    with open(argv[1]) as f:
        weights = json.load(f).get("weights", {})
    args = argv[2:]
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
        loads = [0.0] * n
        picked = [[] for _ in range(n)]
        for t in ordered:
            # An unknown test counts as one minute: heavy enough to spread,
            # not so heavy that it skews the packing of the known ones.
            w = weights.get(t, 60)
            i = min(range(n), key=lambda j: (loads[j], j))
            loads[i] += w
            picked[i].append(t)
        ordered = picked[k - 1]

    print("\n".join(ordered))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
