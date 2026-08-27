#!/usr/bin/env python3
# Build the two negative PROM fixtures this row needs.
#
# Generated rather than committed: each is a handful of header bytes, and
# what makes them useful is exactly what the header says, which is clearer
# as code than as an opaque blob.
import struct
import sys

out_dir = sys.argv[1]


def option_rom(code_type, with_fcode):
    """A minimal, structurally valid PCI expansion ROM."""
    img = bytearray(2048)
    img[0:2] = b"\x55\xAA"
    img[2:4] = struct.pack("<H", 0x0040)  # FCode offset
    img[0x18:0x1A] = struct.pack("<H", 0x20)  # -> the PCI Data Structure
    img[0x20:0x24] = b"PCIR"
    img[0x24:0x28] = struct.pack("<HH", 0x1002, 0x4758)  # ATI, mach64 GX
    img[0x2C:0x2F] = bytes((0x00, 0x00, 0x03))  # class 030000
    img[0x30:0x32] = struct.pack("<H", 4)  # 4 * 512 = 2 KB
    img[0x34] = code_type
    img[0x35] = 0x80  # last image
    if with_fcode:
        img[0x40] = 0xF1  # start1
        img[0x41] = 0x08
    return bytes(img)


# The predictable user error: the ROM off a PC Mach64.  Right vendor, right
# device, real PCIR — and code type 0, so it is an x86 option ROM that
# cannot drive a Macintosh card.
with open(f"{out_dir}/fake-x86-vga.prom", "wb") as f:
    f.write(option_rom(0x00, with_fcode=False))

# A structurally perfect Open Firmware expansion ROM that simply is not in
# our catalog — the "some other card" case.
with open(f"{out_dir}/uncatalogued.prom", "wb") as f:
    f.write(option_rom(0x01, with_fcode=True))

print("tnt-pci-mach64: negative PROM fixtures written")
