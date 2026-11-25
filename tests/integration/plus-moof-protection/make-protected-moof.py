#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) pappadf
#
# Turn a writer-made MOOF (a plain 800K disk laid down as a Mac formats it)
# into one carrying the kinds of copy protection that live on the track
# rather than in its sectors:
#
#   - track SIG_TRACK, side 0, sector SIG_SECTOR: the last sync cell in
#     front of the data mark becomes a $DE, so the stream reads
#     ... FF DE D5 AA AD ...; the sector itself still reads;
#   - track MARK_TRACK, side 1: right after sector MARK_SECTOR's data field,
#     in the gap, a field that is no sector:
#       B5 96 FA B5  DE AA  32 x $FF self-sync  D5 AA 96 B3 96 96 96 96
#
# Every sector of the disk still decodes, so it boots.  The header CRC is
# recomputed.  Usage: make-protected-moof.py in.moof out.moof

import struct
import sys
import zlib

SIG_TRACK, SIG_SECTOR = 40, 2
MARK_TRACK, MARK_SECTOR = 52, 1

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


def get(f, at, i, n):
    """`n` cells from cell `i` of the track at byte `at`, as a number."""
    v = 0
    for k in range(i, i + n):
        v = v << 1 | (f[at + (k >> 3)] >> (7 - (k & 7))) & 1
    return v


def put(f, at, i, v, n):
    """Overwrite `n` cells from cell `i` with the low `n` bits of `v`."""
    for k in range(n):
        b = (v >> (n - 1 - k)) & 1
        byte, mask = at + ((i + k) >> 3), 0x80 >> ((i + k) & 7)
        f[byte] = f[byte] | mask if b else f[byte] & ~mask


def data_mark(f, at, nbits, sector):
    """The cell where `sector`'s data mark starts (the writer lays bytes
    on 8-cell boundaries inside fields)."""
    for i in range(nbits - 48):
        if get(f, at, i, 24) != 0xD5AA96:
            continue
        if INV.get(get(f, at, i + 32, 8)) != sector:
            continue
        for j in range(i + 24, nbits - 24):
            if get(f, at, j, 24) == 0xD5AAAD:
                return j
    raise SystemExit(f'sector {sector}: no data mark')


def main():
    f = bytearray(open(sys.argv[1], 'rb').read())
    assert f[:8] == b'MOOF\xff\n\r\n'
    # The signature: $DE and two zero cells in a ten-cell sync slot.
    at, n = track_at(f, SIG_TRACK, 0)
    dm = data_mark(f, at, n, SIG_SECTOR)
    put(f, at, dm - 10, 0b1101111000, 10)
    # The field after MARK_SECTOR's data field: past D5 AA AD, the sector
    # value, 703 values and DE AA FF.
    at, n = track_at(f, MARK_TRACK, 1)
    i = data_mark(f, at, n, MARK_SECTOR) + 24 + 8 + 703 * 8 + 24
    for b in (0xB5, 0x96, 0xFA, 0xB5, 0xDE, 0xAA):
        put(f, at, i, b, 8)
        i += 8
    for _ in range(32):
        put(f, at, i, 0x3FC, 10)  # $FF and two zero cells
        i += 10
    for b in (0xD5, 0xAA, 0x96, 0xB3, 0x96, 0x96, 0x96, 0x96):
        put(f, at, i, b, 8)
        i += 8
    put(f, at, i, 0, 8)  # back onto the gap's ten-cell grid
    struct.pack_into('<I', f, 8, zlib.crc32(bytes(f[12:])) & 0xFFFFFFFF)
    open(sys.argv[2], 'wb').write(f)


if __name__ == '__main__':
    main()
