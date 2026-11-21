#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) pappadf
"""testconfig.py -- the one reader of an integration test's config.mk for
the Python scripts (order-tests.py, test-matrix.py).

Every config.mk assigns its keys as `KEY := value`; the integration
Makefile refuses a TEST_ key assigned any other way, so no reader has to
know `=`, `?=` or `+=`.  The shell readers (scripts/run-integration-test.sh
cfg(), the Makefile's tier_of) take the same form, the first such line of a
key, and so does read_config().
"""

import re
from pathlib import Path

# One `KEY := value` line; the value without its surrounding blanks.
ASSIGN = re.compile(r"^([A-Z][A-Z0-9_]*)[ \t]*:=[ \t]*(.*?)[ \t]*$")


def read_config(test_dir):
    """{KEY: value} of `test_dir`/config.mk (the first assignment of each
    key wins); empty when the test has no config.mk."""
    try:
        text = (Path(test_dir) / "config.mk").read_text(errors="replace")
    except FileNotFoundError:
        return {}
    out = {}
    for line in text.splitlines():
        m = ASSIGN.match(line)
        if m and m.group(1) not in out:
            out[m.group(1)] = m.group(2)
    return out
