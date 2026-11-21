#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) pappadf
"""ckptdiff.py -- do two checkpoints from two BUILDS hold the same guest state?

The benchmark harness boots every row once per binary (a checkpoint refuses
a file from another build) and asks this whether the PR changed the guest
timeline on the way.  The decoding and the masking are
scripts/lib/gscheckpoint.py, shared with the predecoded cores' differential
oracle (scripts/cmp-checkpoints.py), with one difference that two builds
need: a block is identified by its source file, tag and position, not by the
source LINE that wrote it, since a PR that edits a file above a
checkpoint_write() moves the line and nothing else.

Masked, because they are not guest state:
  * the storage layer's blocks (backing-file paths and delta ids, which name
    each run's own storage cache);
  * 8-byte words that hold a host pointer in both files (several structs are
    written verbatim, pointers included);
  * the 68040's MMUSR, which mirrors the last table walk (fast-path state);
  * the GSDisk driver's build stamp ("GSDisk build <date> <time>"), which the
    bare-volume wrapper puts on the disk and the guest copies into RAM: the
    one place a build's time reaches guest state.

Usage: ckptdiff.py A.gsc B.gsc     exit 0 = identical guest state
"""

import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "lib"))
from gscheckpoint import decode_blocks, diff_block, is_storage_block


def compare(a, b):
    """(same, [difference description, ...])."""
    if len(a) != len(b):
        return False, ["block count differs: %d vs %d" % (len(a), len(b))]
    diffs = []
    for idx, (ba, bb) in enumerate(zip(a, b)):
        fa, da, db = ba.file, ba.data, bb.data
        # Identity without the LINE: an edit above a checkpoint_write() moves it.
        if (ba.file, ba.tag, ba.kind) != (bb.file, bb.tag, bb.kind):
            return False, diffs + ["block %d identity differs: %s/%s vs %s/%s" % (idx, fa, ba.kind, bb.file, bb.kind)]
        if is_storage_block(ba) or da == db:
            continue
        if len(da) != len(db):
            diffs.append("block %d (%s): size %d vs %d" % (idx, fa, len(da), len(db)))
            continue
        bad = diff_block(fa, da, db, mask_build_stamp=True)
        if bad:
            diffs.append("block %d (%s): %d byte(s) differ, first at +%#x" % (idx, fa, len(bad), bad[0]))
    return not diffs, diffs


def compare_files(pa, pb):
    try:
        return compare(decode_blocks(pa), decode_blocks(pb))
    except (OSError, ValueError, struct.error) as e:
        return False, ["could not compare: %s" % e]


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: ckptdiff.py A.gsc B.gsc")
    same, diffs = compare_files(sys.argv[1], sys.argv[2])
    for d in diffs:
        print(d)
    print("identical guest state" if same else "guest state differs")
    sys.exit(0 if same else 1)


if __name__ == "__main__":
    main()
