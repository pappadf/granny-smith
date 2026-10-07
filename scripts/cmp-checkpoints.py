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

import struct
import sys


# The header: magic (8) + build id (BUILD_ID_LEN = 20); blocks follow.
HEADER_LEN = 8 + 20


def tag_hash(name):
    # checkpoint.c cp_tag_hash: FNV-1a over the block's name, 0 reserved.
    h = 2166136261
    for ch in name.encode():
        h = ((h ^ ch) * 16777619) & 0xFFFFFFFF
    return h or 1


def decode_blocks(path):
    """[(file, line, tag, kind, data)] for every block of a v2 checkpoint.

    A data block is size u64, tag u32, filename (u32 length + bytes), line
    i32, then a flag byte: 0 raw, 1 RLE (u64 compressed size + runs).  A file
    block (checkpoint_write_file: a ROM by content or reference) has no tag
    and no flag: size u64, filename, line, then `size` payload bytes."""
    d = open(path, "rb").read()
    if d[:8] != b"GSCHKPT2":
        sys.exit("%s: not a v2 checkpoint (%r)" % (path, d[:8]))
    out = []
    i = HEADER_LEN
    while i < len(d):
        size = struct.unpack_from("<Q", d, i)[0]
        if d[i + 12 : i + 16] == b"src/":
            fl = struct.unpack_from("<I", d, i + 8)[0]
            fname = d[i + 12 : i + 12 + fl].decode()
            j = i + 12 + fl
            line = struct.unpack_from("<i", d, j)[0]
            j += 4
            out.append((fname, line, 0, "file", bytes(d[j : j + size])))
            i = j + size
            continue
        if d[i + 16 : i + 20] != b"src/":
            sys.exit("%s: unparseable block header at %#x" % (path, i))
        tag = struct.unpack_from("<I", d, i + 8)[0]
        fl = struct.unpack_from("<I", d, i + 12)[0]
        fname = d[i + 16 : i + 16 + fl].decode()
        j = i + 16 + fl
        line = struct.unpack_from("<i", d, j)[0]
        j += 4
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
            data = bytearray()
            ip = 0
            while ip < len(comp):
                m = comp[ip]
                c = struct.unpack_from("<I", comp, ip + 1)[0]
                ip += 5
                if m == 1:
                    data += bytes([comp[ip]]) * c
                    ip += 1
                else:
                    data += comp[ip : ip + c]
                    ip += c
            data = bytes(data)
        out.append((fname, line, tag, "data", data))
        i = j
    return out


# The machine's storage part (system.c): the attached devices' host-side
# records, masked with the storage layer's own blocks below.
STORAGE_TAG = tag_hash("storage")


def looks_like_host_pointer(v):
    # User-space heap/stack addresses on 64-bit hosts (0x5xxx_xxxx_xxxx and
    # 0x7fxx_xxxx_xxxx are the common ranges); guest values never get there.
    return v >= (1 << 40) and v < (1 << 48)


def compare(a, b, verbose):
    if len(a) != len(b):
        print("block count differs: %d vs %d" % (len(a), len(b)))
        return False
    ok = True
    for (fa, la, ta, ka, da), (fb, lb, tb, kb, db) in zip(a, b):
        if (fa, la, ta, ka) != (fb, lb, tb, kb):
            print("block identity differs: %s:%d/%s vs %s:%d/%s" % (fa, la, ka, fb, lb, kb))
            return False
        if "/storage/" in fa or ta == STORAGE_TAG:
            # Host-side backing-file bookkeeping (instance paths, delta ids):
            # not guest state.  The guest-visible disk content is driven by the
            # very RAM/register timeline compared here.
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
        # The 68040 MMU register file (cpu.c's second block, 48 bytes): MMUSR
        # at +0x1c mirrors the LAST table walk (mmu040.c publishes it on every
        # walk, not only on PTEST), and which accesses walk depends on the
        # fast-path fill state — which the code-page marks change.  The guest
        # only reads it from a bus-error handler, where both executors walked
        # the same faulting address, so the word is not part of the timeline.
        mmusr = range(0x1C, 0x20) if (fa.endswith("cpu.c") and len(da) == 48) else range(0)
        # Mask host pointers: 8-byte-aligned words that are pointer-shaped in
        # both files.
        bad = []
        n = len(da)
        k = 0
        while k < n:
            if k in mmusr:
                k += 1
                continue
            if da[k] != db[k]:
                w = k & ~7
                if w + 8 <= n:
                    va = struct.unpack_from("<Q", da, w)[0]
                    vb = struct.unpack_from("<Q", db, w)[0]
                    if looks_like_host_pointer(va) and looks_like_host_pointer(vb):
                        k = w + 8
                        continue
                bad.append(k)
            k += 1
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
    a = decode_blocks(args[0])
    b = decode_blocks(args[1])
    if compare(a, b, verbose):
        print("identical guest state (%d blocks)" % len(a))
        sys.exit(0)
    sys.exit(1)


if __name__ == "__main__":
    main()
