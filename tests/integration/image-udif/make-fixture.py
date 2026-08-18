#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) pappadf

"""Build a UDIF (.dmg) fixture from a raw disk image.

Written straight from the format description rather than from
src/core/storage/image_udif.c, so the two are independent implementations of
the same spec: if the emulator's reader and this writer agree on a real
round-trip, both are probably right.

The output deliberately mixes chunk types — zlib (UDZO), raw, and zero-fill —
so one decode exercises every branch the reader implements, and carries the
CRC-32s (per block table and over the whole data fork) that the reader
verifies.  It is also split into two 'mish' block tables rather than one, so
the second table's chunk sectors restart at zero: getting that relative-to-
absolute arithmetic wrong is the classic way to misread this format, and a
single-table fixture would not catch it.  Encrypted, multi-segment, bzip2 and
LZFSE images are outside what the reader supports and are not produced here.

Usage: make-fixture.py <source.img> <out.dmg>
"""

import base64
import struct
import sys
import zlib

SECTOR = 512
CHUNK_SECTORS = 128  # 64 KB runs, so even a small source yields many chunks

CHUNK_ZERO = 0x00000000
CHUNK_RAW = 0x00000001
CHUNK_ZLIB = 0x80000005
CHUNK_END = 0xFFFFFFFF

CHECKSUM_CRC32 = 2


def build_chunks(data, fork, force_raw_first):
    """Chunk one table's sector run, appending its payload bytes to `fork`.

    The returned entries carry sectors relative to the start of `data`, which
    is what the format stores; the table's own base sector is what makes them
    absolute again.
    """
    entries = []
    total = len(data) // SECTOR
    sector = 0
    force_raw = force_raw_first
    while sector < total:
        count = min(CHUNK_SECTORS, total - sector)
        run = data[sector * SECTOR:(sector + count) * SECTOR]
        if not any(run):
            # Zero-fill costs no data-fork bytes at all.
            entries.append(dict(type=CHUNK_ZERO, sector=sector, count=count,
                                offset=len(fork), length=0))
        else:
            packed = zlib.compress(run, 9)
            # Store raw when compression does not pay, as hdiutil does — plus
            # one forced raw run (the table's first run with actual content) so
            # the reader's raw path stays covered even on a source where every
            # run happens to compress.
            if len(packed) >= len(run) or force_raw:
                force_raw = False
                entries.append(dict(type=CHUNK_RAW, sector=sector, count=count,
                                    offset=len(fork), length=len(run)))
                fork += run
            else:
                entries.append(dict(type=CHUNK_ZLIB, sector=sector, count=count,
                                    offset=len(fork), length=len(packed)))
                fork += packed
        sector += count
    return entries


def build_mish(entries, base, sectors, checksum):
    """Serialise one 'mish' block table: 0xCC header + 40-byte entries."""
    b = bytearray(0xCC)
    b[0x00:0x04] = b"mish"
    struct.pack_into(">I", b, 0x04, 1)  # version
    struct.pack_into(">Q", b, 0x08, base)  # absolute start sector of this table
    struct.pack_into(">Q", b, 0x10, sectors)
    struct.pack_into(">Q", b, 0x18, 0)  # data offset (always 0 in practice)
    struct.pack_into(">I", b, 0x20, SECTOR + 8)  # buffers needed
    struct.pack_into(">I", b, 0x24, 0)  # blkx resource ID — NOT a count
    struct.pack_into(">I", b, 0x40, CHECKSUM_CRC32)
    struct.pack_into(">I", b, 0x44, 32)  # checksum bits
    struct.pack_into(">I", b, 0x48, checksum)
    struct.pack_into(">I", b, 0xC8, len(entries) + 1)  # entries + terminator
    for e in entries:
        b += struct.pack(">IIQQQQ", e["type"], 0, e["sector"], e["count"],
                         e["offset"], e["length"])
    tail = entries[-1]["offset"] + entries[-1]["length"] if entries else 0
    b += struct.pack(">IIQQQQ", CHUNK_END, 0, sectors, 0, tail, 0)
    return bytes(b)


def build_plist(tables):
    """Wrap the Base64'd block tables in the plist hdiutil writes."""
    entries = []
    for i, (name, mish) in enumerate(tables):
        blob = base64.b64encode(mish).decode()
        wrapped = "\n\t\t\t\t".join(blob[j:j + 64] for j in range(0, len(blob), 64))
        entries.append(
            "\t\t\t<dict>\n"
            "\t\t\t\t<key>Attributes</key>\n\t\t\t\t<string>0x0050</string>\n"
            f"\t\t\t\t<key>CFName</key>\n\t\t\t\t<string>{name}</string>\n"
            f"\t\t\t\t<key>Data</key>\n\t\t\t\t<data>\n\t\t\t\t{wrapped}\n\t\t\t\t</data>\n"
            f"\t\t\t\t<key>ID</key>\n\t\t\t\t<string>{i}</string>\n"
            f"\t\t\t\t<key>Name</key>\n\t\t\t\t<string>{name}</string>\n"
            "\t\t\t</dict>\n")
    return (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" '
        '"http://www.apple.com/DTDs/PropertyList-1.0.dtd">\n'
        '<plist version="1.0">\n<dict>\n'
        "\t<key>resource-fork</key>\n\t<dict>\n\t\t<key>blkx</key>\n\t\t<array>\n"
        + "".join(entries) +
        "\t\t</array>\n\t</dict>\n</dict>\n</plist>\n"
    ).encode()


def build_trailer(fork_len, fork_crc, xml_off, xml_len, sectors):
    """Serialise the fixed 512-byte 'koly' trailer that ends every UDIF."""
    t = bytearray(512)
    t[0x00:0x04] = b"koly"
    struct.pack_into(">I", t, 0x04, 4)  # version
    struct.pack_into(">I", t, 0x08, 512)  # header size
    struct.pack_into(">I", t, 0x0C, 1)  # flags
    struct.pack_into(">Q", t, 0x18, 0)  # data fork offset
    struct.pack_into(">Q", t, 0x20, fork_len)
    struct.pack_into(">I", t, 0x38, 1)  # segment number
    struct.pack_into(">I", t, 0x3C, 1)  # segment count
    struct.pack_into(">I", t, 0x50, CHECKSUM_CRC32)  # data-fork checksum
    struct.pack_into(">I", t, 0x54, 32)
    struct.pack_into(">I", t, 0x58, fork_crc)
    struct.pack_into(">Q", t, 0xD8, xml_off)
    struct.pack_into(">Q", t, 0xE0, xml_len)
    struct.pack_into(">I", t, 0x1E8, 1)  # image variant: device image
    struct.pack_into(">Q", t, 0x1EC, sectors)  # NB: 0x1EC, inside the 512 bytes
    return bytes(t)


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: make-fixture.py <source.img> <out.dmg>")
    src, dst = sys.argv[1], sys.argv[2]
    with open(src, "rb") as f:
        data = f.read()
    if len(data) % SECTOR:
        sys.exit(f"{src}: {len(data)} bytes is not a whole number of sectors")

    sectors = len(data) // SECTOR
    # Split on a chunk boundary so neither table straddles a run.
    split = (sectors // 2 // CHUNK_SECTORS) * CHUNK_SECTORS
    if split == 0 or split == sectors:
        sys.exit(f"{src}: too small to split into two block tables")

    fork = bytearray()
    tables = []
    for i, (base, end) in enumerate(((0, split), (split, sectors))):
        part = data[base * SECTOR:end * SECTOR]
        entries = build_chunks(part, fork, force_raw_first=(i == 1))
        # A table's CRC-32 covers the decoded bytes of every chunk except the
        # ignored/unallocated ones (of which this writer emits none).
        crc = zlib.crc32(part) & 0xFFFFFFFF
        tables.append((f"part {i} (Apple_HFS : {i})",
                       build_mish(entries, base, end - base, crc)))

    xml = build_plist(tables)
    with open(dst, "wb") as f:
        f.write(fork)
        f.write(xml)
        f.write(build_trailer(len(fork), zlib.crc32(bytes(fork)) & 0xFFFFFFFF,
                              len(fork), len(xml), sectors))
    print(f"wrote {dst}: {sectors} sectors, {len(tables)} block tables, "
          f"{len(fork)} data-fork bytes")


if __name__ == "__main__":
    main()
