#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) pappadf
"""gscheckpoint.py -- read a v2 (consolidated) checkpoint and compare guest state.

The one Python reader of the checkpoint format (the authoritative reader is
src/core/storage/checkpoint.c).  Two tools use it: scripts/cmp-checkpoints.py,
the predecoded cores' differential oracle (two runs of one build), and
scripts/bench/ckptdiff.py (two builds).

Masked by diff_block, because they are not guest state:
  * 8-byte words that hold a host pointer in both files (cpu_t/ppc_t and
    several device structs are written verbatim, pointers included; the
    reader nulls them, and ASLR plus a different heap history make them
    differ between any two processes);
  * the 68040's MMUSR, which mirrors the last table walk (fast-path state);
  * optionally, the GSDisk driver's build stamp, which differs between two
    builds only.
The storage layer's blocks (backing-file paths and delta ids) are skipped
whole by the callers: is_storage_block().

Self-test: python3 scripts/lib/gscheckpoint.py selftest
"""

import collections
import re
import struct
import sys

# The header: magic (8) + build id (BUILD_ID_LEN = 20); blocks follow.
MAGIC = b"GSCHKPT2"
HEADER_LEN = 8 + 20


class CheckpointFormatError(ValueError):
    """The file is not a v2 checkpoint this reader can walk."""


# One block: its writer's source file and line, its tag (0 for a file
# block), "data" or "file", and the decoded payload.
Block = collections.namedtuple("Block", "file line tag kind data")


def tag_hash(name):
    # checkpoint.c cp_tag_hash: FNV-1a over the block's name, 0 reserved.
    h = 2166136261
    for ch in name.encode():
        h = ((h ^ ch) * 16777619) & 0xFFFFFFFF
    return h or 1


# The machine's storage part (system.c): the attached devices' host-side
# records, skipped with the storage layer's own blocks.
STORAGE_TAG = tag_hash("storage")

# The GSDisk driver's build stamp (__DATE__ __TIME__), as it appears in RAM.
BUILD_STAMP_RE = re.compile(rb"GSDisk build [A-Z][a-z]{2} [ \d]\d \d{4} \d\d:\d\d:\d\d")


def rle_decode(comp):
    # Runs of: mode u8 (1 = repeat one byte, else literal), count u32, payload.
    out = bytearray()
    ip = 0
    while ip < len(comp):
        m = comp[ip]
        c = struct.unpack_from("<I", comp, ip + 1)[0]
        ip += 5
        if m == 1:
            out += bytes([comp[ip]]) * c
            ip += 1
        else:
            out += comp[ip : ip + c]
            ip += c
    return bytes(out)


def decode_bytes(d, name="checkpoint"):
    """[Block] for every block of a v2 checkpoint held in `d`.

    A data block is size u64, tag u32, filename (u32 length + bytes), line
    i32, then a flag byte: 0 raw, 1 RLE (u64 compressed size + runs).  A file
    block (checkpoint_write_file: a ROM by content or reference) has no tag
    and no flag: size u64, filename, line, then `size` payload bytes."""
    if d[:8] != MAGIC:
        raise CheckpointFormatError("%s: not a v2 checkpoint (%r)" % (name, d[:8]))
    out = []
    i = HEADER_LEN
    try:
        while i < len(d):
            size = struct.unpack_from("<Q", d, i)[0]
            if d[i + 12 : i + 16] == b"src/":
                fl = struct.unpack_from("<I", d, i + 8)[0]
                fname = d[i + 12 : i + 12 + fl].decode()
                j = i + 12 + fl
                line = struct.unpack_from("<i", d, j)[0]
                j += 4
                if j + size > len(d):
                    raise CheckpointFormatError("%s: file block at %#x runs past the end" % (name, i))
                out.append(Block(fname, line, 0, "file", bytes(d[j : j + size])))
                i = j + size
                continue
            if d[i + 16 : i + 20] != b"src/":
                raise CheckpointFormatError("%s: unparseable block header at %#x" % (name, i))
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
                data = rle_decode(d[j : j + cs])
                j += cs
            if j > len(d) or len(data) != size:
                raise CheckpointFormatError("%s: data block at %#x is truncated" % (name, i))
            out.append(Block(fname, line, tag, "data", data))
            i = j
    except (struct.error, IndexError, UnicodeDecodeError) as e:
        raise CheckpointFormatError("%s: truncated or corrupt at %#x (%s)" % (name, i, e))
    return out


def decode_blocks(path):
    """[Block] for every block of the v2 checkpoint at `path`."""
    with open(path, "rb") as f:
        return decode_bytes(f.read(), path)


def is_storage_block(b):
    # Host-side backing-file bookkeeping (instance paths, delta ids): not
    # guest state.  The guest-visible disk content is driven by the very
    # RAM/register timeline the callers compare.
    return "/storage/" in b.file or b.tag == STORAGE_TAG


def looks_like_host_pointer(v):
    # User-space heap/stack addresses on 64-bit hosts (0x5xxx_xxxx_xxxx and
    # 0x7fxx_xxxx_xxxx are the common ranges); guest values never get there.
    return (1 << 40) <= v < (1 << 48)


def diff_block(fname, da, db, mask_build_stamp=False):
    """Offsets where two same-identity, same-size blocks differ, masked."""
    # The 68040 MMU register file (cpu.c's second block, 48 bytes): MMUSR
    # at +0x1c mirrors the LAST table walk (mmu040.c publishes it on every
    # walk, not only on PTEST), and which accesses walk depends on the
    # fast-path fill state, which the code-page marks change.  The guest
    # only reads it from a bus-error handler, where both executors walked
    # the same faulting address, so the word is not part of the timeline.
    mmusr = range(0x1C, 0x20) if (fname.endswith("cpu.c") and len(da) == 48) else range(0)
    stamps = [m.span() for m in BUILD_STAMP_RE.finditer(da)] if mask_build_stamp else []
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
            # Host pointers: 8-byte-aligned words pointer-shaped in both files.
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


# --- self-test ---------------------------------------------------------------


def _data_block(fname, line, tag, payload, rle=False):
    # Encode one data block the way checkpoint.c writes it.
    fb = fname.encode()
    head = struct.pack("<QII", len(payload), tag, len(fb)) + fb + struct.pack("<i", line)
    if not rle:
        return head + b"\0" + payload
    # One repeat run per byte value change: enough to exercise the decoder.
    runs = bytearray()
    k = 0
    while k < len(payload):
        e = k
        while e < len(payload) and payload[e] == payload[k]:
            e += 1
        runs += struct.pack("<BI", 1, e - k) + payload[k : k + 1]
        k = e
    return head + b"\1" + struct.pack("<Q", len(runs)) + bytes(runs)


def _file_block(fname, line, payload):
    fb = fname.encode()
    return struct.pack("<QI", len(payload), len(fb)) + fb + struct.pack("<i", line) + payload


def selftest():
    ptr_a = struct.pack("<Q", 0x5555_1234_5678)
    ptr_b = struct.pack("<Q", 0x7FFF_8765_4321)
    cpu_a = b"\x11" * 0x1C + b"\xAA" * 4 + b"\x22" * 16
    cpu_b = b"\x11" * 0x1C + b"\xBB" * 4 + b"\x22" * 16
    ram_a = b"\0" * 64 + b"GSDisk build Nov 19 2026 10:00:00" + b"\0" * 7
    ram_b = b"\0" * 64 + b"GSDisk build Nov 20 2026 11:22:33" + b"\0" * 7

    def image(cpu, ptr, ram, guest):
        return (MAGIC + b"\0" * 20
                + _file_block("src/core/memory/rom.c", 10, b"ROM!")
                + _data_block("src/core/cpu/cpu.c", 40, tag_hash("cpu"), cpu)
                + _data_block("src/core/peripherals/via.c", 77, tag_hash("via"), ptr + guest)
                + _data_block("src/core/memory/memory.c", 90, tag_hash("ram"), ram, rle=True)
                + _data_block("src/core/storage/image.c", 5, tag_hash("image"), b"/tmp/a"))

    a = decode_bytes(image(cpu_a, ptr_a, ram_a, b"\1\2"), "a")
    b = decode_bytes(image(cpu_b, ptr_b, ram_b, b"\1\3"), "b")
    assert [x.kind for x in a] == ["file", "data", "data", "data", "data"], a
    assert a[0] == Block("src/core/memory/rom.c", 10, 0, "file", b"ROM!")
    assert a[3].data == ram_a, "RLE decode"
    assert is_storage_block(a[4]) and not is_storage_block(a[3])
    assert diff_block(a[1].file, a[1].data, b[1].data) == [], "MMUSR masked"
    assert diff_block(a[2].file, a[2].data, b[2].data) == [9], "host pointer masked, guest byte kept"
    assert diff_block(a[3].file, a[3].data, b[3].data) != [], "stamp kept by default"
    assert diff_block(a[3].file, a[3].data, b[3].data, mask_build_stamp=True) == []
    try:
        decode_bytes(b"GSCHKPT1" + b"\0" * 20, "old")
        raise AssertionError("v1 accepted")
    except CheckpointFormatError:
        pass
    try:
        decode_bytes(image(cpu_a, ptr_a, ram_a, b"\1\2")[:-3], "cut")
        raise AssertionError("truncated file accepted")
    except CheckpointFormatError:
        pass
    print("gscheckpoint selftest: ok")


if __name__ == "__main__":
    if sys.argv[1:] == ["selftest"]:
        selftest()
    else:
        sys.exit("usage: gscheckpoint.py selftest")
