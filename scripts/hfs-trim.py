#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) pappadf
"""Cut a bare HFS volume short of its own size: drop the free allocation
blocks after the last one in use, the alternate MDB with them.  The volume
itself is untouched, so its MDB still claims the full length.

Usage: hfs-trim.py <bare.img> <out.img>

This is the shape of archive.org disk images someone trimmed of their free
tail, which the image layer opens at the volume's claimed length
(docs/internals/core/storage/bare-volume-wrapper.md, "Trimmed volumes").
Used by tests/integration/scsi-bare-volume to make one from a bare volume
already in gs-test-data."""

import struct
import sys

BLK = 512


def main() -> int:
    src, dst = sys.argv[1], sys.argv[2]
    with open(src, "rb") as f:
        data = f.read()
    mdb = data[2 * BLK : 3 * BLK]
    if mdb[:2] != b"BD":
        print(f"{src}: no HFS MDB at 1024", file=sys.stderr)
        return 1
    (vbm,) = struct.unpack_from(">H", mdb, 14)
    (count,) = struct.unpack_from(">H", mdb, 18)
    (size,) = struct.unpack_from(">I", mdb, 20)
    (first,) = struct.unpack_from(">H", mdb, 28)
    bitmap = data[vbm * BLK : vbm * BLK + (count + 7) // 8]
    used = [b for b in range(count) if bitmap[b // 8] >> (7 - b % 8) & 1]
    end = first * BLK + ((used[-1] + 1) if used else 0) * size
    with open(dst, "wb") as f:
        f.write(data[:end])
    return 0


if __name__ == "__main__":
    sys.exit(main())
