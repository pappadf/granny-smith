#!/usr/bin/env python3
# Build the fixtures this row needs: two negative PROMs, and the two
# reference PNGs for the 5-6-5 display-format rows.
#
# Generated rather than committed: each is a handful of header bytes (or a
# raster derived from a stated formula), and what makes them useful is
# exactly what the code says, which is clearer as code than as an opaque
# blob.
import struct
import sys
import zlib

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


# --- Reference PNGs for the 5-6-5 rows --------------------------------------
#
# The test script pokes the raster below into VRAM and the emulator's PNG
# encoder expands it for screen.match; these fixtures are the INDEPENDENT
# expansion of the same 16-bit words, computed here from the documented
# rules, so a match proves the encoder — not that the encoder agrees with
# itself.  Two interpretations of the identical bytes, because the
# predictable bug is a 5-6-5 expander that silently falls through to the
# neighbouring 5-5-5 case and looks exactly like success.

W, H = 64, 32


def pixel(x, y):
    """The synthetic raster: every channel a different walk, all 16 bits used."""
    return ((x & 31) << 11) | (((x + y) & 63) << 5) | (y & 31)


def expand_565(v):
    r5, g6, b5 = (v >> 11) & 31, (v >> 5) & 63, v & 31
    return ((r5 << 3) | (r5 >> 2), (g6 << 2) | (g6 >> 4), (b5 << 3) | (b5 >> 2))


def expand_555(v):
    r5, g5, b5 = (v >> 10) & 31, (v >> 5) & 31, v & 31
    return ((r5 << 3) | (r5 >> 2), (g5 << 3) | (g5 >> 2), (b5 << 3) | (b5 >> 2))


def write_png(path, expand):
    """Minimal RGBA PNG, matching what save_framebuffer_as_png emits."""
    raw = bytearray()
    for y in range(H):
        raw.append(0)  # filter: none
        for x in range(W):
            r, g, b = expand(pixel(x, y))
            raw += bytes((r, g, b, 255))

    def chunk(tag, data):
        body = tag + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body))

    ihdr = struct.pack(">IIBBBBB", W, H, 8, 6, 0, 0, 0)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", ihdr))
        f.write(chunk(b"IDAT", zlib.compress(bytes(raw))))
        f.write(chunk(b"IEND", b""))


write_png(f"{out_dir}/raster-565.png", expand_565)
write_png(f"{out_dir}/raster-555.png", expand_555)

print("tnt-pci-mach64: negative PROM and 5-6-5 raster fixtures written")
