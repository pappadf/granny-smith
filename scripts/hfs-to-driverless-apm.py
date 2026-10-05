#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) pappadf
"""Put a bare HFS volume behind an Apple Partition Map with no driver: a
Driver Descriptor Map naming no drivers, a three-entry map
(Apple_partition_map at 1-63, Apple_HFS at 64, a 16-block Apple_Free tail).

Usage: hfs-to-driverless-apm.py <bare.img> <out.img>

This is the shape Disk Copy and SheepShaver write, and the second shape the
SCSI volume wrapper handles (docs/core/storage/bare-volume-wrapper.md).  Used
by tests/integration/scsi-bare-volume to make one from a bare volume already
in gs-test-data."""

import os
import struct
import sys

BLK = 512
MAP_BLOCKS = 63
HFS_START = 64
FREE_BLOCKS = 16


def entry(count: int, start: int, blocks: int, name: bytes, ptype: bytes, status: int) -> bytes:
    e = bytearray(BLK)
    struct.pack_into(">2sHIII", e, 0, b"PM", 0, count, start, blocks)
    e[16 : 16 + len(name)] = name
    e[48 : 48 + len(ptype)] = ptype
    struct.pack_into(">III", e, 80, 0, blocks, status)
    return bytes(e)


def main() -> int:
    src, dst = sys.argv[1], sys.argv[2]
    vol_blocks = os.path.getsize(src) // BLK
    total = HFS_START + vol_blocks + FREE_BLOCKS
    ddm = bytearray(BLK)
    struct.pack_into(">2sHI", ddm, 0, b"ER", BLK, total)  # sbDrvrCount stays 0
    head = bytes(ddm)
    head += entry(3, 1, MAP_BLOCKS, b"Apple", b"Apple_partition_map", 0x3)
    head += entry(3, HFS_START, vol_blocks, b"disk image", b"Apple_HFS", 0x40000033)
    head += entry(3, HFS_START + vol_blocks, FREE_BLOCKS, b"", b"Apple_Free", 0)
    head += bytes(HFS_START * BLK - len(head))
    with open(src, "rb") as f, open(dst, "wb") as out:
        out.write(head)
        while chunk := f.read(1 << 20):
            out.write(chunk)
        out.write(bytes(FREE_BLOCKS * BLK))
    return 0


if __name__ == "__main__":
    sys.exit(main())
