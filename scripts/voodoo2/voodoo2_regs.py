#!/usr/bin/env python3
"""Voodoo2 (CVG) memory-mapped register table, built as data.

Transcribed from 3dfx's own *Voodoo2 Graphics Specification*, rev 1.16
(December 1999): the standard map from pp.22-26 and the alternate triangle
mapping (fbiInit3[0]=1 plus register-address bit 21) from pp.27-29.
Printed page == PDF page, so every row below is checkable directly.

Used by trace tooling and logpoint templates so a register access is
logged by NAME rather than by offset, and importable from other scripts:

    from voodoo2_regs import reg_by_offset, reg_name
    reg_name(0x210)            -> "fbiInit0"
    reg_name(0x020, alt=True)  -> "startR"   (the remapped location)

THE POSITIVE CONTROL (run `voodoo2_regs.py --check`).  A hand-built
register table fails by transposition, and a transposed entry produces
confident, wrong trace output (Phase 2's proposal §1.7 records what that
cost: a "CONFIG_CNTL never appears" raised as blocking before it was
found to be an instrument error).  The spec itself provides the
cross-check: pp.27-29 list the same triangle-parameter registers at
different offsets, and that layout is DERIVABLE by rule — each
parameter's start/dX/dY made adjacent, parameters in the order
R,G,B,Z,A,S,T,W (integer block, then the float block again).  --check
regenerates the alternate map from the standard table by that rule and
requires the regeneration to equal the transcription, entry for entry.
A transposition in either transcription breaks the agreement.
"""

import argparse
import sys

# One row per dword register: offset -> (name, bits, chip, rw, pipelined, fifo)
#   bits:      width of the documented field ("31:0" etc., as printed)
#   chip:      C = Chuck, C+B = Chuck+Bruce*, B = Bruce*, B1 = Bruce-1 only
#              (* = written to the Bruces selected by the chip field;
#               reads always come from Chuck, V2 p.22)
#   rw:        R, W, or RW
#   pipelined: True/False, or None where the spec says n/a
#   fifo:      True if a write pushes onto the PCI FIFO, False if it takes
#              effect immediately (the race hazard, V2 p.22), None for n/a
#
# Reserved offsets are entered explicitly so a lookup can tell "reserved
# by the spec" apart from "you are off the end of the table".

STANDARD = {}


def _reg(off, name, bits, chip, rw, pipelined, fifo):
    assert off not in STANDARD, f"duplicate offset {off:#05x} ({name})"
    STANDARD[off] = (name, bits, chip, rw, pipelined, fifo)


def _reserved(off):
    _reg(off, "reserved", "n/a", "n/a", "n/a", None, None)


# --- pp.22-23: status, interrupt, and the triangle parameter registers ------
_reg(0x000, "status", "31:0", "C", "R", True, None)
_reg(0x004, "intrCtrl", "31:0", "C", "RW", True, False)
for i, v in enumerate(("Ax", "Ay", "Bx", "By", "Cx", "Cy")):
    _reg(0x008 + 4 * i, f"vertex{v}", "15:0", "C+B", "W", True, True)
# Integer start parameters, then the X and Y gradients, in register order.
_reg(0x020, "startR", "23:0", "C", "W", True, True)
_reg(0x024, "startG", "23:0", "C", "W", True, True)
_reg(0x028, "startB", "23:0", "C", "W", True, True)
_reg(0x02C, "startZ", "31:0", "C", "W", True, True)
_reg(0x030, "startA", "23:0", "C", "W", True, True)
_reg(0x034, "startS", "31:0", "B", "W", True, True)
_reg(0x038, "startT", "31:0", "B", "W", True, True)
_reg(0x03C, "startW", "31:0", "C+B", "W", True, True)
for base, ax in ((0x040, "X"), (0x060, "Y")):
    _reg(base + 0x00, f"dRd{ax}", "23:0", "C", "W", True, True)
    _reg(base + 0x04, f"dGd{ax}", "23:0", "C", "W", True, True)
    _reg(base + 0x08, f"dBd{ax}", "23:0", "C", "W", True, True)
    _reg(base + 0x0C, f"dZd{ax}", "31:0", "C", "W", True, True)
    _reg(base + 0x10, f"dAd{ax}", "23:0", "C", "W", True, True)
    _reg(base + 0x14, f"dSd{ax}", "31:0", "B", "W", True, True)
    _reg(base + 0x18, f"dTd{ax}", "31:0", "B", "W", True, True)
    _reg(base + 0x1C, f"dWd{ax}", "31:0", "C+B", "W", True, True)
_reg(0x080, "triangleCMD", "31", "C+B", "W", True, True)
_reserved(0x084)
# The IEEE-single mirrors of everything above, same order.
for i, v in enumerate(("Ax", "Ay", "Bx", "By", "Cx", "Cy")):
    _reg(0x088 + 4 * i, f"fvertex{v}", "31:0", "C+B", "W", True, True)
_reg(0x0A0, "fstartR", "31:0", "C", "W", True, True)
_reg(0x0A4, "fstartG", "31:0", "C", "W", True, True)
_reg(0x0A8, "fstartB", "31:0", "C", "W", True, True)
_reg(0x0AC, "fstartZ", "31:0", "C", "W", True, True)
_reg(0x0B0, "fstartA", "31:0", "C", "W", True, True)
_reg(0x0B4, "fstartS", "31:0", "B", "W", True, True)
_reg(0x0B8, "fstartT", "31:0", "B", "W", True, True)
_reg(0x0BC, "fstartW", "31:0", "C+B", "W", True, True)
for base, ax in ((0x0C0, "X"), (0x0E0, "Y")):
    _reg(base + 0x00, f"fdRd{ax}", "31:0", "C", "W", True, True)
    _reg(base + 0x04, f"fdGd{ax}", "31:0", "C", "W", True, True)
    _reg(base + 0x08, f"fdBd{ax}", "31:0", "C", "W", True, True)
    _reg(base + 0x0C, f"fdZd{ax}", "31:0", "C", "W", True, True)
    _reg(base + 0x10, f"fdAd{ax}", "31:0", "C", "W", True, True)
    _reg(base + 0x14, f"fdSd{ax}", "31:0", "B", "W", True, True)
    _reg(base + 0x18, f"fdTd{ax}", "31:0", "B", "W", True, True)
    _reg(base + 0x1C, f"fdWd{ax}", "31:0", "C+B", "W", True, True)
_reg(0x100, "ftriangleCMD", "31", "C+B", "W", True, True)

# --- p.24: mode registers, commands, statistics, fog, CMDFIFO ---------------
_reg(0x104, "fbzColorPath", "29:0", "C+B", "RW", True, True)
_reg(0x108, "fogMode", "7:0", "C", "RW", True, True)
_reg(0x10C, "alphaMode", "31:0", "C", "RW", True, True)
_reg(0x110, "fbzMode", "21:0", "C", "RW", False, True)
_reg(0x114, "lfbMode", "16:0", "C", "RW", False, True)
_reg(0x118, "clipLeftRight", "31:0", "C", "RW", False, True)
_reg(0x11C, "clipLowYHighY", "31:0", "C", "RW", False, True)
_reg(0x120, "nopCMD", "1:0", "C+B", "W", False, True)
_reg(0x124, "fastfillCMD", "n/a", "C", "W", False, True)
_reg(0x128, "swapbufferCMD", "9:0", "C", "W", False, True)
_reg(0x12C, "fogColor", "23:0", "C", "W", False, True)
_reg(0x130, "zaColor", "31:0", "C", "W", False, True)
_reg(0x134, "chromaKey", "23:0", "C+B", "W", False, True)
_reg(0x138, "chromaRange", "27:0", "C+B", "W", False, True)
_reg(0x13C, "userIntrCMD", "9:0", "C", "W", False, True)
_reg(0x140, "stipple", "31:0", "C", "RW", False, True)
_reg(0x144, "color0", "31:0", "C", "RW", False, True)
_reg(0x148, "color1", "31:0", "C", "RW", False, True)
_reg(0x14C, "fbiPixelsIn", "23:0", "C", "R", None, None)
_reg(0x150, "fbiChromaFail", "23:0", "C", "R", None, None)
_reg(0x154, "fbiZfuncFail", "23:0", "C", "R", None, None)
_reg(0x158, "fbiAfuncFail", "23:0", "C", "R", None, None)
_reg(0x15C, "fbiPixelsOut", "23:0", "C", "R", None, None)
for i in range(32):  # 0x160..0x1DC — 64 entries, two per word
    _reg(0x160 + 4 * i, f"fogTable[{i}]", "31:0", "C", "W", False, True)
_reg(0x1E0, "cmdFifoBaseAddr", "25:0", "C", "RW", None, False)
_reg(0x1E4, "cmdFifoBump", "15:0", "C", "RW", None, False)
_reg(0x1E8, "cmdFifoRdPtr", "31:0", "C", "RW", None, False)
_reg(0x1EC, "cmdFifoAMin", "31:0", "C", "RW", None, False)
_reg(0x1F0, "cmdFifoAMax", "31:0", "C", "RW", None, False)
_reg(0x1F4, "cmdFifoDepth", "15:0", "C", "RW", None, False)
_reg(0x1F8, "cmdFifoHoles", "15:0", "C", "RW", None, False)
_reserved(0x1FC)

# --- pp.24-25: init and video, all single-cycle non-FIFO --------------------
_reg(0x200, "fbiInit4", "12:0", "C", "RW", None, False)
_reg(0x204, "vRetrace", "12:0", "C", "R", None, False)
_reg(0x208, "backPorch", "24:0", "C", "RW", None, False)
_reg(0x20C, "videoDimensions", "26:0", "C", "RW", None, False)
_reg(0x210, "fbiInit0", "31:0", "C", "RW", None, False)
_reg(0x214, "fbiInit1", "31:0", "C", "RW", None, False)
_reg(0x218, "fbiInit2", "31:0", "C", "RW", None, False)
_reg(0x21C, "fbiInit3", "31:0", "C", "RW", None, False)
_reg(0x220, "hSync", "26:0", "C", "W", None, False)
_reg(0x224, "vSync", "28:0", "C", "W", None, False)
_reg(0x228, "clutData", "29:0", "C", "W", False, True)
_reg(0x22C, "dacData", "13:0", "C", "W", None, False)
_reg(0x230, "maxRgbDelta", "23:0", "C", "W", None, False)
_reg(0x234, "hBorder", "24:0", "C", "W", None, False)
_reg(0x238, "vBorder", "24:0", "C", "W", None, False)
_reg(0x23C, "borderColor", "23:0", "C", "W", None, False)
_reg(0x240, "hvRetrace", "26:0", "C", "R", None, False)
_reg(0x244, "fbiInit5", "31:0", "C", "RW", None, False)
_reg(0x248, "fbiInit6", "31:0", "C", "RW", None, False)
_reg(0x24C, "fbiInit7", "31:0", "C", "RW", None, False)
_reserved(0x250)
_reserved(0x254)
_reg(0x258, "fbiSwapHistory", "31:0", "C", "R", None, None)
_reg(0x25C, "fbiTrianglesOut", "23:0", "C", "R", None, None)

# --- p.25: the on-chip triangle setup block (all new on Voodoo2) ------------
_reg(0x260, "sSetupMode", "19:0", "C", "W", True, True)
_reg(0x264, "sVx", "31:0", "C+B", "W", True, True)
_reg(0x268, "sVy", "31:0", "C+B", "W", True, True)
_reg(0x26C, "sARGB", "31:0", "C+B", "W", True, True)
_reg(0x270, "sRed", "31:0", "C", "W", True, True)
_reg(0x274, "sGreen", "31:0", "C", "W", True, True)
_reg(0x278, "sBlue", "31:0", "C", "W", True, True)
_reg(0x27C, "sAlpha", "31:0", "C", "W", True, True)
_reg(0x280, "sVz", "31:0", "C", "W", True, True)
_reg(0x284, "sWb", "31:0", "C+B", "W", True, True)
_reg(0x288, "sWtmu0", "31:0", "B", "W", True, True)
_reg(0x28C, "sS/W0", "31:0", "B", "W", True, True)
_reg(0x290, "sT/W0", "31:0", "B", "W", True, True)
_reg(0x294, "sWtmu1", "31:0", "B1", "W", True, True)
_reg(0x298, "sS/Wtmu1", "31:0", "B1", "W", True, True)
_reg(0x29C, "sT/Wtmu1", "31:0", "B1", "W", True, True)
_reg(0x2A0, "sDrawTriCMD", "31:0", "C+B", "W", True, True)
_reg(0x2A4, "sBeginTriCMD", "31:0", "C", "W", True, True)
for off in range(0x2A8, 0x2C0, 4):
    _reserved(off)

# --- pp.25-26: the 2D BitBLT engine (a NON-GOAL for the card model) ---------
_reg(0x2C0, "bltSrcBaseAddr", "21:0", "C", "RW", True, True)
_reg(0x2C4, "bltDstBaseAddr", "21:0", "C", "RW", True, True)
_reg(0x2C8, "bltXYStrides", "27:0", "C", "RW", True, True)
_reg(0x2CC, "bltSrcChromaRange", "31:0", "C", "RW", True, True)
_reg(0x2D0, "bltDstChromaRange", "31:0", "C", "RW", True, True)
_reg(0x2D4, "bltClipX", "27:0", "C", "RW", True, True)
_reg(0x2D8, "bltClipY", "27:0", "C", "RW", True, True)
_reserved(0x2DC)
_reg(0x2E0, "bltSrcXY", "26:0", "C", "RW", True, True)
_reg(0x2E4, "bltDstXY", "31:0", "C", "RW", True, True)
_reg(0x2E8, "bltSize", "31:0", "C", "RW", True, True)
_reg(0x2EC, "bltRop", "15:0", "C", "RW", True, True)
_reg(0x2F0, "bltColor", "31:0", "C", "RW", True, True)
_reserved(0x2F4)
_reg(0x2F8, "bltCommand", "31:0", "C", "RW", True, True)
_reg(0x2FC, "bltData", "31:0", "C", "W", True, True)

# --- p.26: the per-TMU registers --------------------------------------------
_reg(0x300, "textureMode", "30:0", "B", "W", True, True)
_reg(0x304, "tLOD", "27:0", "B", "W", True, True)
_reg(0x308, "tDetail", "21:0", "B", "W", True, True)
_reg(0x30C, "texBaseAddr", "18:0", "B", "W", True, True)
_reg(0x310, "texBaseAddr_1", "18:0", "B", "W", True, True)
_reg(0x314, "texBaseAddr_2", "18:0", "B", "W", True, True)
_reg(0x318, "texBaseAddr_3_8", "18:0", "B", "W", True, True)
_reg(0x31C, "trexInit0", "31:0", "B", "W", False, True)
_reg(0x320, "trexInit1", "31:0", "B", "W", False, True)
for i in range(12):  # 0x324..0x350
    _reg(0x324 + 4 * i, f"nccTable0[{i}]", "31:0", "B", "W", False, True)
for i in range(12):  # 0x354..0x380
    _reg(0x354 + 4 * i, f"nccTable1[{i}]", "31:0", "B", "W", False, True)
for off in range(0x384, 0x400, 4):
    _reserved(off)


# --- The alternate triangle mapping, transcribed from pp.27-29 --------------
#
# Only the offsets are listed: enabling the remap changes WHERE the
# triangle parameters sit, not what they are, and p.27 states it "has no
# affect [on] any registers not specified in the table" — so every other
# attribute is required (by --check) to come from the standard table.
# The one transcription quirk worth recording: p.27's own row prints
# status as "R/W | Yes / Yes", contradicting p.22 and §5.1 ("the status
# register is read only") — Voodoo1 leftover; the standard row is truth.
ALTERNATE_NAMES = {
    0x000: "status",
    0x004: "reserved",
    0x008: "vertexAx", 0x00C: "vertexAy", 0x010: "vertexBx", 0x014: "vertexBy",
    0x018: "vertexCx", 0x01C: "vertexCy",
    0x020: "startR", 0x024: "dRdX", 0x028: "dRdY",
    0x02C: "startG", 0x030: "dGdX", 0x034: "dGdY",
    0x038: "startB", 0x03C: "dBdX", 0x040: "dBdY",
    0x044: "startZ", 0x048: "dZdX", 0x04C: "dZdY",
    0x050: "startA", 0x054: "dAdX", 0x058: "dAdY",
    0x05C: "startS", 0x060: "dSdX", 0x064: "dSdY",
    0x068: "startT", 0x06C: "dTdX", 0x070: "dTdY",
    0x074: "startW", 0x078: "dWdX", 0x07C: "dWdY",
    0x080: "triangleCMD",
    0x084: "reserved",
    0x088: "fvertexAx", 0x08C: "fvertexAy", 0x090: "fvertexBx", 0x094: "fvertexBy",
    0x098: "fvertexCx", 0x09C: "fvertexCy",
    0x0A0: "fstartR", 0x0A4: "fdRdX", 0x0A8: "fdRdY",
    0x0AC: "fstartG", 0x0B0: "fdGdX", 0x0B4: "fdGdY",
    0x0B8: "fstartB", 0x0BC: "fdBdX", 0x0C0: "fdBdY",
    0x0C4: "fstartZ", 0x0C8: "fdZdX", 0x0CC: "fdZdY",
    0x0D0: "fstartA", 0x0D4: "fdAdX", 0x0D8: "fdAdY",
    0x0DC: "fstartS", 0x0E0: "fdSdX", 0x0E4: "fdSdY",
    0x0E8: "fstartT", 0x0EC: "fdTdX", 0x0F0: "fdTdY",
    0x0F4: "fstartW", 0x0F8: "fdWdX", 0x0FC: "fdWdY",
    0x100: "ftriangleCMD",
}


def reg_by_offset(offset, alt=False):
    """(name, bits, chip, rw, pipelined, fifo) for a register-face offset.

    `offset` is the dword-aligned offset within the 16 KB register map
    (wrap, chip-select and swizzle fields already stripped).  With
    alt=True, offsets 0x000-0x100 resolve through the alternate triangle
    mapping; everything else is unaffected by the remap.
    """
    offset &= 0x3FC
    if alt and offset in ALTERNATE_NAMES:
        name = ALTERNATE_NAMES[offset]
        if name == "reserved":
            return ("reserved", "n/a", "n/a", "n/a", None, None)
        std_off = next(o for o, r in STANDARD.items() if r[0] == name)
        return STANDARD[std_off]
    return STANDARD[offset]


def reg_name(offset, alt=False):
    """Register name for a register-face offset (see reg_by_offset)."""
    return reg_by_offset(offset, alt)[0]


def _generate_alternate():
    """Regenerate pp.27-29 from the standard table by the documented rule.

    The remap makes each parameter's start/dX/dY adjacent, parameters in
    the order the standard table lists the start registers (R,G,B,Z,A,
    S,T,W), integer block then float block; vertices and both
    triangleCMDs keep their standard offsets.

    Derived from the STANDARD table itself — not from a second hardcoded
    parameter order, which would let one transposition hide in both
    copies: the parameter order comes from the standard start-register
    block in offset order, so a transposed standard entry disagrees with
    the pp.27-29 transcription here.
    """
    gen = {0x000: "status", 0x004: "reserved", 0x084: "reserved",
           0x080: "triangleCMD", 0x100: "ftriangleCMD"}
    for off in range(0x008, 0x020, 4):
        gen[off] = STANDARD[off][0]  # vertex[A-C][xy]
    for off in range(0x088, 0x0A0, 4):
        gen[off] = STANDARD[off][0]  # fvertex[A-C][xy]
    for base, prefix in ((0x020, ""), (0x0A0, "f")):
        off = base
        for i in range(8):
            start = STANDARD[base + 4 * i][0]  # the start block, in offset order
            p = start[len(prefix) + 5:]  # "startR"/"fstartR" -> "R"
            for form in (start, f"{prefix}d{p}dX", f"{prefix}d{p}dY"):
                gen[off] = form
                off += 4
    return gen


def check():
    """The positive control.  Returns the number of failures found."""
    fails = 0

    def bad(msg):
        nonlocal fails
        fails += 1
        print(f"FAIL: {msg}")

    # Density and uniqueness: every dword offset in the 1 KB map, exactly
    # once, and names unique (a transposed entry usually collides).
    for off in range(0x000, 0x400, 4):
        if off not in STANDARD:
            bad(f"standard map has a hole at {off:#05x}")
    names = [r[0] for r in STANDARD.values() if r[0] != "reserved"]
    for n in set(names):
        if names.count(n) > 1:
            bad(f"register name {n!r} appears {names.count(n)} times")

    # The spec's own cross-check: regenerate the alternate mapping by rule
    # and require it to equal the transcription of pp.27-29.
    gen = _generate_alternate()
    for off in range(0x000, 0x104, 4):
        t, g = ALTERNATE_NAMES.get(off), gen.get(off)
        if t != g:
            bad(f"alternate map {off:#05x}: transcribed {t!r}, rule gives {g!r}")

    # The standard table's own gradient blocks must be ordered like its
    # start block: startP at base+4i pairs with dPdX at base+0x20+4i and
    # dPdY at base+0x40+4i (V2 pp.22-23) — a transposition inside a dX/dY
    # block never reaches the alternate-map comparison, so pin it here.
    for base, prefix in ((0x020, ""), (0x0A0, "f")):
        for i in range(8):
            p = STANDARD[base + 4 * i][0][len(prefix) + 5:]
            for delta, form in ((0x20, f"{prefix}d{p}dX"), (0x40, f"{prefix}d{p}dY")):
                got = STANDARD[base + delta + 4 * i][0]
                if got != form:
                    bad(f"standard map {base + delta + 4 * i:#05x}: "
                        f"expected {form!r} to pair with the start block, found {got!r}")

    # Every alternate name must exist in the standard table (attribute
    # identity — the remap moves registers, it does not redefine them).
    std_names = {r[0] for r in STANDARD.values()}
    for off, name in ALTERNATE_NAMES.items():
        if name != "reserved" and name not in std_names:
            bad(f"alternate map {off:#05x} names unknown register {name!r}")

    # Spot pins straight off the spec pages, so a wholesale shift of the
    # table (an off-by-one row during editing) cannot slip through.
    pins = [
        (0x000, "status", "R"), (0x080, "triangleCMD", "W"),
        (0x104, "fbzColorPath", "RW"), (0x120, "nopCMD", "W"),
        (0x124, "fastfillCMD", "W"), (0x128, "swapbufferCMD", "W"),
        (0x15C, "fbiPixelsOut", "R"), (0x210, "fbiInit0", "RW"),
        (0x21C, "fbiInit3", "RW"), (0x22C, "dacData", "W"),
        (0x240, "hvRetrace", "R"), (0x24C, "fbiInit7", "RW"),
        (0x2A4, "sBeginTriCMD", "W"), (0x300, "textureMode", "W"),
        (0x31C, "trexInit0", "W"),
    ]
    for off, name, rw in pins:
        got = STANDARD[off]
        if got[0] != name or got[3] != rw:
            bad(f"pin {off:#05x}: expected {name}/{rw}, table has {got[0]}/{got[3]}")

    # The non-FIFO set is exactly the hazard list of V2 p.22: the CMDFIFO
    # block and the $200+ init/video registers (minus clutData, which IS
    # FIFO'd — the one exception the spec calls out).
    non_fifo = sorted(o for o, r in STANDARD.items() if r[5] is False and r[0] != "reserved")
    expect = [0x004] + list(range(0x1E0, 0x1FC, 4)) + [
        o for o in range(0x200, 0x260, 4)
        if STANDARD[o][0] not in ("reserved", "clutData", "fbiSwapHistory", "fbiTrianglesOut")
    ]
    if non_fifo != sorted(expect):
        bad(f"non-FIFO register set does not match V2 p.22's hazard list: {non_fifo}")

    if fails == 0:
        print(f"voodoo2_regs: OK — {len(STANDARD)} standard rows, "
              f"{len(ALTERNATE_NAMES)} alternate rows, cross-validated")
    return fails


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("offset", nargs="?", help="register-face offset (hex ok) to name")
    ap.add_argument("--alt", action="store_true", help="use the alternate triangle mapping")
    ap.add_argument("--check", action="store_true", help="run the positive control")
    args = ap.parse_args()
    if args.check:
        sys.exit(1 if check() else 0)
    if args.offset is None:
        ap.print_help()
        return
    off = int(args.offset, 0)
    name, bits, chip, rw, pipelined, fifo = reg_by_offset(off, args.alt)
    flags = f"pipelined={pipelined} fifo={fifo}"
    print(f"{off & 0x3FC:#05x}: {name}  bits={bits} chip={chip} rw={rw} {flags}")


if __name__ == "__main__":
    main()
