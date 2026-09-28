#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) pappadf
"""Write the first Apple_HFS partition of an Apple-partitioned disk image
out as a bare volume (no DDM, no map, no driver).

Usage: apm-extract-hfs.py <disk.img> <out.img>

Used by tests/integration/scsi-bare-volume to make a bare volume of a
System the AV machines boot, from an image already in gs-test-data."""

import struct
import sys


def main() -> int:
    src, dst = sys.argv[1], sys.argv[2]
    with open(src, "rb") as f:
        f.seek(512)
        first = f.read(512)
        if first[:2] != b"PM":
            print(f"{src}: no Apple partition map", file=sys.stderr)
            return 1
        count = struct.unpack(">I", first[4:8])[0]
        for i in range(1, count + 1):
            f.seek(512 * i)
            e = f.read(512)
            if e[48:80].split(b"\0")[0] != b"Apple_HFS":
                continue
            start, blocks = struct.unpack(">II", e[8:16])
            f.seek(512 * start)
            with open(dst, "wb") as out:
                left = blocks * 512
                while left:
                    chunk = f.read(min(left, 1 << 20))
                    if not chunk:
                        break
                    out.write(chunk)
                    left -= len(chunk)
            return 0
    print(f"{src}: no Apple_HFS partition", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
