#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) pappadf

"""Build an ISO 9660 disc image, with a Joliet tree, from files given.

Written from ECMA-119 and the Joliet specification rather than from
src/core/storage/image_iso9660.c, so the two are independent: if the
emulator's reader lists and reads what this writer laid down, both are
probably right.  The disc has a primary volume descriptor (8.3 names,
";1" versions, upper case), a Joliet supplementary descriptor (UCS-2 long
names) and both trees' path tables, as mastering tools write them.

Two options, from Apple's practice rather than ECMA-119:
  --finder <iso-path>=TYPE/CREA  give that file Apple's "AA" system use entry
                                 (Apple Extensions to ISO 9660: type, creator
                                 and Finder flags -- the bundle bit, here)
  --hybrid-hfs <volume>          make the disc a hybrid: a driver descriptor
                                 and Apple Partition Map in the ISO system
                                 area, and the HFS volume given as an
                                 Apple_HFS partition after the ISO data, as
                                 Apple's hybrid mastering lays a disc out
  --finder-out <iso-path>=<file> write the 32 Finder info bytes a reader
                                 should report for that file

Usage: make-fixture.py [options] <out.iso> <iso-path>=<host-file> ...
  An iso-path may contain one '/' (a file in a folder).
"""

import struct
import sys

SECTOR = 2048


def both16(v):
    return struct.pack("<H", v) + struct.pack(">H", v)


def both32(v):
    return struct.pack("<I", v) + struct.pack(">I", v)


FINDER_FLAGS = 0x2000  # hasBundle: one of the bits Apple's extensions carry


def apple_aa(finder):
    """Apple's "AA" entry: signature, length 14, id 2 (HFS), type, creator,
    Finder flags."""
    ftype, creator = finder
    return b"AA" + bytes([14, 2]) + ftype + creator + struct.pack(">H", FINDER_FLAGS)


def dir_record(ident, extent, size, is_dir, su=b""):
    rec = bytearray(33)
    rec[1] = 0
    rec[2:10] = both32(extent)
    rec[10:18] = both32(size)
    rec[18:25] = bytes([101, 1, 2, 3, 4, 5, 0])  # 2001-01-02 03:04:05 GMT
    rec[25] = 0x02 if is_dir else 0
    rec[28:32] = both16(1)
    rec[32] = len(ident)
    rec += ident
    if len(ident) % 2 == 0:
        rec += b"\0"
    rec += su
    rec += b"\0" * (len(rec) % 2)
    rec[0] = len(rec)
    return bytes(rec)


def iso_name(name, is_dir):
    base = name.upper().replace(" ", "_").replace("-", "_")
    if is_dir:
        return base[:31].encode()
    stem, _, ext = base.rpartition(".") if "." in base else (base, "", "")
    return (stem[:8] + "." + ext[:3] + ";1").encode()


def joliet_name(name, is_dir):
    return (name if is_dir else name + ";1").encode("utf-16-be")


def directory(entries, self_extent, parent_extent, namefn):
    """Records of one directory: '.', '..', then entries, never crossing a
    sector; returns (bytes padded to whole sectors)."""
    recs = [dir_record(b"\0", self_extent, 0, True), dir_record(b"\1", parent_extent, 0, True)]
    for name, is_dir, extent, size, finder in entries:
        su = apple_aa(finder) if finder else b""
        recs.append(dir_record(namefn(name, is_dir), extent, size, is_dir, su))
    out = bytearray()
    for r in recs:
        if len(out) % SECTOR + len(r) > SECTOR:
            out += b"\0" * (SECTOR - len(out) % SECTOR)
        out += r
    out += b"\0" * (-len(out) % SECTOR)
    return out


def put_be(buf, off, fmt, *v):
    struct.pack_into(">" + fmt, buf, off, *v)


def apm_entry(map_blocks, start, count, name, ptype):
    e = bytearray(512)
    e[0:2] = b"PM"
    put_be(e, 4, "III", map_blocks, start, count)
    e[16:16 + len(name)] = name.encode()
    e[48:48 + len(ptype)] = ptype.encode()
    put_be(e, 0x54, "II", 0, count)  # logical data start and count
    put_be(e, 0x58, "I", 0x33)  # valid, allocated, in use, readable
    return e


def main():
    args = sys.argv[1:]
    finder, finder_out, hybrid_hfs = {}, {}, None
    while args and args[0].startswith("--"):
        opt, val = args[0], args[1]
        args = args[2:]
        if opt == "--finder":
            path, tc = val.split("=", 1)
            ftype, creator = tc.split("/", 1)
            finder[path] = (ftype.encode("mac_roman"), creator.encode("mac_roman"))
        elif opt == "--finder-out":
            path, host = val.split("=", 1)
            finder_out[path] = host
        elif opt == "--hybrid-hfs":
            hybrid_hfs = val
        else:
            sys.exit("unknown option " + opt)
    out_path, specs = args[0], args[1:]
    files = []  # (folder or None, name, bytes)
    for spec in specs:
        iso_path, host = spec.split("=", 1)
        folder, _, name = iso_path.rpartition("/")
        with open(host, "rb") as f:
            files.append((folder or None, name, f.read()))
    folders = sorted({f for f, _, _ in files if f})

    # Layout: 16 system sectors, PVD, SVD, terminator, 4 path-table sectors,
    # then each tree's directories, then the file data.
    sec = 16 + 3 + 4
    trees = {}
    for tree in ("iso", "joliet"):
        trees[tree] = {"root": sec}
        sec += 1
        for fo in folders:
            trees[tree][fo] = sec
            sec += 1
    extents = {}
    for folder, name, data in files:
        extents[(folder, name)] = sec
        sec += (len(data) + SECTOR - 1) // SECTOR
    total = sec
    disc = bytearray(total * SECTOR)

    def put(sector, data):
        disc[sector * SECTOR:sector * SECTOR + len(data)] = data

    for tree, namefn in (("iso", iso_name), ("joliet", joliet_name)):
        t = trees[tree]
        entries = [(fo, True, t[fo], SECTOR, None) for fo in folders]
        entries += [(n, False, extents[(None, n)], len(d), finder.get(n)) for f, n, d in files if f is None]
        put(t["root"], directory(entries, t["root"], t["root"], namefn))
        for fo in folders:
            sub = [(n, False, extents[(fo, n)], len(d), finder.get(fo + "/" + n)) for f, n, d in files if f == fo]
            put(t[fo], directory(sub, t[fo], t["root"], namefn))
    for folder, name, data in files:
        put(extents[(folder, name)], data)

    # Path tables (ECMA-119 9.4): the root, then each folder; L in little
    # endian at sector 19 (+20 for Joliet), M in big endian at 21 (+22).
    def path_table(tree, namefn, big):
        t = trees[tree]
        pt = bytearray()
        rows = [(b"\0", t["root"], 1)] + [(namefn(fo, True), t[fo], 1) for fo in folders]
        for ident, extent, parent in rows:
            fmt = ">IH" if big else "<IH"
            pt += bytes([len(ident), 0]) + struct.pack(fmt, extent, parent) + ident
            if len(ident) % 2:
                pt += b"\0"
        return bytes(pt)

    pts = {}
    for i, (tree, namefn) in enumerate((("iso", iso_name), ("joliet", joliet_name))):
        lpt, mpt = path_table(tree, namefn, False), path_table(tree, namefn, True)
        put(19 + i, lpt)
        put(21 + i, mpt)
        pts[tree] = (len(lpt), 19 + i, 21 + i)

    def descriptor(kind, tree, ident_enc):
        vd = bytearray(SECTOR)
        vd[0] = kind
        vd[1:6] = b"CD001"
        vd[6] = 1
        vd[8:40] = ident_enc("GS-SYSTEM", 32)
        vd[40:72] = ident_enc("GS_TEST_DISC", 32)
        vd[80:88] = both32(total)
        if kind == 2:
            vd[88:91] = b"%/E"  # Joliet, UCS-2 level 3
        vd[120:124] = both16(1)
        vd[124:128] = both16(1)
        vd[128:132] = both16(SECTOR)
        size, l_sec, m_sec = pts[tree]
        vd[132:140] = both32(size)
        vd[140:144] = struct.pack("<I", l_sec)
        vd[148:152] = struct.pack(">I", m_sec)
        vd[156:190] = dir_record(b"\0", trees[tree]["root"], SECTOR, True)
        vd[881] = 1  # file structure version
        return vd

    def a_chars(s, n):
        return s.encode().ljust(n, b" ")

    put(16, descriptor(1, "iso", a_chars))
    put(17, descriptor(2, "joliet", lambda s, n: (s.encode("utf-16-be") + b"\0 " * n)[:n]))
    term = bytearray(SECTOR)
    term[0] = 255
    term[1:6] = b"CD001"
    term[6] = 1
    put(18, term)

    if hybrid_hfs:
        # Blocks 0-2 (of 512 bytes) sit in the ISO system area, which ISO
        # leaves to the platform: the driver descriptor ("ER") and a map of
        # two entries, the map itself and the HFS volume after the ISO data.
        with open(hybrid_hfs, "rb") as f:
            hfs = f.read()
        hfs_start = total * SECTOR // 512
        disc += hfs
        blocks = len(disc) // 512
        ddm = bytearray(512)
        ddm[0:2] = b"ER"
        put_be(ddm, 2, "HI", 512, blocks)
        disc[0:512] = ddm
        disc[512:1024] = apm_entry(2, 1, 2, "Apple", "Apple_partition_map")
        disc[1024:1536] = apm_entry(2, hfs_start, len(hfs) // 512, "Hybrid", "Apple_HFS")

    for path, host in finder_out.items():
        ftype, creator = finder[path]
        with open(host, "wb") as f:
            f.write(ftype + creator + struct.pack(">H", FINDER_FLAGS) + b"\0" * 22)

    with open(out_path, "wb") as f:
        f.write(disc)


if __name__ == "__main__":
    main()
