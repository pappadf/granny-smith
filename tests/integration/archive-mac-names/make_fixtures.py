#!/usr/bin/env python3
# Build the archives the archive-mac-names row reads:
#   names.sit  -- StuffIt 5: folder b"Dossier \x8e" holding a stored text file
#                 named b"Caf\x8e" (MacRoman 0x8E is "é"), and the floppy
#                 image named b"Syst\x8fme.dsk" (0x8F is "è")
#   utf8.zip   -- a zip whose one member is named "Café" in UTF-8 (Python
#                 sets the UTF-8 flag)
# The StuffIt layout is sit.md §5, as archive-confined/make_archive.py and
# tests/unit/suites/peeler (build_sit5) lay it out.
# Usage: make_fixtures.py <floppy.dsk> <out-dir>

import struct
import sys
import zipfile


def crc16_arc(data):
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


# One entry: header 1, header 2, the data.  `name` is raw bytes; a folder
# (`children` > 0) has no data, its children follow it and name it by its
# offset (`parent`).
def sit5_entry(name, data, ftype, creator, parent=0, children=0):
    h1 = bytearray(48 + len(name))
    struct.pack_into(">IB", h1, 0, 0xA5A5A5A5, 1)  # magic, version 1
    struct.pack_into(">H", h1, 6, len(h1))  # header-1 length
    h1[9] = 0x40 if children else 0  # folder
    struct.pack_into(">I", h1, 26, parent)
    struct.pack_into(">H", h1, 46, children)
    struct.pack_into(">H", h1, 30, len(name))
    struct.pack_into(">IIH", h1, 34, len(data), len(data), crc16_arc(data))
    h1[48:] = name
    struct.pack_into(">H", h1, 32, crc16_arc(bytes(h1)))  # CRC with its own bytes zero
    h2 = bytearray(36)
    h2[4:12] = ftype + creator
    return bytes(h1 + h2) + data


def sit5(top_level, entries):
    top = bytearray(100)
    top[0:20] = b"StuffIt (c)1997-2001"
    top[20:78] = b" Aladdin Systems, Inc., http://www.aladdinsys.com/StuffIt/"
    top[78:80] = b"\r\n"
    struct.pack_into(">HI", top, 92, top_level, 100)  # top-level entries, the first at 100
    body = b"".join(sit5_entry(*e) for e in entries)
    struct.pack_into(">I", top, 84, 100 + len(body))  # total length
    return bytes(top) + body


disk_path, out = sys.argv[1], sys.argv[2]
with open(disk_path, "rb") as f:
    disk = f.read()

with open(f"{out}/names.sit", "wb") as f:
    folder = (b"Dossier \x8e", b"", b"\0" * 4, b"\0" * 4, 0, 1)
    f.write(
        sit5(
            2,
            [
                folder,  # at 100
                (b"Caf\x8e", b"payload", b"TEXT", b"ttxt", 100),
                (b"Syst\x8fme.dsk", disk, b"dImg", b"dCpy"),
            ],
        )
    )

with zipfile.ZipFile(f"{out}/utf8.zip", "w", zipfile.ZIP_STORED) as z:
    z.writestr("Café", b"zipped")
