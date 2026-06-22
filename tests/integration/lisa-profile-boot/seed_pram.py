#!/usr/bin/env python3
"""Synthesize the Lisa parameter memory (PRAM) that makes the OS boot from the
installed ProFile.

This writes the exact 64-byte PRAM image the Office System installer leaves at a
clean shutdown (the device-configuration table with the ProFile as a configured
device), plus BootVol=2 so the boot ROM auto-boots the ProFile rather than the
(absent) floppy.  Format is documented in docs/machines/lisa/pram_format.md.

Usage:  seed_pram.py <output-path>
"""
import sys


def rotl16(v):
    v &= 0xFFFF
    return ((v << 1) | (v >> 15)) & 0xFFFF


def checksum(words31):
    # ROM WRTSUM: two's-complement negate of the add-then-ROL#1 sum of words 0..30,
    # so the full 32-word rotate-sum comes out 0 (= CHKPM "valid"). docs §5.
    acc = 0
    for w in words31:
        acc = rotl16((acc + (w & 0xFFFF)) & 0xFFFF)
    return (-acc) & 0xFFFF


def pack_dev(slot, chan, dev, driver_id, ext=()):
    # One DevConfig entry, docs §3 (pack_pm). slot holds the internal cd_* code.
    idsize = 1 if driver_id > 0x1FF else 0
    id_hi = (driver_id >> (16 if idsize else 8)) & 1
    b = bytes([(slot << 4) | (chan << 1) | idsize,
               (dev << 3) | (len(ext) << 1) | id_hi])
    b += (bytes([(driver_id >> 8) & 0xFF, driver_id & 0xFF]) if idsize
          else bytes([driver_id & 0xFF]))
    for w in ext:
        b += bytes([(w >> 8) & 0xFF, w & 0xFF])
    return b


pm = bytearray(64)
pm[0:2] = (4).to_bytes(2, "big")        # Version = cd_pm_version
pm[2:4] = (0x9F80).to_bytes(2, "big")   # TimeStamp (any value; pm_good carries the boot)
pm[4] = (2 << 4) | 5                    # BootVol=2 (cd_paraport / ProFile), NormCont=5
pm[5] = (15 << 4) | 1                   # DimCont=15, BeepVol=1
pm[6] = 0xC3                            # MouseOn|ExtendMem, DoubleClick=3
pm[7] = 0x34                            # FadeDelay=3, BeginRepeat=4
pm[8] = 0x10                            # SubRepeat=1

# Device-configuration table — matches what the LOS 3.1 install writes; the
# driver ids come from that system's SYSTEM.CDD.  The last entry (cd_paraport=10)
# is the ProFile; FIND_PM_IDS needs it or the boot falls back to Twiggy (err 10738).
devs = [
    pack_dev(9, 1, 31, 32),              # SCC channel
    pack_dev(1, 7, 31, 34),             # slot device
    pack_dev(1, 0, 31, 35),             # slot device
    pack_dev(9, 0, 31, 32, (0xC020,)),  # SCC channel + 1 extension word
    pack_dev(10, 7, 31, 35),            # cd_paraport == the ProFile (boot device)
]
cfg = b"".join(devs)
pm[9] = len(devs)                       # CDcount
pm[10:10 + len(cfg)] = cfg
for i in range(10 + len(cfg), 60):      # 0xFF filler doubles as the end-of-list sentinel
    pm[i] = 0xFF
pm[60:62] = (0x004C).to_bytes(2, "big")  # MemLoss

words = [(pm[i] << 8) | pm[i + 1] for i in range(0, 62, 2)]  # words 0..30
pm[62:64] = checksum(words).to_bytes(2, "big")

with open(sys.argv[1], "wb") as f:
    f.write(pm)
