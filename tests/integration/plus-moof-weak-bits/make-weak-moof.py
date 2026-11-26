#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) pappadf
#
# Turn a writer-made MOOF (a plain 800K disk laid down as a Mac formats it)
# into one carrying a weak-bit copy protection: on track WEAK_TRACK, side
# WEAK_SIDE, sector WEAK_SECTOR's data field (its mark included) becomes a
# weak area -- RECORDS records
#
#     F6 AF xx yy 96 F6   then 16 cells with no flux transition
#
# (a 17-cell run with the last byte's own zero), record i numbered by
# xx = codeword(i % 64), yy = $96 + i // 64, then self-sync to where the
# field ended.  The sector's address field stays, so the Sony driver finds
# it and reads on into the area.  The track is then turned so its index
# falls inside the area, as it may on a real capture, and INFO's
# write-protect flag is set, as on an original disk.  The header CRC is
# recomputed.  Usage: make-weak-moof.py in.moof out.moof

import struct
import sys
import zlib

WEAK_TRACK, WEAK_SIDE, WEAK_SECTOR = 26, 1, 0
RECORDS = 88  # 64 cells each in the field's 709 bytes, the rest sync
FIELD_CELLS = (3 + 1 + 703 + 2) * 8
INDEX_AT = 37 * 64 + 56  # where the index falls: inside record 37's run

GCR = [0x96, 0x97, 0x9a, 0x9b, 0x9d, 0x9e, 0x9f, 0xa6, 0xa7, 0xab, 0xac, 0xad, 0xae, 0xaf, 0xb2, 0xb3,
       0xb4, 0xb5, 0xb6, 0xb7, 0xb9, 0xba, 0xbb, 0xbc, 0xbd, 0xbe, 0xbf, 0xcb, 0xcd, 0xce, 0xcf, 0xd3,
       0xd6, 0xd7, 0xd9, 0xda, 0xdb, 0xdc, 0xdd, 0xde, 0xdf, 0xe5, 0xe6, 0xe7, 0xe9, 0xea, 0xeb, 0xec,
       0xed, 0xee, 0xef, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf9, 0xfa, 0xfb, 0xfc, 0xfd, 0xfe, 0xff]
INV = {v: i for i, v in enumerate(GCR)}


def track_at(f, t, s):
    """Byte offset of (t, s)'s bits and its bit count."""
    idx = f[88 + t * 2 + s]
    assert idx != 0xFF, f'track {t} side {s} is blank'
    start, _, nbits = struct.unpack_from('<HHI', f, 256 + idx * 8)
    return start * 512, nbits


def value(cells, i, n):
    """`n` cells from cell `i`, as a number."""
    v = 0
    for k in range(i, i + n):
        v = v << 1 | cells[k]
    return v


def put(cells, i, v, n):
    """Overwrite `n` cells from cell `i` with the low `n` bits of `v`."""
    for k in range(n):
        cells[i + k] = (v >> (n - 1 - k)) & 1


def data_mark(cells, sector):
    """The cell where `sector`'s data mark starts (the writer lays bytes
    on 8-cell boundaries inside fields)."""
    n = len(cells)
    for i in range(n - 48):
        if value(cells, i, 24) != 0xD5AA96 or INV.get(value(cells, i + 32, 8)) != sector:
            continue
        for j in range(i + 24, n - 24):
            if value(cells, j, 24) == 0xD5AAAD:
                return j
    raise SystemExit(f'sector {sector}: no data mark')


def main():
    f = bytearray(open(sys.argv[1], 'rb').read())
    assert f[:8] == b'MOOF\xff\n\r\n'
    at, n = track_at(f, WEAK_TRACK, WEAK_SIDE)
    cells = [(f[at + (k >> 3)] >> (7 - (k & 7))) & 1 for k in range(n)]
    dm = data_mark(cells, WEAK_SECTOR)
    i = dm
    for r in range(RECORDS):
        for b in (0xF6, 0xAF, GCR[r % 64], 0x96 + r // 64, 0x96, 0xF6):
            put(cells, i, b, 8)
            i += 8
        put(cells, i, 0, 16)  # no flux transition
        i += 16
    while i + 10 <= dm + FIELD_CELLS:
        put(cells, i, 0x3FC, 10)  # $FF and two zero cells
        i += 10
    assert i == dm + FIELD_CELLS
    # The index inside the area.
    turn = dm + INDEX_AT
    cells = cells[turn:] + cells[:turn]
    for k in range(n):
        mask = 0x80 >> (k & 7)
        f[at + (k >> 3)] = f[at + (k >> 3)] | mask if cells[k] else f[at + (k >> 3)] & ~mask
    f[20 + 2] = 1  # INFO: write-protected
    struct.pack_into('<I', f, 8, zlib.crc32(bytes(f[12:])) & 0xFFFFFFFF)
    open(sys.argv[2], 'wb').write(f)


if __name__ == '__main__':
    main()
