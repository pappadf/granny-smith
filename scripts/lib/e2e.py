#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) pappadf
"""e2e.py -- the web2 e2e suite's shared bookkeeping for the CI scripts.

scripts/gen-e2e-weights.py writes tests/e2e/e2e-weights.json from CI's
Playwright JSON reports and scripts/e2e-shard.py reads it; both name a spec
file by spec_key(), so a spec in a subdirectory of web2-specs gets its own
measured weight.  The serial project's spec list is tests/e2e/serial-specs.json,
read by the Playwright config and by e2e-shard.py alike.  merge_median() is
the weight files' update rule, shared with scripts/gen-test-weights.py.

Self-test: python3 scripts/lib/e2e.py selftest
"""

import json
import pathlib
import statistics
import sys

E2E = pathlib.Path(__file__).resolve().parents[2] / "tests" / "e2e"
TEST_DIR = "web2-specs"
SERIAL_SPECS = E2E / "serial-specs.json"


def spec_key(file):
    """The weights key of a spec file: its path under tests/e2e.

    `file` is a report's suite `file` (relative to the config's testDir,
    web2-specs) or any path that runs through web2-specs/."""
    p = pathlib.PurePosixPath(str(file).replace("\\", "/"))
    parts = p.parts
    if TEST_DIR in parts:
        parts = parts[len(parts) - 1 - parts[::-1].index(TEST_DIR) + 1 :]
    return "/".join((TEST_DIR,) + tuple(parts))


def serial_specs():
    """The spec file names that run in the `serial` project."""
    return set(json.loads(SERIAL_SPECS.read_text()))


def merge_median(old, samples):
    """`old` with every key of `samples` set to the rounded median of its samples.

    Keys the samples do not mention keep their old value, so a partial run
    never drops a weight back to unknown."""
    out = dict(old)
    for key, xs in samples.items():
        out[key] = int(round(statistics.median(xs)))
    return out


def selftest():
    assert spec_key("boot.spec.ts") == "web2-specs/boot.spec.ts"
    assert spec_key("perf-quake3-gpu/perf-quake3-gpu.spec.ts") == "web2-specs/perf-quake3-gpu/perf-quake3-gpu.spec.ts"
    assert spec_key("/w/tests/e2e/web2-specs/a/b.spec.ts") == "web2-specs/a/b.spec.ts"
    assert spec_key("web2-specs/c.spec.ts") == "web2-specs/c.spec.ts"
    # The reader's keys: every spec on disk, by its path under tests/e2e.
    for path in (E2E / TEST_DIR).rglob("*.spec.ts"):
        rel = str(path.relative_to(E2E))
        assert spec_key(rel) == rel, rel
        assert spec_key(str(path.relative_to(E2E / TEST_DIR))) == rel, rel
    assert merge_median({"a": 1, "b": 2}, {"b": [10, 30, 20], "c": [4.6]}) == {"a": 1, "b": 20, "c": 5}
    names = serial_specs()
    assert names and all(n.endswith(".spec.ts") for n in names), names
    print("e2e selftest: ok")


if __name__ == "__main__":
    if sys.argv[1:] == ["selftest"]:
        selftest()
    else:
        sys.exit("usage: e2e.py selftest")
