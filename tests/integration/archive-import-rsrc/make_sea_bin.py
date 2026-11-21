#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) pappadf
# A MacBinary II file whose data fork is empty and whose resource fork is a
# StuffIt 5 archive holding one stored file -- the shape of a .sea.bin.
# The archive layout is archive-confined/make_archive.py's.
import os, struct, sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "archive-confined"))
from make_archive import sit5  # noqa: E402

def crc16_xmodem(data):
    crc = 0
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc

def macbinary(name, data, rsrc):
    hdr = bytearray(128)
    n = name.encode("latin-1")
    hdr[1] = len(n)
    hdr[2:2 + len(n)] = n
    hdr[65:73] = b"APPLaust"
    struct.pack_into(">II", hdr, 83, len(data), len(rsrc))
    hdr[122] = hdr[123] = 129  # MacBinary II
    struct.pack_into(">H", hdr, 124, crc16_xmodem(bytes(hdr[:124])))
    pad = lambda b: b + bytes((128 - len(b) % 128) % 128)
    return bytes(hdr) + pad(data) + pad(rsrc)

member = sys.argv[2]
payload = bytes((i * 7 + 3) & 0xFF for i in range(4096))
open(sys.argv[1], "wb").write(macbinary("Installer.sea", b"", sit5(member, payload)))
