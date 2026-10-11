#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) pappadf
"""ckptdiff.py -- do two checkpoints from two BUILDS hold the same guest state?

The benchmark harness boots every row once per binary (a checkpoint refuses
a file from another build) and asks this whether the PR changed the guest
timeline on the way.  The decoding and the masking follow the predecoded
cores' differential oracle (scripts/cmp-checkpoints.py on that branch), with
one difference that two builds need: a block is identified by its source
file, tag and position, not by the source LINE that wrote it, since a PR
that edits a file above a checkpoint_write() moves the line and nothing else.

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

import re
import struct
import sys

# The header: magic (8) + build id (BUILD_ID_LEN = 20); blocks follow.
HEADER_LEN = 8 + 20


def tag_hash(name):
    # checkpoint.c's tag hash: FNV-1a over the block's name, 0 reserved.
    h = 2166136261
    for ch in name.encode():
        h = ((h ^ ch) * 16777619) & 0xFFFFFFFF
    return h or 1


STORAGE_TAG = tag_hash("storage")

# The GSDisk driver's build stamp (__DATE__ __TIME__), as it appears in RAM.
BUILD_STAMP_RE = re.compile(rb"GSDisk build [A-Z][a-z]{2} [ \d]\d \d{4} \d\d:\d\d:\d\d")


def decode_blocks(path):
    """[(file, tag, kind, data)] for every block of a v2 checkpoint.

    A data block is size u64, tag u32, filename (u32 length + bytes), line
    i32, then a flag byte: 0 raw, 1 RLE (u64 compressed size + runs).  A file
    block (a ROM by content or reference) has no tag and no flag: size u64,
    filename, line, then `size` payload bytes."""
    with open(path, "rb") as f:
        d = f.read()
    if d[:8] != b"GSCHKPT2":
        raise ValueError("%s: not a v2 checkpoint (%r)" % (path, d[:8]))
    out = []
    i = HEADER_LEN
    while i < len(d):
        size = struct.unpack_from("<Q", d, i)[0]
        if d[i + 12 : i + 16] == b"src/":
            fl = struct.unpack_from("<I", d, i + 8)[0]
            fname = d[i + 12 : i + 12 + fl].decode()
            j = i + 12 + fl + 4
            out.append((fname, 0, "file", bytes(d[j : j + size])))
            i = j + size
            continue
        if d[i + 16 : i + 20] != b"src/":
            raise ValueError("%s: unparseable block header at %#x" % (path, i))
        tag = struct.unpack_from("<I", d, i + 8)[0]
        fl = struct.unpack_from("<I", d, i + 12)[0]
        fname = d[i + 16 : i + 16 + fl].decode()
        j = i + 16 + fl + 4
        flag = d[j]
        j += 1
        if flag == 0:
            data = bytes(d[j : j + size])
            j += size
        else:
            cs = struct.unpack_from("<Q", d, j)[0]
            j += 8
            comp = d[j : j + cs]
            j += cs
            buf = bytearray()
            ip = 0
            while ip < len(comp):
                m = comp[ip]
                c = struct.unpack_from("<I", comp, ip + 1)[0]
                ip += 5
                if m == 1:
                    buf += bytes([comp[ip]]) * c
                    ip += 1
                else:
                    buf += comp[ip : ip + c]
                    ip += c
            data = bytes(buf)
        out.append((fname, tag, "data", data))
        i = j
    return out


def looks_like_host_pointer(v):
    # User-space heap/stack addresses on 64-bit hosts; guest values never get there.
    return (1 << 40) <= v < (1 << 48)


def diff_block(fa, da, db):
    """Offsets where two same-identity blocks differ, host pointers masked."""
    mmusr = range(0x1C, 0x20) if (fa.endswith("cpu.c") and len(da) == 48) else range(0)
    stamps = [m.span() for m in BUILD_STAMP_RE.finditer(da)]
    bad = []
    n = len(da)
    k = 0
    while k < n:
        if k in mmusr:
            k += 1
            continue
        if da[k] != db[k]:
            if any(lo <= k < hi for lo, hi in stamps):
                k += 1
                continue
            w = k & ~7
            if w + 8 <= n:
                va = struct.unpack_from("<Q", da, w)[0]
                vb = struct.unpack_from("<Q", db, w)[0]
                if looks_like_host_pointer(va) and looks_like_host_pointer(vb):
                    k = w + 8
                    continue
            bad.append(k)
        k += 1
    return bad


def compare(a, b):
    """(same, [difference description, ...])."""
    if len(a) != len(b):
        return False, ["block count differs: %d vs %d" % (len(a), len(b))]
    diffs = []
    for idx, ((fa, ta, ka, da), (fb, tb, kb, db)) in enumerate(zip(a, b)):
        if (fa, ta, ka) != (fb, tb, kb):
            return False, diffs + ["block %d identity differs: %s/%s vs %s/%s" % (idx, fa, ka, fb, kb)]
        if "/storage/" in fa or ta == STORAGE_TAG or da == db:
            continue
        if len(da) != len(db):
            diffs.append("block %d (%s): size %d vs %d" % (idx, fa, len(da), len(db)))
            continue
        bad = diff_block(fa, da, db)
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
