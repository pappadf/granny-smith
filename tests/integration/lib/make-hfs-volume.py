#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) pappadf

"""Build a classic HFS volume holding the files given -- optionally storing a
disk image as an NDIF (Disk Copy 6) file, its chunk map in a 'bcem' resource.

Written from Inside Macintosh: Files (the master directory block, the volume
bitmap, the catalog and extents B-trees) and from the NDIF layout (a 128-byte
'bcem' header, 12-byte chunk descriptors: (first sector << 8) | type, data
fork offset, length), not from the emulator's readers -- so a test where the
emulator reads what this wrote checks one against the other.  7-Zip reads
the volumes this writes, which checks the HFS side against a third reader.

Every file is one contiguous extent in the root folder.  NDIF chunks are
written as zero-fill, copy, and ADC (as literal runs: a valid ADC stream that
exercises the decoder without needing a compressor), in rotation.

Usage: make-hfs-volume.py <out.img> <size-KiB> <name>=<host-file>[@ndif] ...
"""

import struct
import sys

BLOCK = 512
EPOCH = 0xB492F400  # a fixed Mac-epoch date, so the volume is repeatable


def pstr(s, n):
    b = s.encode("mac_roman")
    return bytes([len(b)]) + b + b"\0" * (n - 1 - len(b))


# ---- NDIF ------------------------------------------------------------------

NDIF_ZERO, NDIF_COPY, NDIF_ADC, NDIF_END = 0x00, 0x02, 0x83, 0xFF
CHUNK_SECTORS = 64


def adc_literals(data):
    out = bytearray()
    for i in range(0, len(data), 128):
        run = data[i:i + 128]
        out.append(0x80 | (len(run) - 1))
        out += run
    return bytes(out)


def ndif(raw, name):
    """Return (data fork, resource fork) of an NDIF image of `raw`."""
    sectors = len(raw) // BLOCK
    data = bytearray()
    descs = []
    for k, s in enumerate(range(0, sectors, CHUNK_SECTORS)):
        chunk = raw[s * BLOCK:(s + CHUNK_SECTORS) * BLOCK]
        if not any(chunk):
            descs.append((s << 8 | NDIF_ZERO, 0, 0))
            continue
        kind = NDIF_ADC if k % 2 else NDIF_COPY
        body = adc_literals(chunk) if kind == NDIF_ADC else chunk
        descs.append((s << 8 | kind, len(data), len(body)))
        data += body
    descs.append((sectors << 8 | NDIF_END, 0, 0))
    bcem = bytearray(128)
    struct.pack_into(">HH", bcem, 0, 11, 0)  # version, driver
    nb = name.encode("mac_roman")[:63]
    bcem[4] = len(nb)
    bcem[5:5 + len(nb)] = nb
    struct.pack_into(">IIII", bcem, 0x44, sectors, 128, 0, 0)
    struct.pack_into(">I", bcem, 0x7C, len(descs))
    for word, off, length in descs:
        bcem += struct.pack(">III", word, off, length)
    return bytes(data), resource_fork(b"bcem", 128, bytes(bcem))


def resource_fork(rtype, rid, body):
    """A resource fork holding one resource."""
    data_area = struct.pack(">I", len(body)) + body
    tlo, ref = 28, 28 + 2 + 8
    nlo = ref + 12
    mp = bytearray(nlo)
    struct.pack_into(">HH", mp, 24, tlo, nlo)
    struct.pack_into(">H", mp, tlo, 0)  # one type
    mp[tlo + 2:tlo + 6] = rtype
    struct.pack_into(">HH", mp, tlo + 6, 0, ref - tlo)  # one resource
    struct.pack_into(">hhB3sI", mp, ref, rid, -1, 0, b"\0\0\0", 0)
    hdr = struct.pack(">IIII", 256, 256 + len(data_area), len(data_area), len(mp))
    struct.pack_into(">IIII", mp, 0, 256, 256 + len(data_area), len(data_area), len(mp))
    return hdr + b"\0" * (256 - 16) + data_area + bytes(mp)


# ---- HFS -------------------------------------------------------------------

def btree_header(node_size, key_len, depth, root, nrecs, first, last, nnodes, used_nodes):
    node = bytearray(node_size)
    struct.pack_into(">IIbBHH", node, 0, 0, 0, 1, 0, 3, 0)  # header node, 3 records
    struct.pack_into(">HIIIIHHII", node, 14, depth, root, nrecs, first, last, node_size, key_len, nnodes,
                     nnodes - used_nodes)
    # Records: header (14), user data (120), map (248); offsets from the end.
    mp = 248
    for n in range(used_nodes):
        node[mp + n // 8] |= 0x80 >> (n % 8)
    struct.pack_into(">HHHH", node, node_size - 8, node_size - 8, mp, 120, 14)
    return node


def cat_key(parent, name):
    nb = name.encode("mac_roman")
    key = struct.pack(">BI", 0, parent) + bytes([len(nb)]) + nb
    key = bytes([len(key)]) + key
    if len(key) % 2:
        key += b"\0"
    return key


def main():
    out, size_kib, specs = sys.argv[1], int(sys.argv[2]), sys.argv[3:]
    total = size_kib * 1024 // BLOCK
    files = []
    for spec in specs:
        name, host = spec.split("=", 1)
        as_ndif = host.endswith("@ndif")
        if as_ndif:
            host = host[:-5]
        with open(host, "rb") as f:
            raw = f.read()
        if as_ndif:
            data, rsrc = ndif(raw, name)
            files.append((name, data, rsrc, b"rohd", b"ddsk"))
        else:
            files.append((name, raw, b"", b"dImg", b"dCpy"))

    al_st = 4  # allocation blocks start after the boot blocks, MDB and bitmap
    n_al = total - al_st - 2  # the last two blocks: the alternate MDB and spare
    vol = bytearray(total * BLOCK)
    nxt = 0

    def alloc(nbytes):
        nonlocal nxt
        count = (nbytes + BLOCK - 1) // BLOCK
        start = nxt
        nxt += count
        assert nxt <= n_al, "volume too small"
        return start, count

    def put(alblk, data):
        at = (al_st + alblk) * BLOCK
        vol[at:at + len(data)] = data

    # Extents B-tree: a header node and nothing else.  Catalog: header + leaf.
    xt = alloc(BLOCK)
    ct = alloc(2 * BLOCK)
    put(xt[0], btree_header(BLOCK, 7, 0, 0, 0, 0, 0, 1, 1))
    put(ct[0], btree_header(BLOCK, 37, 1, 1, 2 + len(files), 1, 1, 2, 2))

    recs = []
    vname = "GS Volume"
    root = struct.pack(">BBHHIIII", 1, 0, 0, len(files), 2, EPOCH, EPOCH, 0) + b"\0" * 48
    recs.append((cat_key(1, vname), root))
    thread = struct.pack(">BB8xI", 3, 0, 1) + pstr(vname, 32)
    recs.append((cat_key(2, ""), thread))
    cnid = 16
    for name, data, rsrc, ftype, creator in sorted(files, key=lambda f: f[0].upper()):
        d = alloc(len(data)) if data else (0, 0)
        r = alloc(len(rsrc)) if rsrc else (0, 0)
        if data:
            put(d[0], data)
        if rsrc:
            put(r[0], rsrc)
        rec = struct.pack(">BBBB", 2, 0, 0, 0) + ftype + creator + b"\0" * 8
        rec += struct.pack(">IHIIHII", cnid, 0, len(data), d[1] * BLOCK, 0, len(rsrc), r[1] * BLOCK)
        rec += struct.pack(">III", EPOCH, EPOCH, 0) + b"\0" * 16 + struct.pack(">H", 0)
        rec += struct.pack(">HHHHHH", d[0], d[1], 0, 0, 0, 0)
        rec += struct.pack(">HHHHHH", r[0], r[1], 0, 0, 0, 0) + b"\0" * 4
        recs.append((cat_key(2, name), rec))
        cnid += 1

    leaf = bytearray(BLOCK)
    struct.pack_into(">IIbBHH", leaf, 0, 0, 0, -1, 1, len(recs), 0)
    pos, offs = 14, []
    for key, body in recs:
        offs.append(pos)
        leaf[pos:pos + len(key) + len(body)] = key + body
        pos += len(key) + len(body)
    offs.append(pos)
    for i, o in enumerate(offs):
        struct.pack_into(">H", leaf, BLOCK - 2 * (i + 1), o)
    put(ct[0] + 1, leaf)

    # Bitmap and master directory block.
    for b in range(nxt):
        vol[3 * BLOCK + b // 8] |= 0x80 >> (b % 8)
    mdb = bytearray(BLOCK)
    struct.pack_into(">HIIHHHHHIIHIH", mdb, 0, 0x4244, EPOCH, EPOCH, 0x0100, len(files), 3, nxt, n_al, BLOCK,
                     4 * BLOCK, al_st, cnid, n_al - nxt)
    mdb[36:64] = pstr(vname, 28)
    struct.pack_into(">IHIII", mdb, 64, 0, 0, 0, BLOCK, BLOCK)
    struct.pack_into(">HII", mdb, 82, 0, len(files), 0)
    struct.pack_into(">I", mdb, 130, xt[1] * BLOCK)
    struct.pack_into(">HH", mdb, 134, xt[0], xt[1])
    struct.pack_into(">I", mdb, 146, ct[1] * BLOCK)
    struct.pack_into(">HH", mdb, 150, ct[0], ct[1])
    vol[2 * BLOCK:3 * BLOCK] = mdb
    vol[(total - 2) * BLOCK:(total - 1) * BLOCK] = mdb
    with open(out, "wb") as f:
        f.write(vol)


if __name__ == "__main__":
    main()
