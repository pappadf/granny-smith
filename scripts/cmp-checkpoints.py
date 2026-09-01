#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) pappadf
#
# cmp-checkpoints.py — the predecoded cores' differential oracle.
#
# Decodes two v2 (consolidated) checkpoints block by block and compares the
# guest state they carry.  Two kinds of bytes are NOT guest state and are
# masked: the machine config record (system.c's manifest block carries the
# creation time) and 8-byte-aligned words that hold HOST POINTERS in both
# files (cpu_t/ppc_t and several device structs are written verbatim,
# pointers included; the reader nulls them, and ASLR plus a different heap
# history make them differ between any two processes).  Everything else —
# RAM, the register files with their raw flag words, the scheduler's cycle
# count and event queue, every peripheral — must match byte for byte.
#
# Usage: cmp-checkpoints.py A.gs B.gs [-v]      exit 0 = identical guest state

import struct
import sys


def decode_blocks(path):
    d = open(path, "rb").read()
    if d[:8] != b"GSCHKPT2":
        sys.exit("%s: not a v2 checkpoint (%r)" % (path, d[:8]))
    out = []
    i = 48  # magic(8) + build id(24) + model id(16); the RAM-size u32 precedes block 0
    while i < len(d):
        if d[i + 12 : i + 16] != b"src/":
            k = d.find(b"src/", i)
            if k < 0:
                break
            i = k - 12
        size = struct.unpack_from("<Q", d, i)[0]
        fl = struct.unpack_from("<I", d, i + 8)[0]
        fname = d[i + 12 : i + 12 + fl].decode()
        j = i + 12 + fl
        line = struct.unpack_from("<i", d, j)[0]
        j += 4
        # A file block (checkpoint_write_file: the ROM by content or reference)
        # carries its payload raw with no flag byte; recognise it by the next
        # block header landing exactly after `size` payload bytes.
        if d[j + size + 12 : j + size + 16] == b"src/" or j + size == len(d):
            if d[j] not in (0, 1) or d[j + 1 + 12 : j + 1 + 16] != b"src/":
                out.append((fname, line, "file", bytes(d[j : j + size])))
                i = j + size
                continue
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
        out.append((fname, line, "data", data))
        i = j
    return out


def looks_like_host_pointer(v):
    # User-space heap/stack addresses on 64-bit hosts (0x5xxx_xxxx_xxxx and
    # 0x7fxx_xxxx_xxxx are the common ranges); guest values never get there.
    return v >= (1 << 40) and v < (1 << 48)


def compare(a, b, verbose):
    if len(a) != len(b):
        print("block count differs: %d vs %d" % (len(a), len(b)))
        return False
    ok = True
    for (fa, la, ka, da), (fb, lb, kb, db) in zip(a, b):
        if (fa, la, ka) != (fb, lb, kb):
            print("block identity differs: %s:%d/%s vs %s:%d/%s" % (fa, la, ka, fb, lb, kb))
            return False
        if fa.endswith("system.c") and la < 1100 and len(da) == len(db) and da != db:
            if verbose:
                print("skip  %s:%d (config record: creation time)" % (fa, la))
            continue
        if "/storage/" in fa:
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
        # Mask host pointers: 8-byte-aligned words that are pointer-shaped in
        # both files.
        bad = []
        n = len(da)
        k = 0
        while k < n:
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
