# SPDX-License-Identifier: MIT
# Copyright (c) pappadf
#
# mach64_regs.py
# The ATI mach64 register cross-reference, as DATA — I/O select and
# memory-mapped DWORD offset per mnemonic, transcribed from the mach64
# Register Reference Guide (RRG-S00700-05, 1994) Chapter 2, "Registers
# Alphabetically by Mnemonic".
#
# This table exists to annotate a detokenized FCode listing, and it is kept
# separate from the detokenizer for one reason: an earlier hand scanner had
# I/O selects $1A and $1B transposed and therefore reported "CONFIG_CNTL
# appears nowhere in the FCode" — a confident wrong answer that turned a
# decoded fact into a blocking unknown.  `selftest()` below is the positive
# control that catches that class of mistake.
#
# Addressing (RRG ch. 1, "Register Mapping"):
#   I/O address  = (io_select << 10) | io_base,  io_base in {$2EC,$1CC,$1C8}
#   MMIO address = aperture_base + $7FFC00 + (dword << 2)   [8 MB aperture]
# CONFIG_CNTL is the one register with NO memory-mapped alias, which is why
# a mach64 cannot be brought up through memory space alone.

# mnemonic -> (io_select or None, mmio_dword or None)
MACH64_REGS = {
    # CRTC
    "CRTC_H_TOTAL_DISP": (0x00, 0x00),
    "CRTC_H_SYNC_STRT_WID": (0x01, 0x01),
    "CRTC_V_TOTAL_DISP": (0x02, 0x02),
    "CRTC_V_SYNC_STRT_WID": (0x03, 0x03),
    "CRTC_VLINE_CRNT_VLINE": (0x04, 0x04),
    "CRTC_OFF_PITCH": (0x05, 0x05),
    "CRTC_INT_CNTL": (0x06, 0x06),
    "CRTC_GEN_CNTL": (0x07, 0x07),
    # Overscan
    "OVR_CLR": (0x08, 0x10),
    "OVR_WID_LEFT_RIGHT": (0x09, 0x11),
    "OVR_WID_TOP_BOTTOM": (0x0A, 0x12),
    # Hardware cursor
    "CUR_CLR0": (0x0B, 0x18),
    "CUR_CLR1": (0x0C, 0x19),
    "CUR_OFFSET": (0x0D, 0x1A),
    "CUR_HORZ_VERT_POSN": (0x0E, 0x1B),
    "CUR_HORZ_VERT_OFF": (0x0F, 0x1C),
    # Scratch pad
    "SCRATCH_REG0": (0x10, 0x20),
    "SCRATCH_REG1": (0x11, 0x21),
    # Clock / bus / memory
    "CLOCK_CNTL": (0x12, 0x24),
    "BUS_CNTL": (0x13, 0x28),
    "MEM_CNTL": (0x14, 0x2C),
    "MEM_VGA_WP_SEL": (0x15, 0x2D),
    "MEM_VGA_RP_SEL": (0x16, 0x2E),
    # DAC
    "DAC_REGS": (0x17, 0x30),
    "DAC_CNTL": (0x18, 0x31),
    # Test / configuration
    "GEN_TEST_CNTL": (0x19, 0x34),
    "CONFIG_CNTL": (0x1A, None),  # I/O ONLY — no memory alias
    "CONFIG_CHIP_ID": (0x1B, 0x38),
    "CONFIG_STAT0": (0x1C, 0x39),
    "CONFIG_STAT1": (0x1D, 0x3A),
    # Draw engine (memory-mapped only; writes pass the command FIFO)
    "DST_OFF_PITCH": (None, 0x40),
    "DST_X": (None, 0x41),
    "DST_Y": (None, 0x42),
    "DST_Y_X": (None, 0x43),
    "DST_WIDTH": (None, 0x44),
    "DST_HEIGHT": (None, 0x45),
    "DST_HEIGHT_WIDTH": (None, 0x46),
    "DST_X_WIDTH": (None, 0x47),
    "DST_BRES_LNTH": (None, 0x48),
    "DST_BRES_ERR": (None, 0x49),
    "DST_BRES_INC": (None, 0x4A),
    "DST_BRES_DEC": (None, 0x4B),
    "DST_CNTL": (None, 0x4C),
    "SRC_OFF_PITCH": (None, 0x60),
    "SRC_X": (None, 0x61),
    "SRC_Y": (None, 0x62),
    "SRC_Y_X": (None, 0x63),
    "SRC_WIDTH1": (None, 0x64),
    "SRC_HEIGHT1": (None, 0x65),
    "SRC_HEIGHT1_WIDTH1": (None, 0x66),
    "SRC_X_START": (None, 0x67),
    "SRC_Y_START": (None, 0x68),
    "SRC_Y_X_START": (None, 0x69),
    "SRC_WIDTH2": (None, 0x6A),
    "SRC_HEIGHT2": (None, 0x6B),
    "SRC_HEIGHT2_WIDTH2": (None, 0x6C),
    "SRC_CNTL": (None, 0x6D),
    "HOST_DATA0": (None, 0x80),
    "HOST_CNTL": (None, 0x90),
    "PAT_REG0": (None, 0xA0),
    "PAT_REG1": (None, 0xA1),
    "PAT_CNTL": (None, 0xA2),
    "SC_LEFT": (None, 0xA8),
    "SC_RIGHT": (None, 0xA9),
    "SC_LEFT_RIGHT": (None, 0xAA),
    "SC_TOP": (None, 0xAB),
    "SC_BOTTOM": (None, 0xAC),
    "SC_TOP_BOTTOM": (None, 0xAD),
    "DP_BKGD_CLR": (None, 0xB0),
    "DP_FRGD_CLR": (None, 0xB1),
    "DP_WRITE_MSK": (None, 0xB2),
    "DP_CHAIN_MSK": (None, 0xB3),
    "DP_PIX_WIDTH": (None, 0xB4),
    "DP_MIX": (None, 0xB5),
    "DP_SRC": (None, 0xB6),
    "CLR_CMP_CLR": (None, 0xC0),
    "CLR_CMP_MSK": (None, 0xC1),
    "CLR_CMP_CNTL": (None, 0xC2),
    "FIFO_STAT": (None, 0xC4),
    "CONTEXT_MSK": (None, 0xC8),
    "CONTEXT_LOAD_CNTL": (None, 0xCB),
    "GUI_TRAJ_CNTL": (None, 0xCC),
    "GUI_STAT": (None, 0xCE),
}

# The three strap-selectable sparse-I/O base addresses (RRG ch. 1).  $2EC is
# the default and the one the Apple board uses.
IO_BASES = (0x2EC, 0x1CC, 0x1C8)

# The memory-mapped register block sits at aperture + this offset, for the
# 8 MB aperture configuration the Apple FCode selects (RRG ch. 1).
MMIO_BLOCK_8MB = 0x7FFC00
MMIO_BLOCK_4MB = 0x3FFC00

# Reverse maps, built once.
_BY_IO = {}
_BY_DWORD = {}
for _name, (_io, _dw) in MACH64_REGS.items():
    if _io is not None:
        _BY_IO.setdefault(_io, _name)
    if _dw is not None:
        _BY_DWORD.setdefault(_dw, _name)


def io_decode(addr):
    """Name the mach64 register a sparse-I/O address selects, or None.

    Decodes (io_select << 10) | io_base.  Returns (name, io_select, io_base,
    byte_offset) where byte_offset is the address's offset within the
    32-bit register (the FCode reaches CONFIG_CNTL's upper halfword as
    base+2, which is a real access pattern, not a decode failure)."""
    for base in IO_BASES:
        if (addr & 0x3FF) < base or (addr & 0x3FF) > base + 3:
            continue
        sel = (addr >> 10) & 0x3F
        name = _BY_IO.get(sel)
        if name is None:
            return None
        return (name, sel, base, (addr & 0x3FF) - base)
    return None


def mmio_decode(offset):
    """Name the register at a byte offset into the memory-mapped block."""
    if offset % 4:
        return None
    return _BY_DWORD.get(offset >> 2)


def selftest():
    """Positive control for the table itself.

    Three independent anchors, each from a source outside this file:
      * the ATI manual's own statement that CONFIG_CNTL has no MMIO alias;
      * $72EC decoding as CONFIG_STAT0 (I/O select $1C over base $2EC),
        which is the literal the Apple FCode carries;
      * $6AEC decoding as CONFIG_CNTL — the entry an earlier scanner had
        transposed with CONFIG_CHIP_ID.
    Also asserts that no two registers claim the same I/O select or the
    same DWORD offset, which is what a transposition looks like from the
    inside."""
    assert MACH64_REGS["CONFIG_CNTL"][1] is None, "CONFIG_CNTL must have no MMIO alias"
    assert io_decode(0x72EC)[0] == "CONFIG_STAT0", "$72EC must be CONFIG_STAT0"
    assert io_decode(0x6AEC)[0] == "CONFIG_CNTL", "$6AEC must be CONFIG_CNTL"
    assert io_decode(0x6EEC)[0] == "CONFIG_CHIP_ID", "$6EEC must be CONFIG_CHIP_ID"
    assert io_decode(0x62EC)[0] == "DAC_CNTL", "$62EC must be DAC_CNTL"
    assert io_decode(0x6AEE)[3] == 2, "$6AEE is CONFIG_CNTL's upper halfword"
    assert io_decode(0x0400) is None, "an address off every base must not decode"
    seen_io, seen_dw = {}, {}
    for name, (io, dw) in MACH64_REGS.items():
        if io is not None:
            assert io not in seen_io, f"I/O select ${io:02X}: {name} vs {seen_io[io]}"
            seen_io[io] = name
        if dw is not None:
            assert dw not in seen_dw, f"DWORD ${dw:02X}: {name} vs {seen_dw[dw]}"
            seen_dw[dw] = name
    return True


if __name__ == "__main__":
    selftest()
    print(f"mach64_regs: {len(MACH64_REGS)} registers, self-test OK")
