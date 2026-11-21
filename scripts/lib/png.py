#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) pappadf
"""png.py -- the stdlib PNG reading the golden scripts share.

scripts/check-goldens.py (which must REJECT any filter but 0, since the
emulator's reader ignores filters) and scripts/golden-triage.py (which
reads any 8-bit-or-less PNG and unfilters it) walk the same chunks and
inflate the same IDAT; the row handling stays with each caller's intent:
rows(..., unfilter=False) hands back the raw filter byte per row.
"""

import struct
import zlib

SIGNATURE = b"\x89PNG\r\n\x1a\n"

# Colour type -> channels per pixel.
CHANNELS = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}


def chunks(data):
    """[(type, body)] of a PNG's chunks, up to IEND; ValueError if no PNG."""
    if not data.startswith(SIGNATURE):
        raise ValueError("not a PNG")
    out = []
    pos = 8
    while pos + 8 <= len(data):
        length, ctype = struct.unpack(">I4s", data[pos : pos + 8])
        out.append((ctype, data[pos + 8 : pos + 8 + length]))
        pos += 12 + length  # length, type, body, CRC
        if ctype == b"IEND":
            break
    return out


def ihdr(chs):
    """(width, height, bit depth, colour type, compression, filter, interlace), or None."""
    for ctype, body in chs:
        if ctype == b"IHDR":
            return struct.unpack(">IIBBBBB", body[:13])
    return None


def idat(chs):
    """The concatenated (still deflated) IDAT payload."""
    return b"".join(body for ctype, body in chs if ctype == b"IDAT")


def rows(raw, width, height, channels, depth, unfilter=True):
    """The scanlines of an inflated IDAT: [(filter type, bytes)].

    With `unfilter` each row is reconstructed (filters 1-4, the PNG spec's
    predictors) and its filter reported as 0; without, the bytes are as
    stored.  ValueError when `raw` is the wrong size."""
    bpp = max(1, channels * depth // 8)
    stride = (width * channels * depth + 7) // 8
    if len(raw) != (stride + 1) * height:
        raise ValueError(f"IDAT holds {len(raw)} bytes, {(stride + 1) * height} expected")
    out, prev, p = [], bytearray(stride), 0
    for _ in range(height):
        filt = raw[p]
        line = bytearray(raw[p + 1 : p + 1 + stride])
        p += 1 + stride
        if unfilter:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                b = prev[i]
                c = prev[i - bpp] if i >= bpp else 0
                if filt == 1:
                    line[i] = (line[i] + a) & 0xFF
                elif filt == 2:
                    line[i] = (line[i] + b) & 0xFF
                elif filt == 3:
                    line[i] = (line[i] + (a + b) // 2) & 0xFF
                elif filt == 4:
                    pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                    pred = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                    line[i] = (line[i] + pred) & 0xFF
            out.append((0, bytes(line)))
            prev = line
        else:
            out.append((filt, bytes(line)))
    return out


def inflate(chs):
    """The inflated IDAT; zlib.error when it does not inflate."""
    return zlib.decompress(idat(chs))
