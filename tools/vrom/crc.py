#!/usr/bin/env python3
"""Format-Block finaliser for GS generic declaration ROM images.

vasm emits the ROM as a flat binary whose last 20 bytes are the Format
Block with the Length/CRC/ByteLanes fields left as placeholders.  This
script stamps:

  - Length          (covered region = whole image by default)
  - CRC             (rotate-left-1-and-add over Length bytes, CRC field
                     itself read as zero — docs/core/peripherals/
                     nubus_vrom.md sec. 2.6)
  - ByteLanes       (dense 4-lane $0F for all GS images, sec. 5.3 of the
                     generic-nubus-vrom proposal)

and verifies the TestPattern is present where the Format Block claims.

Usage: crc.py <in.bin> <out.bin> [--byte-lanes 0x0F] [--max-size N]

The image is left at its assembled size (the loader tail-places it at
the top of the slot window; Length/CRC cover exactly the content, like
the real 8•24 ROM whose Length is $4000 inside a 32 KB chip).
--max-size guards against the image outgrowing its slot window budget.
"""

import argparse
import struct
import sys

TEST_PATTERN = 0x5A932BC7

# Format Block layout, relative to end of image (20 bytes total, low to
# high address): DirectoryOffset, Length, CRC, RevisionLevel, Format,
# TestPattern, Reserved, ByteLanes.
FB_SIZE = 20
OFF_DIR = -20  # long: self-relative directory offset
OFF_LENGTH = -16  # long: covered byte count
OFF_CRC = -12  # long: checksum
OFF_REV = -8  # byte: revision level (1..9)
OFF_FORMAT = -7  # byte: $01 Apple format
OFF_TESTPAT = -6  # long: $5A932BC7
OFF_RESERVED = -2  # byte: $00
OFF_BYTELANES = -1  # byte: lane mask + complement


def crc_of(image: bytes, length: int) -> int:
    """Rotate-left-1-and-add checksum over the last `length` bytes, with
    the 4 CRC bytes read as zero."""
    total = len(image)
    crc_lo = total + OFF_CRC  # first byte of the CRC field
    acc = 0
    for i in range(total - length, total):
        acc = ((acc << 1) | (acc >> 31)) & 0xFFFFFFFF  # rol.l #1
        byte = 0 if crc_lo <= i < crc_lo + 4 else image[i]
        acc = (acc + byte) & 0xFFFFFFFF
    return acc


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("infile")
    ap.add_argument("outfile")
    ap.add_argument("--byte-lanes", type=lambda s: int(s, 0), default=0x0F)
    ap.add_argument("--max-size", type=lambda s: int(s, 0), default=0)
    args = ap.parse_args()

    with open(args.infile, "rb") as f:
        image = bytearray(f.read())

    if len(image) < FB_SIZE:
        print(f"crc.py: {args.infile} is smaller than a Format Block", file=sys.stderr)
        return 1
    if args.max_size and len(image) > args.max_size:
        print(f"crc.py: image ({len(image)} bytes) exceeds --max-size {args.max_size}", file=sys.stderr)
        return 1

    # Sanity: the assembler must have emitted the TestPattern in place.
    (testpat,) = struct.unpack_from(">L", image, len(image) + OFF_TESTPAT)
    if testpat != TEST_PATTERN:
        print(f"crc.py: TestPattern is ${testpat:08X}, expected ${TEST_PATTERN:08X} — "
              "Format Block not at end of image?", file=sys.stderr)
        return 1

    # Stamp ByteLanes (with complement nibble validity check) and Length.
    lanes = args.byte_lanes & 0xFF
    if ((~lanes) & 0x0F) != (lanes >> 4):
        print(f"crc.py: byte-lanes ${lanes:02X} fails the complement rule", file=sys.stderr)
        return 1
    image[len(image) + OFF_BYTELANES] = lanes
    length = len(image)  # cover the whole chip
    struct.pack_into(">L", image, len(image) + OFF_LENGTH, length)

    # Stamp the CRC last.
    struct.pack_into(">L", image, len(image) + OFF_CRC, crc_of(bytes(image), length))

    with open(args.outfile, "wb") as f:
        f.write(image)
    print(f"crc.py: {args.outfile}: {len(image)} bytes, lanes ${lanes:02X}, "
          f"crc ${crc_of(bytes(image), length):08X}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
