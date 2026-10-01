#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) pappadf
#
# make_zip_gz_fixtures.py -- build the zip and gzip test cases under
# testfiles/ with independent writers (Info-ZIP `zip`, GNU `gzip`, Python's
# zipfile), and their md5sums.txt from the source content, never from
# peeler's own output.  The committed fixtures are what this produced; it is
# kept so they can be rebuilt and so their provenance is plain.
#
#   ./make_zip_gz_fixtures.py [testfiles-dir]

import gzip
import hashlib
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile
import zlib

OUT = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "testfiles"))
EPOCH = "2001-02-03 04:05:06"  # every source file's mtime, for repeatable archives


# Deterministic pseudo-random bytes (incompressible content without os.urandom).
def noise(n, seed):
    out = bytearray()
    x = seed
    while len(out) < n:
        x = (x * 6364136223846793005 + 1442695040888963407) & (2**64 - 1)
        out += struct.pack("<Q", x)
    return bytes(out[:n])


# The source tree every zip fixture is made from.
SOURCES = {
    "readme.txt": b"A text file, small enough to be stored.\n",
    "notes/long.txt": "".join(f"line {i}: the quick brown fox jumps over the lazy dog\n" for i in range(1500)).encode(),
    "notes/empty.txt": b"",
    "data/noise.bin": noise(70000, 1),
    "data/zeros.bin": bytes(20000),
}


def md5(b):
    return hashlib.md5(b).hexdigest()


def write_tree(root, files):
    for path, data in files.items():
        p = os.path.join(root, path)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "wb") as f:
            f.write(data)
    subprocess.run(["find", root, "-exec", "touch", "-h", "-d", EPOCH, "{}", "+"], check=True)


# A test case directory: the input and the md5 of every file peeler should
# write for it.  Empty files are not written by the CLI (as for every format).
def case(name, data, expected):
    d = os.path.join(OUT, name)
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    with open(os.path.join(d, "testfile." + name), "wb") as f:
        f.write(data)
    with open(os.path.join(d, "md5sums.txt"), "w") as f:
        for path in sorted(expected):
            f.write(f"{md5(expected[path])}  {path}\n")


def nonempty(files):
    return {p: b for p, b in files.items() if b}


# AppleDouble (RFC 1740 / Apple's AppleSingle/AppleDouble spec), version 2:
# 26-byte header, 12-byte descriptors, payloads in descriptor order.
def appledouble(entries):
    hdr = struct.pack(">II16sH", 0x00051607, 0x00020000, b"\0" * 16, len(entries))
    off = len(hdr) + 12 * len(entries)
    desc, body = b"", b""
    for eid, data in entries:
        desc += struct.pack(">III", eid, off + len(body), len(data))
        body += data
    return hdr + desc + body


def finder_info(ftype, creator, flags):
    return struct.pack(">4s4sH", ftype, creator, flags) + bytes(22)


def zip_cli(srcdir, args, members):
    out = os.path.join(srcdir, "..", "out.zip")
    if os.path.exists(out):
        os.remove(out)
    subprocess.run(["zip", "-q", "-X"] + args + [out] + members, cwd=srcdir, check=True)
    with open(out, "rb") as f:
        return f.read()


def main():
    tmp = tempfile.mkdtemp()
    try:
        src = os.path.join(tmp, "src")
        write_tree(src, SOURCES)
        members = sorted(SOURCES)

        # 1. Info-ZIP, the usual mix: tiny and incompressible files stored by
        #    zip's own choice, text deflated, a folder tree.
        case("zip_infozip.zip", zip_cli(src, ["-r"], members), nonempty(SOURCES))

        # 2. Every member stored (method 0): each is a view of the archive.
        case("zip_stored.zip", zip_cli(src, ["-0"], members), nonempty(SOURCES))

        # 3. Zip64: Info-ZIP's -fz writes Zip64 extra fields and the Zip64
        #    end-of-central-directory record and locator.
        case("zip_zip64.zip", zip_cli(src, ["-fz"], members), nonempty(SOURCES))

        # 4. Zip64 from a second writer: Python's per-entry force_zip64.
        buf = os.path.join(tmp, "py64.zip")
        with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as z:
            for p in members:
                info = zipfile.ZipInfo(p, date_time=(2001, 2, 3, 4, 5, 6))
                info.compress_type = zipfile.ZIP_DEFLATED
                with z.open(info, "w", force_zip64=True) as w:
                    w.write(SOURCES[p])
        with open(buf, "rb") as f:
            case("zip_py64.zip", f.read(), nonempty(SOURCES))

        # 5. Self-extracting: an executable stub in front.  zip -A adjusts the
        #    offsets to the whole file; the plain concatenation leaves them
        #    relative to the zip, and a reader must find the delta itself.
        plain = zip_cli(src, [], members)
        stub = b"MZ" + noise(8190, 2)
        case("zip_sfx.zip", stub + plain, nonempty(SOURCES))
        sfx = os.path.join(tmp, "sfx.zip")
        with open(sfx, "wb") as f:
            f.write(stub + plain)
        subprocess.run(["zip", "-q", "-A", sfx], check=True)
        with open(sfx, "rb") as f:
            case("zip_sfx_adjusted.zip", f.read(), nonempty(SOURCES))

        # 6. A Mac file zipped by the Finder: its resource fork and Finder
        #    info ride in a "__MACOSX/._<name>" AppleDouble member, which a
        #    reader folds back into the file (here written as the CLI writes
        #    any two-fork file: the data, and a "._<name>" sidecar).
        rsrc = noise(3000, 3)
        fi = finder_info(b"APPL", b"ttxt", 0x0100)
        ad = appledouble([(9, fi), (2, rsrc)])
        mac = dict(SOURCES)
        mac["__MACOSX/._readme.txt"] = ad
        mac["__MACOSX/notes/._long.txt"] = appledouble([(9, finder_info(b"TEXT", b"ttxt", 0))])
        msrc = os.path.join(tmp, "mac")
        write_tree(msrc, mac)
        expected = nonempty(SOURCES)
        expected["._readme.txt"] = appledouble([(9, fi), (2, rsrc)])
        expected["notes/._long.txt"] = appledouble([(9, finder_info(b"TEXT", b"ttxt", 0))])
        case("zip_macosx.zip", zip_cli(msrc, ["-r"], sorted(mac)), expected)

        # 7. gzip, one member, the original name stored (gzip -N).
        long_txt = SOURCES["notes/long.txt"]
        p = os.path.join(tmp, "long.txt")
        with open(p, "wb") as f:
            f.write(long_txt)
        g1 = subprocess.run(["gzip", "-N", "-9", "-c", p], check=True, capture_output=True).stdout
        case("gz_single.gz", g1, {"long.txt": long_txt})

        # 8. gzip, three members concatenated (RFC 1952 § 2.2): the payload is
        #    all three, and the tail ISIZE names only the last.
        parts = [long_txt, SOURCES["data/noise.bin"], b"tail member\n"]
        multi = b""
        for i, part in enumerate(parts):
            q = os.path.join(tmp, f"part{i}")
            with open(q, "wb") as f:
                f.write(part)
            multi += subprocess.run(["gzip", "-n", "-c", q], check=True, capture_output=True).stdout
        # gzip -n stores no name: the CLI names it after the input, less .gz.
        case("gz_multi.gz", multi, {"testfile.gz_multi": b"".join(parts)})

        # 9. BGZF (the SAM/BAM spec's blocked gzip): members of at most
        #    64 KiB, each with a "BC" extra subfield giving the block size, and
        #    the standard empty EOF block.  Readable at random by block.
        payload = SOURCES["data/noise.bin"] + long_txt + SOURCES["data/zeros.bin"]
        case("gz_bgzf.gz", bgzf(payload), {"testfile.gz_bgzf": payload})
    finally:
        shutil.rmtree(tmp)


# One BGZF block holding `data` (at most 64 KiB).
def bgzf_block(data):
    co = zlib.compressobj(6, zlib.DEFLATED, -15)
    cdata = co.compress(data) + co.flush()
    bsize = 12 + 6 + len(cdata) + 8  # header + extra + data + trailer
    hdr = struct.pack("<BBBBIBBH", 0x1F, 0x8B, 8, 4, 0, 0, 0xFF, 6)
    extra = struct.pack("<BBHH", ord("B"), ord("C"), 2, bsize - 1)
    trailer = struct.pack("<II", zlib.crc32(data) & 0xFFFFFFFF, len(data))
    return hdr + extra + cdata + trailer


def bgzf(payload):
    out = b""
    for i in range(0, len(payload), 0xFF00):  # htslib's block payload size
        out += bgzf_block(payload[i:i + 0xFF00])
    out += bgzf_block(b"")  # the EOF marker block
    # Every block is a valid gzip member: the whole is a valid gzip file.
    assert gzip.decompress(out) == payload
    return out


if __name__ == "__main__":
    main()
