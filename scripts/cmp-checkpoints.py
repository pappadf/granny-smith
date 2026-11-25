#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) pappadf
#
# cmp-checkpoints.py — the predecoded cores' differential oracle.
#
# Decodes two v2 (consolidated) checkpoints block by block and compares the
# guest state they carry.  Two kinds of bytes are NOT guest state and are
# masked: the storage bookkeeping (backing-file paths and delta ids, which
# name each run's own storage cache) and 8-byte-aligned words that hold HOST
# POINTERS in both files (cpu_t/ppc_t and several device structs are written verbatim,
# pointers included; the reader nulls them, and ASLR plus a different heap
# history make them differ between any two processes).  Everything else —
# RAM, the register files with their raw flag words, the scheduler's cycle
# count and event queue, every peripheral — must match byte for byte.
#
# Usage: cmp-checkpoints.py A.gs B.gs [-v]      exit 0 = identical guest state

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "lib"))
from gscheckpoint import CheckpointFormatError, decode_blocks, diff_block, is_storage_block


def compare(a, b, verbose):
    if len(a) != len(b):
        print("block count differs: %d vs %d" % (len(a), len(b)))
        return False
    ok = True
    for ba, bb in zip(a, b):
        fa, la, da, db = ba.file, ba.line, ba.data, bb.data
        if (ba.file, ba.line, ba.tag, ba.kind) != (bb.file, bb.line, bb.tag, bb.kind):
            print("block identity differs: %s:%d/%s vs %s:%d/%s" % (fa, la, ba.kind, bb.file, bb.line, bb.kind))
            return False
        if is_storage_block(ba):
            if verbose:
                print("skip  %s:%d (host storage bookkeeping)" % (fa, la))
            continue
        if da == db:
            if verbose:
                print("same  %s:%d %d bytes" % (fa, la, len(da)))
            continue
        if len(da) != len(db):
            print("DIFF  %s:%d size %d vs %d" % (fa, la, len(da), len(db)))
            ok = False
            continue
        bad = diff_block(fa, da, db)
        if bad:
            ok = False
            print("DIFF  %s:%d %d byte(s): %s" % (fa, la, len(bad), ", ".join("%#x" % x for x in bad[:16])))
            for x in bad[:8]:
                print("      +%#x: %02x vs %02x" % (x, da[x], db[x]))
        elif verbose:
            print("same  %s:%d %d bytes (host pointers masked)" % (fa, la, len(da)))
    return ok


def main():
    args = [x for x in sys.argv[1:] if not x.startswith("-")]
    verbose = "-v" in sys.argv
    if len(args) != 2:
        sys.exit("usage: cmp-checkpoints.py A.gs B.gs [-v]")
    try:
        a = decode_blocks(args[0])
        b = decode_blocks(args[1])
    except (OSError, CheckpointFormatError) as e:
        sys.exit(str(e))
    if compare(a, b, verbose):
        print("identical guest state (%d blocks)" % len(a))
        sys.exit(0)
    sys.exit(1)


if __name__ == "__main__":
    main()
