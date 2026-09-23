#!/usr/bin/env python3
# A StuffIt 5 archive holding one stored file, laid out per sit.md §5 --
# the same layout tests/unit/suites/peeler builds in C (build_sit5).
import struct, sys

def crc16_arc(data):
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc

def sit5(name, data):
    name = name.encode("latin-1")
    top = bytearray(100)
    top[0:20] = b"StuffIt (c)1997-2001"
    top[20:78] = b" Aladdin Systems, Inc., http://www.aladdinsys.com/StuffIt/"
    top[78:80] = b"\r\n"
    struct.pack_into(">HI", top, 92, 1, 100)          # one entry, at 100
    h1 = bytearray(48 + len(name))
    struct.pack_into(">IB", h1, 0, 0xA5A5A5A5, 1)     # magic, version 1
    struct.pack_into(">H", h1, 6, len(h1))            # header-1 length
    struct.pack_into(">H", h1, 30, len(name))
    struct.pack_into(">IIH", h1, 34, len(data), len(data), crc16_arc(data))
    h1[48:] = name
    struct.pack_into(">H", h1, 32, crc16_arc(bytes(h1)))  # CRC with its own bytes zero
    h2 = bytearray(36)
    h2[4:12] = b"TEXTttxt"
    return bytes(top + h1 + h2) + data

open(sys.argv[1], "wb").write(sit5(sys.argv[2], sys.argv[3].encode()))
