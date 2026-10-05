#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) pappadf
#
# rage128_regs.py
# The ATI Rage 128 GL register cross-reference, as DATA: every register's
# name, the space it lives in and its offset there, with the manual page it
# is defined on.  The name <-> offset table the Rage 128 card model, its
# tests and the shell's register decoder are written against.
#
# Sources, in precedence order (the `src` column):
#   gl      ATI, "RAGE 128 VR / RAGE 128 GL Register Reference Guide",
#           RRG-G04100-C Rev 0.02 (1999) -- Appendix A.1, "Registers Sorted
#           by Name", cross-checked against every register's own header in
#           Chapters 3-8 (access, width and space come from the header).
#   cce     ATI, "Rage 128 Register Reference Supplement: CCE 3D Packets" --
#           the PM4_* command-engine block and the *_C 3D context registers
#           the GL guide omits.
#   pro     ATI, "RAGE 128 PRO Register Reference Guide" Rev 1.01 (2000),
#           Appendix A -- only for registers the GL drivers program that
#           neither GL source defines (the setup/scale/3D control block).
#   driver  Linux include/video/aty128.h and drivers/gpu/drm/r128/r128_drv.h
#           -- the last few offsets no ATI manual in hand prints (the CCE
#           PIO FIFO ports, PPLL_DIV_0..3, PM4_BUFFER_ADDR).
#
# Generated once from the OCR of those manuals, then hand-checked: every
# MMR offset that the Linux headers also define agrees with them, and the
# disagreements the check found are corrected below with a `note`:
#   * the GL guide prints the GUI scratch registers as GIU_SCRATCH_REGn;
#   * it gives INTERRUPT_LINE an MMR offset of $3C (it is config $3C, so
#     MMR $F3C through the config mirror) -- config-space rows carry their
#     CONFIG offset, and mmr_offset() adds the $F00 mirror base;
#   * the PRO index prints SCALE_PITCH at $1998, SCALE_OFFSET_0's offset;
#     the drivers' $199C is used.
# `selftest()` is the positive control: the register offsets the retail
# Xclaim VR 128 FCode names in its own constant table must decode to the
# registers the decode of that FCode says they are.
#
# Spaces:
#   mmr     a register-aperture offset (both 8 KB apertures decode the same
#           file; $0000-$00FF are also I/O-mapped)
#   cfg     PCI configuration space; read-only copy at MMR $0F00 + offset
#   pll     indexed through CLOCK_CNTL_INDEX/DATA ($08/$0C)
#   crt, attr, seq, gra, vga_io
#           VGA register banks -- never entered on a Macintosh
#
# Usage:
#   rage128_regs.py NAME|OFFSET...      look up by name or by MMR offset
#   rage128_regs.py --list [SPACE]      the whole table, or one space
#   rage128_regs.py --c-defines [SPACE] `#define R128_<NAME> 0x...` lines
#   rage128_regs.py --selftest

import argparse
import sys
from collections import namedtuple

Register = namedtuple("Register", "name space offset rw bits access src page note")

# (name, space, offset, rw, bits, access widths, source, manual page[, note])
# rw / bits / access are blank where only an index row defines the register.
_ROWS = (
    # --- mmr ---
    ('MM_INDEX', 'mmr', 0x0000, '', 0, '', 'gl', '4-14'),
    ('MM_DATA', 'mmr', 0x0004, '', 0, '', 'gl', '4-15'),
    ('CLOCK_CNTL_INDEX', 'mmr', 0x0008, 'RW', 32, '8/16/32', 'gl', '3-13'),
    ('CLOCK_CNTL_DATA', 'mmr', 0x000C, 'RW', 32, '8/16/32', 'gl', '3-13'),
    ('BIOS_0_SCRATCH', 'mmr', 0x0010, 'RW', 32, '8/16/32', 'gl', '8-14'),
    ('FCP_CNTL', 'mmr', 0x0012, '', 0, '', 'gl', '3-28'),
    ('BIOS_1_SCRATCH', 'mmr', 0x0014, 'RW', 32, '8/16/32', 'gl', '8-14'),
    ('BIOS_2_SCRATCH', 'mmr', 0x0018, 'RW', 32, '8/16/32', 'gl', '8-14'),
    ('BIOS_3_SCRATCH', 'mmr', 0x001C, 'RW', 32, '8/16/32', 'gl', '8-15'),
    ('BUS_CNTL', 'mmr', 0x0030, 'RW', 32, '8/16/32', 'gl', '4-25'),
    ('BUS_CNTL1', 'mmr', 0x0034, '', 0, '', 'gl', '3-46'),
    ('MEM_VGA_WP_SEL', 'mmr', 0x0038, 'RW', 32, '8/16/32', 'gl', '6-25'),
    ('MEM_VGA_RP_SEL', 'mmr', 0x003C, 'RW', 32, '8/16/32', 'gl', '6-25'),
    ('GEN_INT_CNTL', 'mmr', 0x0040, 'RW', 32, '32', 'gl', '8-1'),
    ('GEN_INT_STATUS', 'mmr', 0x0044, 'RW', 32, '8/16/32', 'gl', '8-2'),
    ('CRTC_GEN_CNTL', 'mmr', 0x0050, 'RW', 32, '8/16/32', 'gl', '6-1'),
    ('CRTC_EXT_CNTL', 'mmr', 0x0054, 'RW', 32, '8/16/32', 'gl', '6-3'),
    ('DAC_CNTL', 'mmr', 0x0058, 'RW', 32, '8/16/32', 'gl', '6-36'),
    ('CRTC_STATUS', 'mmr', 0x005C, '', 0, '', 'gl', '6-6'),
    ('GPIO_MONID', 'mmr', 0x0068, 'RW', 32, '8/16/32', 'gl', '3-1'),
    ('GPIO_MONIDB', 'mmr', 0x006C, 'RW', 32, '8/16/32', 'gl', '3-2'),
    ('PALETTE_INDEX', 'mmr', 0x00B0, 'RW', 32, '8/16/32', 'gl', '6-39'),
    ('PALETTE_DATA', 'mmr', 0x00B4, 'RW', 32, '8/16/32', 'gl', '6-40'),
    ('SNAPSHOT_VIF_COUNT', 'mmr', 0x00C4, 'RW', 32, '8/16/32', 'gl', '6-24'),
    ('CONFIG_CNTL', 'mmr', 0x00E0, 'RW', 32, '8/16/32', 'gl', '4-9'),
    ('CONFIG_XSTRAP', 'mmr', 0x00E4, 'RW', 32, '8/16/32', 'gl', '4-9'),
    ('CONFIG_BONDS', 'mmr', 0x00E8, 'RW', 32, '8/16/32', 'gl', '4-10'),
    ('GEN_RESET_CNTL', 'mmr', 0x00F0, 'RW', 32, '8/16/32', 'gl', '8-4'),
    ('CONFIG_MEMSIZE', 'mmr', 0x00F8, 'RW', 32, '8/16/32', 'gl', '4-11'),
    ('CONFIG_APER_0_BASE', 'mmr', 0x0100, 'R', 32, '8/16/32', 'gl', '4-11'),
    ('CONFIG_APER_1_BASE', 'mmr', 0x0104, 'RW', 32, '8/16/32', 'gl', '4-11'),
    ('CONFIG_APER_SIZE', 'mmr', 0x0108, 'R', 32, '8/16/32', 'gl', '4-13'),
    ('CONFIG_REG_1_BASE', 'mmr', 0x010C, 'R', 32, '8/16/32', 'gl', '4-13'),
    ('CONFIG_REG_APER_SIZE', 'mmr', 0x0110, '', 0, '', 'gl', '4-14'),
    ('CONFIG_MEMSIZE_EMBEDDED', 'mmr', 0x0114, '', 0, '', 'gl', '4-14'),
    ('TEST_DEBUG_CNTL', 'mmr', 0x0120, 'RW', 32, '8/16/32', 'gl', '3-7'),
    ('TEST_DEBUG_MUX', 'mmr', 0x0124, 'RW', 32, '8/16/32', 'gl', '3-9'),
    ('HW_DEBUG', 'mmr', 0x0128, 'RW', 32, '8/16/32', 'gl', '3-11'),
    ('TEST_DEBUG_OUT', 'mmr', 0x012C, 'R', 32, '8/16/32', 'gl', '3-10'),
    ('HOST_PATH_CNTL', 'mmr', 0x0130, 'RW', 32, '8/16/32', 'gl', '3-5'),
    ('SW_SEMAPHORE', 'mmr', 0x013C, 'RW', 32, '8/16/32', 'gl', '8-5'),
    ('MEM_CNTL', 'mmr', 0x0140, 'RW', 32, '8/16/32', 'gl', '6-25'),
    ('EXT_MEM_CNTL', 'mmr', 0x0144, 'RW', 32, '8/16/32', 'gl', '6-29'),
    ('MEM_ADDR_CONFIG', 'mmr', 0x0148, 'RW', 32, '8/16/32', 'gl', '6-22'),
    ('MEM_INTF_CNTL', 'mmr', 0x014C, 'RW', 32, '8/16/32', 'gl', '6-30'),
    ('MEM_STR_CNTL', 'mmr', 0x0150, 'RW', 32, '8/16/32', 'gl', '6-32'),
    ('MEM_INIT_LAT_TIMER', 'mmr', 0x0154, 'RW', 32, '8/16/32', 'gl', '6-34'),
    ('MEM_SDRAM_MODE_REG', 'mmr', 0x0158, 'RW', 32, '8/16/32', 'gl', '6-34'),
    ('AGP_BASE', 'mmr', 0x0170, '', 0, '', 'gl', '4-23'),
    ('AGP_CNTL', 'mmr', 0x0174, 'RW', 32, '8/16/32', 'gl', '4-23'),
    ('AGP_APER_OFFSET', 'mmr', 0x0178, 'R', 32, '8/16/32', 'gl', '4-24'),
    ('PCI_GART_PAGE', 'mmr', 0x017C, 'RW', 32, '8/16/32', 'gl', '4-12'),
    ('PC_NGUI_MODE', 'mmr', 0x0180, 'RW', 32, '32', 'gl', '3-32'),
    ('PC_NGUI_CTLSTAT', 'mmr', 0x0184, 'RW', 32, '32', 'gl', '3-38'),
    ('VIDEOMUX_CNTL', 'mmr', 0x0190, 'RW', 32, '8/16/32', 'gl', '3-3'),
    ('AMCGPIO_MASK', 'mmr', 0x0194, '', 0, '', 'gl', '8-6'),
    ('MDGPIO_MASK', 'mmr', 0x0198, '', 0, '', 'gl', '8-6'),
    ('AMCGPIO_A_REG', 'mmr', 0x01A0, '', 0, '', 'gl', '8-6'),
    ('AMCGPIO_Y_REG', 'mmr', 0x01A4, '', 0, '', 'gl', '8-7'),
    ('AMCGPIO_EN_REG', 'mmr', 0x01A8, '', 0, '', 'gl', '8-7'),
    ('MDGPIO_A_REG', 'mmr', 0x01AC, 'RW', 32, '8/16/32', 'gl', '8-8'),
    ('MDGPIO_EN_REG', 'mmr', 0x01B0, 'RW', 32, '8/16/32', 'gl', '8-8'),
    ('MDGPIO_Y_REG', 'mmr', 0x01B4, 'RW', 32, '8/16/32', 'gl', '8-8'),
    ('MPP_TB_CONFIG', 'mmr', 0x01C0, '', 0, '', 'pro', ''),
    ('CRTC_H_TOTAL_DISP', 'mmr', 0x0200, '', 0, '', 'gl', '6-6'),
    ('CRTC_H_SYNC_STRT_WID', 'mmr', 0x0204, 'RW', 32, '8/16/32', 'gl', '6-7'),
    ('CRTC_V_TOTAL_DISP', 'mmr', 0x0208, 'RW', 32, '8/16/32', 'gl', '6-7'),
    ('CRTC_V_SYNC_STRT_WID', 'mmr', 0x020C, 'RW', 32, '8/16/32', 'gl', '6-8'),
    ('CRTC_VLINE_CRNT_VLINE', 'mmr', 0x0210, 'RW', 32, '8/16/32', 'gl', '6-8'),
    ('CRTC_CRNT_FRAME', 'mmr', 0x0214, 'RW', 32, '8/16/32', 'gl', '6-13'),
    ('CRTC_GUI_TRIG_VLINE', 'mmr', 0x0218, 'RW', 32, '8/16/32', 'gl', '6-9'),
    ('CRTC_OFFSET', 'mmr', 0x0224, 'RW', 32, '8/16/32', 'gl', '6-10'),
    ('CRTC_OFFSET_CNTL', 'mmr', 0x0228, 'RW', 32, '8/16/32', 'gl', '6-11'),
    ('CRTC_PITCH', 'mmr', 0x022C, 'RW', 32, '8/16/32', 'gl', '6-13'),
    ('OVR_CLR', 'mmr', 0x0230, '', 0, '', 'gl', '6-16'),
    ('OVR_WID_LEFT_RIGHT', 'mmr', 0x0234, '', 0, '', 'gl', '6-16'),
    ('OVR_WID_TOP_BOTTOM', 'mmr', 0x0238, 'RW', 32, '8/16/32', 'gl', '6-17'),
    ('SNAPSHOT_VH_COUNTS', 'mmr', 0x0240, 'R', 32, '8/16/32', 'gl', '6-23'),
    ('SNAPSHOT_F_COUNT', 'mmr', 0x0244, 'R', 32, '8/16/32', 'gl', '6-23'),
    ('N_VIF_COUNT', 'mmr', 0x0248, 'RW', 32, '8/16/32', 'gl', '6-24'),
    ('CUR_OFFSET', 'mmr', 0x0260, '', 0, '', 'gl', '6-18'),
    ('CUR_HORZ_VERT_POSN', 'mmr', 0x0264, 'RW', 32, '8/16/32', 'gl', '6-18'),
    ('CUR_HORZ_VERT_OFF', 'mmr', 0x0268, 'RW', 32, '8/16/32', 'gl', '6-19'),
    ('CUR_CLR0', 'mmr', 0x026C, 'RW', 32, '8/16/32', 'gl', '6-20'),
    ('CUR_CLR1', 'mmr', 0x0270, 'RW', 32, '8/16/32', 'gl', '6-21'),
    ('DAC_CRC_SIG', 'mmr', 0x02CC, 'R', 32, '8/16/32', 'gl', '6-39'),
    ('DDA_CONFIG', 'mmr', 0x02E0, 'RW', 32, '8/16/32', 'gl', '6-14'),
    ('DDA_ON_OFF', 'mmr', 0x02E4, 'RW', 32, '8/16/32', 'gl', '6-14'),
    ('VGA_DDA_CONFIG', 'mmr', 0x02E8, 'RW', 32, '8/16/32', 'gl', ''),
    ('VGA_DDA_ON_OFF', 'mmr', 0x02EC, 'RW', 32, '8/16/32', 'gl', '6-15'),
    ('OV0_SCALE_CNTL', 'mmr', 0x0420, '', 0, '', 'pro', ''),
    ('PM4_BUFFER_OFFSET', 'mmr', 0x0700, 'RW', 32, '32', 'cce', ''),
    ('PM4_BUFFER_CNTL', 'mmr', 0x0704, 'RW', 32, '32', 'cce', ''),
    ('PM4_BUFFER_WM_CNTL', 'mmr', 0x0708, 'RW', 32, '32', 'cce', ''),
    ('PM4_BUFFER_DL_RPTR_ADDR', 'mmr', 0x070C, 'RW', 32, '32', 'cce', ''),
    ('PM4_BUFFER_DL_RPTR', 'mmr', 0x0710, 'RW', 32, '32', 'cce', ''),
    ('PM4_BUFFER_DL_WPTR', 'mmr', 0x0714, 'RW', 32, '32', 'cce', ''),
    ('PM4_VC_FPU_SETUP', 'mmr', 0x071C, '', 0, '', 'pro', ''),
    ('PM4_VC_VLOFF', 'mmr', 0x0730, 'RW', 32, '32', 'cce', ''),
    ('PM4_VC_VLSIZE', 'mmr', 0x0734, 'RW', 32, '32', 'cce', ''),
    ('PM4_IW_INDOFF', 'mmr', 0x0738, 'RW', 32, '32', 'cce', ''),
    ('PM4_IW_INDSIZE', 'mmr', 0x073C, 'RW', 32, '32', 'cce', ''),
    ('CRC_CMDFIFO_ADDR', 'mmr', 0x0740, 'R', 32, '32', 'gl', '3-12'),
    ('CRC_CMDFIFO_DOUT', 'mmr', 0x0744, 'R', 32, '32', 'gl', '3-12'),
    ('PM4_FPU_STAT', 'mmr', 0x07A0, '', 0, '', 'pro', ''),
    ('PM4_STAT', 'mmr', 0x07B8, 'RW', 32, '32', 'cce', ''),
    ('PM4_MICROCODE_ADDR', 'mmr', 0x07D4, 'RW', 32, '32', 'cce', ''),
    ('PM4_MICROCODE_RADDR', 'mmr', 0x07D8, '', 0, '', 'driver', ''),
    ('PM4_MICROCODE_DATAH', 'mmr', 0x07DC, 'RW', 32, '32', 'cce', ''),
    ('PM4_MICROCODE_DATAL', 'mmr', 0x07E0, 'RW', 32, '32', 'cce', ''),
    ('PM4_CMDFIFO_ADDR', 'mmr', 0x07E4, '', 0, '', 'pro', ''),
    ('PM4_BUFFER_ADDR', 'mmr', 0x07F0, '', 0, '', 'driver', ''),
    ('PM4_MICRO_CNTL', 'mmr', 0x07FC, 'RW', 32, '32', 'cce', ''),
    ('VID_BUFFER_CONTROL', 'mmr', 0x0900, '', 0, '', 'gl', '8-9'),
    ('SURFACE_DELAY', 'mmr', 0x0B00, '', 0, '', 'gl', '4-15'),
    ('SURFACE0_LOWER_BOUND', 'mmr', 0x0B04, '', 0, '', 'gl', '4-15'),
    ('SURFACE0_UPPER_BOUND', 'mmr', 0x0B08, 'RW', 32, '32', 'gl', '4-16'),
    ('SURFACE0_INFO', 'mmr', 0x0B0C, 'RW', 32, '32', 'gl', '4-18'),
    ('SURFACE1_LOWER_BOUND', 'mmr', 0x0B14, 'RW', 32, '32', 'gl', '4-16'),
    ('SURFACE1_UPPER_BOUND', 'mmr', 0x0B18, 'RW', 32, '32', 'gl', '4-17'),
    ('SURFACE1_INFO', 'mmr', 0x0B1C, 'RW', 32, '32', 'gl', '4-19'),
    ('SURFACE2_LOWER_BOUND', 'mmr', 0x0B24, 'RW', 32, '32', 'gl', '4-16'),
    ('SURFACE2_UPPER_BOUND', 'mmr', 0x0B28, 'RW', 32, '32', 'gl', '4-17'),
    ('SURFACE2_INFO', 'mmr', 0x0B2C, 'RW', 32, '32', 'gl', '4-20'),
    ('SURFACE3_LOWER_BOUND', 'mmr', 0x0B34, 'RW', 32, '32', 'gl', '4-16'),
    ('SURFACE3_UPPER_BOUND', 'mmr', 0x0B38, 'RW', 32, '32', 'gl', '4-17'),
    ('SURFACE3_INFO', 'mmr', 0x0B3C, 'RW', 32, '32', 'gl', '4-21'),
    ('PM4_FIFO_DATA_EVEN', 'mmr', 0x1000, '', 0, '', 'driver', ''),
    ('PM4_FIFO_DATA_ODD', 'mmr', 0x1004, '', 0, '', 'driver', ''),
    ('DST_OFFSET', 'mmr', 0x1404, 'RW', 32, '32', 'gl', '7-1'),
    ('DST_PITCH', 'mmr', 0x1408, 'RW', 32, '32', 'gl', '7-2'),
    ('DST_WIDTH', 'mmr', 0x140C, 'RW', 32, '32', 'gl', '7-4'),
    ('DST_HEIGHT', 'mmr', 0x1410, 'RW', 32, '32', 'gl', '7-4'),
    ('SRC_X', 'mmr', 0x1414, 'RW', 32, '32', 'gl', '7-15'),
    ('SRC_Y', 'mmr', 0x1418, 'RW', 32, '32', 'gl', '7-15'),
    ('DST_X', 'mmr', 0x141C, 'RW', 32, '32', 'gl', '7-2'),
    ('DST_Y', 'mmr', 0x1420, 'RW', 32, '32', 'gl', '7-2'),
    ('SRC_PITCH_OFFSET', 'mmr', 0x1428, 'W', 32, '32', 'gl', '7-14'),
    ('DST_PITCH_OFFSET', 'mmr', 0x142C, 'W', 32, '32', 'gl', '7-1'),
    ('SRC_Y_X', 'mmr', 0x1434, 'W', 32, '32', 'gl', '7-15'),
    ('DST_Y_X', 'mmr', 0x1438, 'W', 32, '32', 'gl', '7-3'),
    ('DST_HEIGHT_WIDTH', 'mmr', 0x143C, 'W', 32, '32', 'gl', '7-4'),
    ('DP_GUI_MASTER_CNTL', 'mmr', 0x146C, 'RW', 32, '32', 'gl', '7-38'),
    ('BRUSH_SCALE', 'mmr', 0x1470, 'RW', 32, '32', 'gl', '7-31'),
    ('BRUSH_Y_X', 'mmr', 0x1474, 'RW', 32, '32', 'gl', '7-31'),
    ('DP_BRUSH_BKGD_CLR', 'mmr', 0x1478, 'RW', 32, '32', 'gl', '7-32'),
    ('DP_BRUSH_FRGD_CLR', 'mmr', 0x147C, 'RW', 32, '32', 'gl', '7-32'),
    ('BRUSH_DATA0', 'mmr', 0x1480, '', 0, '', 'gl', '7-18'),
    ('BRUSH_DATA1', 'mmr', 0x1484, '', 0, '', 'gl', '7-18'),
    ('BRUSH_DATA2', 'mmr', 0x1488, '', 0, '', 'gl', '7-18'),
    ('BRUSH_DATA3', 'mmr', 0x148C, '', 0, '', 'gl', '7-18'),
    ('BRUSH_DATA4', 'mmr', 0x1490, 'RW', 32, '32', 'gl', '7-19'),
    ('BRUSH_DATA5', 'mmr', 0x1494, 'RW', 32, '32', 'gl', '7-19'),
    ('BRUSH_DATA6', 'mmr', 0x1498, 'RW', 32, '32', 'gl', '7-19'),
    ('BRUSH_DATA7', 'mmr', 0x149C, 'RW', 32, '32', 'gl', '7-19'),
    ('BRUSH_DATA8', 'mmr', 0x14A0, 'RW', 32, '32', 'gl', '7-19'),
    ('BRUSH_DATA9', 'mmr', 0x14A4, 'RW', 32, '32', 'gl', '7-20'),
    ('BRUSH_DATA10', 'mmr', 0x14A8, 'RW', 32, '32', 'gl', '7-20'),
    ('BRUSH_DATA11', 'mmr', 0x14AC, 'RW', 32, '32', 'gl', '7-20'),
    ('BRUSH_DATA12', 'mmr', 0x14B0, 'RW', 32, '32', 'gl', '7-20'),
    ('BRUSH_DATA13', 'mmr', 0x14B4, 'RW', 32, '32', 'gl', '7-20'),
    ('BRUSH_DATA14', 'mmr', 0x14B8, 'RW', 32, '32', 'gl', '7-21'),
    ('BRUSH_DATA15', 'mmr', 0x14BC, 'RW', 32, '32', 'gl', '7-21'),
    ('BRUSH_DATA16', 'mmr', 0x14C0, 'RW', 32, '32', 'gl', '7-21'),
    ('BRUSH_DATA17', 'mmr', 0x14C4, 'RW', 32, '32', 'gl', '7-21'),
    ('BRUSH_DATA18', 'mmr', 0x14C8, 'RW', 32, '32', 'gl', '7-21'),
    ('BRUSH_DATA19', 'mmr', 0x14CC, 'RW', 32, '32', 'gl', '7-22'),
    ('BRUSH_DATA20', 'mmr', 0x14D0, 'RW', 32, '32', 'gl', '7-22'),
    ('BRUSH_DATA21', 'mmr', 0x14D4, 'RW', 32, '32', 'gl', '7-22'),
    ('BRUSH_DATA22', 'mmr', 0x14D8, 'RW', 32, '32', 'gl', '7-22'),
    ('BRUSH_DATA23', 'mmr', 0x14DC, 'RW', 32, '32', 'gl', '7-22'),
    ('BRUSH_DATA24', 'mmr', 0x14E0, 'RW', 32, '32', 'gl', '7-23'),
    ('BRUSH_DATA25', 'mmr', 0x14E4, 'RW', 32, '32', 'gl', '7-23'),
    ('BRUSH_DATA26', 'mmr', 0x14E8, 'RW', 32, '32', 'gl', '7-23'),
    ('BRUSH_DATA27', 'mmr', 0x14EC, 'RW', 32, '32', 'gl', '7-23'),
    ('BRUSH_DATA28', 'mmr', 0x14F0, 'RW', 32, '32', 'gl', '7-23'),
    ('BRUSH_DATA29', 'mmr', 0x14F4, 'RW', 32, '32', 'gl', '7-24'),
    ('BRUSH_DATA30', 'mmr', 0x14F8, 'RW', 32, '32', 'gl', '7-24'),
    ('BRUSH_DATA31', 'mmr', 0x14FC, 'RW', 32, '32', 'gl', '7-24'),
    ('BRUSH_DATA32', 'mmr', 0x1500, 'RW', 32, '32', 'gl', '7-24'),
    ('BRUSH_DATA33', 'mmr', 0x1504, 'RW', 32, '32', 'gl', '7-24'),
    ('BRUSH_DATA34', 'mmr', 0x1508, 'RW', 32, '32', 'gl', '7-25'),
    ('BRUSH_DATA35', 'mmr', 0x150C, 'RW', 32, '32', 'gl', '7-25'),
    ('BRUSH_DATA36', 'mmr', 0x1510, 'RW', 32, '32', 'gl', '7-25'),
    ('BRUSH_DATA37', 'mmr', 0x1514, 'RW', 32, '32', 'gl', '7-25'),
    ('BRUSH_DATA38', 'mmr', 0x1518, 'RW', 32, '32', 'gl', '7-25'),
    ('BRUSH_DATA39', 'mmr', 0x151C, 'RW', 32, '32', 'gl', '7-26'),
    ('BRUSH_DATA40', 'mmr', 0x1520, 'RW', 32, '32', 'gl', '7-26'),
    ('BRUSH_DATA41', 'mmr', 0x1524, 'RW', 32, '32', 'gl', '7-26'),
    ('BRUSH_DATA42', 'mmr', 0x1528, 'RW', 32, '32', 'gl', '7-26'),
    ('BRUSH_DATA43', 'mmr', 0x152C, 'RW', 32, '32', 'gl', '7-26'),
    ('BRUSH_DATA44', 'mmr', 0x1530, 'RW', 32, '32', 'gl', '7-27'),
    ('BRUSH_DATA45', 'mmr', 0x1534, 'RW', 32, '32', 'gl', '7-27'),
    ('BRUSH_DATA46', 'mmr', 0x1538, 'RW', 32, '32', 'gl', '7-27'),
    ('BRUSH_DATA47', 'mmr', 0x153C, 'RW', 32, '32', 'gl', '7-27'),
    ('BRUSH_DATA48', 'mmr', 0x1540, 'RW', 32, '32', 'gl', '7-27'),
    ('BRUSH_DATA49', 'mmr', 0x1544, 'RW', 32, '32', 'gl', '7-28'),
    ('BRUSH_DATA50', 'mmr', 0x1548, 'RW', 32, '32', 'gl', '7-28'),
    ('BRUSH_DATA51', 'mmr', 0x154C, 'RW', 32, '32', 'gl', '7-28'),
    ('BRUSH_DATA52', 'mmr', 0x1550, 'RW', 32, '32', 'gl', '7-28'),
    ('BRUSH_DATA53', 'mmr', 0x1554, 'RW', 32, '32', 'gl', '7-28'),
    ('BRUSH_DATA54', 'mmr', 0x1558, 'RW', 32, '32', 'gl', '7-29'),
    ('BRUSH_DATA55', 'mmr', 0x155C, 'RW', 32, '32', 'gl', '7-29'),
    ('BRUSH_DATA56', 'mmr', 0x1560, 'RW', 32, '32', 'gl', '7-29'),
    ('BRUSH_DATA57', 'mmr', 0x1564, 'RW', 32, '32', 'gl', '7-29'),
    ('BRUSH_DATA58', 'mmr', 0x1568, 'RW', 32, '32', 'gl', '7-29'),
    ('BRUSH_DATA59', 'mmr', 0x156C, 'RW', 32, '32', 'gl', '7-30'),
    ('BRUSH_DATA60', 'mmr', 0x1570, 'RW', 32, '32', 'gl', '7-30'),
    ('BRUSH_DATA61', 'mmr', 0x1574, 'RW', 32, '32', 'gl', '7-30'),
    ('BRUSH_DATA62', 'mmr', 0x1578, 'RW', 32, '32', 'gl', '7-30'),
    ('BRUSH_DATA63', 'mmr', 0x157C, 'RW', 32, '32', 'gl', '7-30'),
    ('DST_WIDTH_X', 'mmr', 0x1588, 'W', 32, '32', 'gl', '7-6'),
    ('DST_HEIGHT_WIDTH_8', 'mmr', 0x158C, 'W', 32, '32', 'gl', '7-6'),
    ('SRC_X_Y', 'mmr', 0x1590, 'W', 32, '32', 'gl', '7-15'),
    ('DST_X_Y', 'mmr', 0x1594, 'W', 32, '32', 'gl', '7-3'),
    ('DST_WIDTH_HEIGHT', 'mmr', 0x1598, 'W', 32, '32', 'gl', '7-5'),
    ('DST_WIDTH_X_INCY', 'mmr', 0x159C, '', 0, '', 'gl', '7-7'),
    ('DST_HEIGHT_Y', 'mmr', 0x15A0, 'W', 32, '32', 'gl', '7-6'),
    ('DST_X_SUB', 'mmr', 0x15A4, 'RW', 32, '32', 'gl', '7-8'),
    ('DST_Y_SUB', 'mmr', 0x15A8, 'RW', 32, '32', 'gl', '7-8'),
    ('SRC_OFFSET', 'mmr', 0x15AC, 'RW', 32, '32', 'gl', '7-14'),
    ('SRC_PITCH', 'mmr', 0x15B0, 'RW', 32, '32', 'gl', '7-14'),
    ('DST_HEIGHT_WIDTH_BW', 'mmr', 0x15B4, 'W', 32, '32', 'gl', '7-5'),
    ('DST_WIDTH_BW', 'mmr', 0x15B4, 'W', 32, '32', 'gl', '7-9', 'the manual gives DST_HEIGHT_WIDTH_BW the same offset'),
    ('CLR_CMP_CNTL', 'mmr', 0x15C0, 'RW', 32, '32', 'gl', '7-53'),
    ('CLR_CMP_CLR_SRC', 'mmr', 0x15C4, 'RW', 32, '32', 'gl', '7-53'),
    ('CLR_CMP_CLR_DST', 'mmr', 0x15C8, 'RW', 32, '32', 'gl', '7-53'),
    ('CLR_CMP_MSK', 'mmr', 0x15CC, 'RW', 32, '32', 'gl', '7-54'),
    ('DP_SRC_FRGD_CLR', 'mmr', 0x15D8, 'RW', 32, '32', 'gl', '7-32'),
    ('DP_SRC_BKGD_CLR', 'mmr', 0x15DC, 'RW', 32, '32', 'gl', '7-32'),
    ('GUI_SCRATCH_REG0', 'mmr', 0x15E0, 'RW', 32, '32', 'gl', '7-57', 'printed GIU_SCRATCH_REG0'),
    ('GUI_SCRATCH_REG1', 'mmr', 0x15E4, 'RW', 32, '32', 'gl', '7-57', 'printed GIU_SCRATCH_REG1'),
    ('GUI_SCRATCH_REG2', 'mmr', 0x15E8, 'RW', 32, '32', 'gl', '7-57', 'printed GIU_SCRATCH_REG2'),
    ('GUI_SCRATCH_REG3', 'mmr', 0x15EC, 'RW', 32, '32', 'gl', '7-57', 'printed GIU_SCRATCH_REG3'),
    ('GUI_SCRATCH_REG4', 'mmr', 0x15F0, 'RW', 32, '32', 'gl', '7-58', 'printed GIU_SCRATCH_REG4'),
    ('GUI_SCRATCH_REG5', 'mmr', 0x15F4, 'RW', 32, '32', 'gl', '7-58', 'printed GIU_SCRATCH_REG5'),
    ('LEAD_BRES_ERR', 'mmr', 0x1600, 'W', 32, '32', 'gl', '7-10'),
    ('LEAD_BRES_INC', 'mmr', 0x1604, 'W', 32, '32', 'gl', '7-11'),
    ('LEAD_BRES_DEC', 'mmr', 0x1608, 'W', 32, '32', 'gl', '7-11'),
    ('TRAIL_BRES_ERR', 'mmr', 0x160C, 'RW', 32, '32', 'gl', '7-11'),
    ('TRAIL_BRES_INC', 'mmr', 0x1610, 'RW', 32, '32', 'gl', '7-12'),
    ('TRAIL_BRES_DEC', 'mmr', 0x1614, 'RW', 32, '32', 'gl', '7-12'),
    ('TRAIL_X', 'mmr', 0x1618, 'RW', 32, '32', 'gl', '7-12'),
    ('LEAD_BRETH_LNTH', 'mmr', 0x161C, 'W', 32, '32', 'gl', '7-11'),
    ('TRAIL_X_SUB', 'mmr', 0x1620, 'RW', 32, '32', 'gl', '7-12'),
    ('LEAD_BRETH_LNTH_SUB', 'mmr', 0x1624, 'RW', 32, '32', 'gl', '7-13'),
    ('DST_BRES_ERR', 'mmr', 0x1628, '', 0, '', 'gl', '7-7'),
    ('DST_BRES_INC', 'mmr', 0x162C, 'RW', 32, '32', 'gl', '7-8'),
    ('DST_BRES_DEC', 'mmr', 0x1630, 'RW', 32, '32', 'gl', '7-8'),
    ('DST_BRES_LNTH', 'mmr', 0x1634, '', 0, '', 'gl', '7-7'),
    ('DST_BRES_LNTH_SUB', 'mmr', 0x1638, 'RW', 32, '32', 'gl', '7-9'),
    ('SC_LEFT', 'mmr', 0x1640, 'RW', 32, '32', 'gl', '7-46'),
    ('SC_RIGHT', 'mmr', 0x1644, 'RW', 32, '32', 'gl', '7-46'),
    ('SC_TOP', 'mmr', 0x1648, 'RW', 32, '32', 'gl', '7-46'),
    ('SC_BOTTOM', 'mmr', 0x164C, 'RW', 32, '32', 'gl', '7-47'),
    ('SRC_SC_RIGHT', 'mmr', 0x1654, 'RW', 32, '32', 'gl', '7-16'),
    ('SRC_SC_BOTTOM', 'mmr', 0x165C, 'RW', 32, '32', 'gl', '7-16'),
    ('AUX_SC_CNTL', 'mmr', 0x1660, 'RW', 32, '32', 'gl', '7-47'),
    ('AUX1_SC_LEFT', 'mmr', 0x1664, 'RW', 32, '32', 'gl', '7-48'),
    ('AUX1_SC_RIGHT', 'mmr', 0x1668, 'RW', 32, '32', 'gl', '7-48'),
    ('AUX1_SC_TOP', 'mmr', 0x166C, 'RW', 32, '32', 'gl', '7-48'),
    ('AUX1_SC_BOTTOM', 'mmr', 0x1670, 'RW', 32, '32', 'gl', '7-48'),
    ('AUX2_SC_LEFT', 'mmr', 0x1674, 'RW', 32, '32', 'gl', '7-49'),
    ('AUX2_SC_RIGHT', 'mmr', 0x1678, 'RW', 32, '32', 'gl', '7-49'),
    ('AUX2_SC_TOP', 'mmr', 0x167C, 'RW', 32, '32', 'gl', '7-49'),
    ('AUX2_SC_BOTTOM', 'mmr', 0x1680, 'RW', 32, '32', 'gl', '7-49'),
    ('AUX3_SC_LEFT', 'mmr', 0x1684, 'RW', 32, '32', 'gl', '7-50'),
    ('AUX3_SC_RIGHT', 'mmr', 0x1688, 'RW', 32, '32', 'gl', '7-50'),
    ('AUX3_SC_TOP', 'mmr', 0x168C, 'RW', 32, '32', 'gl', '7-50'),
    ('AUX3_SC_BOTTOM', 'mmr', 0x1690, 'RW', 32, '32', 'gl', '7-50'),
    ('GUI_DEBUG0', 'mmr', 0x16A0, '', 0, '', 'gl', '7-59'),
    ('GUI_DEBUG1', 'mmr', 0x16A4, '', 0, '', 'gl', '7-59'),
    ('GUI_DEBUG2', 'mmr', 0x16A8, '', 0, '', 'gl', '7-59'),
    ('GUI_DEBUG3', 'mmr', 0x16AC, 'RW', 32, '32', 'gl', '7-60'),
    ('GUI_DEBUG4', 'mmr', 0x16B0, 'RW', 32, '32', 'gl', '7-60'),
    ('GUI_DEBUG5', 'mmr', 0x16B4, 'RW', 32, '32', 'gl', '7-60'),
    ('GUI_DEBUG6', 'mmr', 0x16B8, 'RW', 32, '32', 'gl', '7-60'),
    ('GUI_PROBE', 'mmr', 0x16BC, 'RW', 32, '32', 'gl', '7-61'),
    ('DP_CNTL', 'mmr', 0x16C0, 'RW', 32, '32', 'gl', '7-33'),
    ('DP_DATATYPE', 'mmr', 0x16C4, 'RW', 32, '32', 'gl', '7-35'),
    ('DP_MIX', 'mmr', 0x16C8, 'RW', 32, '32', 'gl', '7-37'),
    ('DP_WRITE_MSK', 'mmr', 0x16CC, 'RW', 32, '32', 'gl', '7-38'),
    ('DP_CNTL_XDIR_YDIR_YMAJOR', 'mmr', 0x16D0, 'RW', 32, '32', 'gl', '7-37'),
    ('DEFAULT_OFFSET', 'mmr', 0x16E0, 'RW', 32, '32', 'gl', '7-44'),
    ('DEFAULT_PITCH', 'mmr', 0x16E4, 'RW', 32, '32', 'gl', '7-44'),
    ('DEFAULT_SC_BOTTOM_RIGHT', 'mmr', 0x16E8, 'RW', 32, '32', 'gl', '7-45'),
    ('SC_TOP_LEFT', 'mmr', 0x16EC, 'W', 32, '32', 'gl', '7-51'),
    ('SC_BOTTOM_RIGHT', 'mmr', 0x16F0, 'W', 32, '32', 'gl', '7-51'),
    ('SRC_SC_BOTTOM_RIGHT', 'mmr', 0x16F4, 'W', 32, '32', 'gl', '7-16'),
    ('FLUSH_1', 'mmr', 0x1704, 'RW', 32, '32', 'gl', '7-62'),
    ('FLUSH_2', 'mmr', 0x1708, 'RW', 32, '32', 'gl', '7-62'),
    ('FLUSH_3', 'mmr', 0x170C, 'RW', 32, '32', 'gl', '7-63'),
    ('FLUSH_4', 'mmr', 0x1710, 'RW', 32, '32', 'gl', '7-63'),
    ('FLUSH_5', 'mmr', 0x1714, 'RW', 32, '32', 'gl', '7-63'),
    ('FLUSH_6', 'mmr', 0x1718, 'RW', 32, '32', 'gl', '7-63'),
    ('FLUSH_7', 'mmr', 0x171C, 'RW', 32, '32', 'gl', '7-63'),
    ('WAIT_UNTIL', 'mmr', 0x1720, 'RW', 32, '32', 'gl', '7-55'),
    ('GUI_STAT', 'mmr', 0x1740, 'RW', 32, '32', 'gl', '7-58'),
    ('PC_GUI_MODE', 'mmr', 0x1744, 'RW', 32, '32', 'gl', ''),
    ('PC_GUI_CTLSTAT', 'mmr', 0x1748, 'RW', 32, '32', 'gl', '3-34'),
    ('PC_DEBUG_MODE', 'mmr', 0x1760, 'RW', 32, '32', 'gl', '3-42'),
    ('HOST_DATA_LAST', 'mmr', 0x17E0, 'RW', 32, '32', 'gl', '7-17'),
    ('TEX_CNTL', 'mmr', 0x1800, '', 0, '', 'pro', ''),
    ('DESTINATION_3D_CLR_CMP_VAL', 'mmr', 0x1820, '', 0, '', 'gl', '8-9'),
    ('DESTINATION_3D_CLR_CMP_MSK', 'mmr', 0x1824, 'RW', 32, '32', 'gl', '8-10'),
    ('SCALE_OFFSET_0', 'mmr', 0x1998, '', 0, '', 'pro', ''),
    ('SCALE_PITCH', 'mmr', 0x199C, '', 0, '', 'driver', '', "the Pro index prints $1998, SCALE_OFFSET_0's offset"),
    ('SCALE_X_INC', 'mmr', 0x19A0, '', 0, '', 'pro', ''),
    ('SCALE_Y_INC', 'mmr', 0x19A4, '', 0, '', 'pro', ''),
    ('SCALE_HACC', 'mmr', 0x19A8, '', 0, '', 'pro', ''),
    ('SCALE_VACC', 'mmr', 0x19AC, '', 0, '', 'pro', ''),
    ('SCALE_DST_X_Y', 'mmr', 0x19B0, '', 0, '', 'pro', ''),
    ('SCALE_DST_HEIGHT_WIDTH', 'mmr', 0x19B4, '', 0, '', 'pro', ''),
    ('MC_SRC1_CNTL', 'mmr', 0x19D8, '', 0, '', 'pro', '3-261'),
    ('SCALE_3D_CNTL', 'mmr', 0x1A00, '', 0, '', 'pro', ''),
    ('COMPOSITE_SHADOW_ID', 'mmr', 0x1A0C, 'RW', 32, '32', 'gl', '7-10'),
    ('SCALE_3D_DATATYPE', 'mmr', 0x1A20, '', 0, '', 'pro', ''),
    ('CLR_CMP_CLR_3D', 'mmr', 0x1A24, 'RW', 32, '32', 'gl', '7-54'),
    ('CLR_CMP_MSK_3D', 'mmr', 0x1A28, 'RW', 32, '32', 'gl', '7-54'),
    ('SETUP_CNTL', 'mmr', 0x1BC4, '', 0, '', 'pro', ''),
    ('WINDOW_XY_OFFSET', 'mmr', 0x1BCC, '', 0, '', 'pro', '3-249'),
    ('DRAW_LINE_POINT', 'mmr', 0x1BD0, '', 0, '', 'pro', '3-249'),
    ('SETUP_CNTL_PM4', 'mmr', 0x1BD4, '', 0, '', 'pro', ''),
    ('DST_PITCH_OFFSET_C', 'mmr', 0x1C80, 'W', 32, '32', 'gl', '7-10'),
    ('DP_GUI_MASTER_CNTL_C', 'mmr', 0x1C84, 'RW', 32, '32', 'gl', ''),
    ('SC_TOP_LEFT_C', 'mmr', 0x1C88, 'W', 32, '32', 'gl', '7-52'),
    ('SC_BOTTOM_RIGHT_C', 'mmr', 0x1C8C, 'W', 32, '32', 'gl', '7-52'),
    ('Z_OFFSET_C', 'mmr', 0x1C90, 'RW', 32, '32', 'cce', ''),
    ('Z_PITCH_C', 'mmr', 0x1C94, 'RW', 32, '32', 'cce', ''),
    ('Z_STEN_CNTL_C', 'mmr', 0x1C98, 'RW', 32, '32', 'cce', ''),
    ('TEX_CNTL_C', 'mmr', 0x1C9C, 'RW', 32, '32', 'cce', ''),
    ('MISC_3D_STATE_CNTL_REG', 'mmr', 0x1CA0, 'RW', 32, '32', 'gl', '8-10'),
    ('TEXTURE_CLR_CMP_CLR_C', 'mmr', 0x1CA4, 'RW', 32, '32', 'cce', ''),
    ('TEXTURE_CLR_CMP_MSK_C', 'mmr', 0x1CA8, 'RW', 32, '32', 'cce', ''),
    ('FOG_COLOR_C', 'mmr', 0x1CAC, 'RW', 32, '32', 'cce', ''),
    ('PRIM_TEX_CNTL_C', 'mmr', 0x1CB0, 'RW', 32, '32', 'cce', ''),
    ('PRIM_TEXTURE_COMBINE_CNTL_C', 'mmr', 0x1CB4, 'RW', 32, '32', 'cce', ''),
    ('TEX_SIZE_PITCH_C', 'mmr', 0x1CB8, 'RW', 32, '32', 'cce', ''),
    ('PRIM_TEX_0_OFFSET_C', 'mmr', 0x1CBC, 'RW', 32, '32', 'cce', ''),
    ('PRIM_TEX_1_OFFSET_C', 'mmr', 0x1CC0, 'RW', 32, '32', 'cce', ''),
    ('PRIM_TEX_2_OFFSET_C', 'mmr', 0x1CC4, 'RW', 32, '32', 'cce', ''),
    ('PRIM_TEX_3_OFFSET_C', 'mmr', 0x1CC8, 'RW', 32, '32', 'cce', ''),
    ('PRIM_TEX_4_OFFSET_C', 'mmr', 0x1CCC, 'RW', 32, '32', 'cce', ''),
    ('PRIM_TEX_5_OFFSET_C', 'mmr', 0x1CD0, 'RW', 32, '32', 'cce', ''),
    ('PRIM_TEX_6_OFFSET_C', 'mmr', 0x1CD4, 'RW', 32, '32', 'cce', ''),
    ('PRIM_TEX_7_OFFSET_C', 'mmr', 0x1CD8, 'RW', 32, '32', 'cce', ''),
    ('PRIM_TEX_8_OFFSET_C', 'mmr', 0x1CDC, 'RW', 32, '32', 'cce', ''),
    ('PRIM_TEX_9_OFFSET_C', 'mmr', 0x1CE0, 'RW', 32, '32', 'cce', ''),
    ('PRIM_TEX_10_OFFSET_C', 'mmr', 0x1CE4, 'RW', 32, '32', 'cce', ''),
    ('SEC_TEX_CNTL_C', 'mmr', 0x1D00, 'RW', 32, '32', 'cce', ''),
    ('SEC_TEX_COMBINE_CNTL_C', 'mmr', 0x1D04, 'RW', 32, '32', 'cce', ''),
    ('SEC_TEX_0_OFFSET_C', 'mmr', 0x1D08, 'RW', 32, '32', 'cce', ''),
    ('SEC_TEX_1_OFFSET_C', 'mmr', 0x1D0C, 'RW', 32, '32', 'cce', ''),
    ('SEC_TEX_2_OFFSET_C', 'mmr', 0x1D10, 'RW', 32, '32', 'cce', ''),
    ('SEC_TEX_3_OFFSET_C', 'mmr', 0x1D14, 'RW', 32, '32', 'cce', ''),
    ('SEC_TEX_4_OFFSET_C', 'mmr', 0x1D18, 'RW', 32, '32', 'cce', ''),
    ('SEC_TEX_5_OFFSET_C', 'mmr', 0x1D1C, 'RW', 32, '32', 'cce', ''),
    ('SEC_TEX_6_OFFSET_C', 'mmr', 0x1D20, 'RW', 32, '32', 'cce', ''),
    ('SEC_TEX_7_OFFSET_C', 'mmr', 0x1D24, 'RW', 32, '32', 'cce', ''),
    ('SEC_TEX_8_OFFSET_C', 'mmr', 0x1D28, 'RW', 32, '32', 'cce', ''),
    ('SEC_TEX_9_OFFSET_C', 'mmr', 0x1D2C, 'RW', 32, '32', 'cce', ''),
    ('SEC_TEX_10_OFFSET_C', 'mmr', 0x1D30, 'RW', 32, '32', 'cce', ''),
    ('CONSTANT_COLOR_C', 'mmr', 0x1D34, 'RW', 32, '32', 'gl', '8-13'),
    ('PRIM_TEXTURE_BORDER_COLOR_C', 'mmr', 0x1D38, 'RW', 32, '32', 'cce', ''),
    ('SEC_TEXTURE_BORDER_COLOR_C', 'mmr', 0x1D3C, 'RW', 32, '32', 'cce', ''),
    ('STEN_REF_MSK_C', 'mmr', 0x1D40, 'RW', 32, '32', 'cce', ''),
    ('PLANE_3D_MASK_C', 'mmr', 0x1D44, 'RW', 32, '32', 'gl', '8-13'),
    # --- cfg ---
    ('VENDOR_ID', 'cfg', 0x0000, 'R', 16, '8/16/32', 'gl', '4-1'),
    ('DEVICE_ID', 'cfg', 0x0002, 'R', 16, '8/16/32', 'gl', '4-1'),
    ('COMMAND', 'cfg', 0x0004, 'RW', 16, '8/16/32', 'gl', '4-1'),
    ('STATUS', 'cfg', 0x0006, 'RW', 16, '8/16/32', 'gl', '4-2'),
    ('REVISION_ID', 'cfg', 0x0008, 'R', 8, '8/16/32', 'gl', '4-3'),
    ('REGPROG_ID', 'cfg', 0x0009, '', 0, '', 'gl', '4-4'),
    ('SUB_CLASS', 'cfg', 0x000A, '', 0, '', 'gl', '4-4'),
    ('BASE_CODE', 'cfg', 0x000B, '', 0, '', 'gl', '4-4'),
    ('CACHE_LINE', 'cfg', 0x000C, '', 0, '', 'gl', '4-4'),
    ('LATENCY', 'cfg', 0x000D, 'RW', 8, '8/16/32', 'gl', '4-5'),
    ('HEADER', 'cfg', 0x000E, 'R', 8, '8/16/32', 'gl', '4-5'),
    ('BIST', 'cfg', 0x000F, 'R', 8, '8/16/32', 'gl', '4-5'),
    ('MEM_BASE', 'cfg', 0x0010, 'RW', 32, '8/16/32', 'gl', '4-5'),
    ('IO_BASE', 'cfg', 0x0014, '', 0, '', 'gl', '4-6'),
    ('REG_BASE', 'cfg', 0x0018, '', 0, '', 'gl', '4-6'),
    ('ADAPTER_ID', 'cfg', 0x002C, '', 0, '', 'gl', '4-6'),
    ('BIOS_ROM', 'cfg', 0x0030, '', 0, '', 'gl', '4-6'),
    ('CAPABILITIES_PTR', 'cfg', 0x0034, 'R', 32, '8/16/32', 'gl', '4-7'),
    ('INTERRUPT_LINE', 'cfg', 0x003C, 'RW', 8, '8/16/32', 'gl', '4-7'),
    ('INTERRUPT_PIN', 'cfg', 0x003D, 'RW', 8, '8/16/32', 'gl', '4-7'),
    ('MIN_GRANT', 'cfg', 0x003E, 'R', 8, '8/16/32', 'gl', '4-7'),
    ('MAX_LATENCY', 'cfg', 0x003F, 'R', 8, '8/16/32', 'gl', '4-8'),
    ('ADAPTER_ID_W', 'cfg', 0x004C, 'W', 32, '8/16/32', 'gl', '4-8'),
    ('CAPABILITIES_ID', 'cfg', 0x0050, 'R', 32, '8/16/32', 'gl', '4-8'),
    ('AGP_STATUS', 'cfg', 0x0054, '', 0, '', 'gl', '4-22'),
    ('AGP_COMMAND', 'cfg', 0x0058, '', 0, '', 'gl', '4-22'),
    ('PMI_CAP_ID', 'cfg', 0x005C, '', 0, '', 'gl', '3-43'),
    ('PMI_REGISTER', 'cfg', 0x005C, '', 0, '', 'gl', '3-43'),
    ('PMI_NXT_CAP_PTR', 'cfg', 0x005D, 'R', 8, '8/16/32', 'gl', '3-44'),
    ('PMI_PMC_REG', 'cfg', 0x005E, 'R', 16, '8/16/32', 'gl', '3-44'),
    ('PMI_PMCSR_REG', 'cfg', 0x0060, '', 0, '', 'gl', '3-45'),
    ('PWR_MNGMT_CNTL_STATUS', 'cfg', 0x0060, 'RW', 32, '8/16/32', 'gl', '4-8'),
    ('PMI_DATA', 'cfg', 0x0063, 'R', 8, '8/16/32', 'gl', '3-45'),
    # --- pll ---
    ('CLK_PIN_CNTL', 'pll', 0x0001, 'RW', 32, '8/16/32', 'gl', '3-13'),
    ('PPLL_CNTL', 'pll', 0x0002, 'RW', 32, '8/16/32', 'gl', '3-14'),
    ('PPLL_REF_DIV', 'pll', 0x0003, 'RW', 32, '8/16/32', 'gl', '3-15'),
    ('PPLL_DIV_0', 'pll', 0x0004, '', 0, '', 'driver', ''),
    ('PPLL_DIV_1', 'pll', 0x0005, '', 0, '', 'driver', ''),
    ('PPLL_DIV_2', 'pll', 0x0006, '', 0, '', 'driver', ''),
    ('PPLL_DIV_3', 'pll', 0x0007, '', 0, '', 'driver', ''),
    ('VCLK_ECP_CNTL', 'pll', 0x0008, 'RW', 32, '8/16/32', 'gl', '3-18'),
    ('HTOTAL_CNTL', 'pll', 0x0009, 'RW', 32, '8/16/32', 'gl', '3-20'),
    ('X_MPLL_REF_FB_DIV', 'pll', 0x000A, 'RW', 32, '8/16/32', 'gl', '3-22'),
    ('XPLL_CNTL', 'pll', 0x000B, 'RW', 32, '8/16/32', 'gl', '3-22'),
    ('XDLL_CNTL', 'pll', 0x000C, '', 0, '', 'gl', '3-23'),
    ('XCLK_CNTL', 'pll', 0x000D, '', 0, '', 'gl', '3-25'),
    ('MPLL_CNTL', 'pll', 0x000E, '', 0, '', 'gl', '3-26'),
    ('MCLK_CNTL', 'pll', 0x000F, '', 0, '', 'gl', '3-27'),
    ('AGP_PLL_CNTL', 'pll', 0x0010, '', 0, '', 'gl', '3-28'),
    ('PLL_TEST_CNTL', 'pll', 0x0013, '', 0, '', 'gl', '3-29'),
    # --- crt ---
    ('CRT00', 'crt', 0x0000, '', 0, '', 'gl', '5-13'),
    ('CRT01', 'crt', 0x0001, '', 0, '', 'gl', '5-14'),
    ('CRT02', 'crt', 0x0002, '', 0, '', 'gl', '5-14'),
    ('CRT03', 'crt', 0x0003, '', 0, '', 'gl', '5-15'),
    ('CRT04', 'crt', 0x0004, '', 0, '', 'gl', '5-15'),
    ('CRT05', 'crt', 0x0005, '', 0, '', 'gl', '5-16'),
    ('CRT06', 'crt', 0x0006, '', 0, '', 'gl', '5-16'),
    ('CRT07', 'crt', 0x0007, '', 0, '', 'gl', '5-17'),
    ('CRT08', 'crt', 0x0008, '', 0, '', 'gl', '5-18'),
    ('CRT09', 'crt', 0x0009, '', 0, '', 'gl', '5-18'),
    ('CRT0A', 'crt', 0x000A, '', 0, '', 'gl', '5-19'),
    ('CRT0B', 'crt', 0x000B, '', 0, '', 'gl', '5-20'),
    ('CRT0C', 'crt', 0x000C, '', 0, '', 'gl', '5-20'),
    ('CRT0D', 'crt', 0x000D, '', 0, '', 'gl', '5-21'),
    ('CRT0E', 'crt', 0x000E, '', 0, '', 'gl', '5-21'),
    ('CRT0F', 'crt', 0x000F, '', 0, '', 'gl', '5-22'),
    ('CRT10', 'crt', 0x0010, '', 0, '', 'gl', '5-22'),
    ('CRT11', 'crt', 0x0011, '', 0, '', 'gl', '5-23'),
    ('CRT12', 'crt', 0x0012, '', 0, '', 'gl', '5-23'),
    ('CRT13', 'crt', 0x0013, '', 0, '', 'gl', '5-24'),
    ('CRT14', 'crt', 0x0014, '', 0, '', 'gl', '5-24'),
    ('CRT15', 'crt', 0x0015, '', 0, '', 'gl', '5-25'),
    ('CRT16', 'crt', 0x0016, '', 0, '', 'gl', '5-25'),
    ('CRT17', 'crt', 0x0017, 'RW', 8, '8/16/32', 'gl', '5-26'),
    ('CRT18', 'crt', 0x0018, '', 0, '', 'gl', '5-27'),
    ('CRT1E', 'crt', 0x001E, '', 0, '', 'gl', '5-27'),
    ('CRT1F', 'crt', 0x001F, '', 0, '', 'gl', '5-28'),
    ('CRT22', 'crt', 0x0022, '', 0, '', 'gl', '5-28'),
    ('CRT00_S', 'crt', 0x0040, '', 0, '', 'gl', '5-28'),
    ('CRT01_S', 'crt', 0x0041, '', 0, '', 'gl', '5-29'),
    ('CRT02_S', 'crt', 0x0042, '', 0, '', 'gl', '5-29'),
    ('CRT04_S', 'crt', 0x0044, '', 0, '', 'gl', '5-29'),
    ('CRT05_S', 'crt', 0x0045, '', 0, '', 'gl', '5-30'),
    ('CRT06_S', 'crt', 0x0046, '', 0, '', 'gl', '5-30'),
    ('CRT07_S', 'crt', 0x0047, '', 0, '', 'gl', '5-30'),
    ('CRT08_S', 'crt', 0x0048, '', 0, '', 'gl', '5-31'),
    ('CRT09_S', 'crt', 0x0049, '', 0, '', 'gl', '5-31'),
    ('CRT0A_S', 'crt', 0x004A, '', 0, '', 'gl', '5-31'),
    ('CRT0B_S', 'crt', 0x004B, '', 0, '', 'gl', '5-32'),
    ('CRT0C_S', 'crt', 0x004C, '', 0, '', 'gl', '5-32'),
    ('CRT0D_S', 'crt', 0x004D, '', 0, '', 'gl', '5-32'),
    ('CRT0E_S', 'crt', 0x004E, '', 0, '', 'gl', '5-32'),
    ('CRT0F_S', 'crt', 0x004F, '', 0, '', 'gl', '5-32'),
    ('CRT10_S', 'crt', 0x0050, '', 0, '', 'gl', '5-33'),
    ('CRT11_S', 'crt', 0x0051, '', 0, '', 'gl', '5-33'),
    ('CRT12_S', 'crt', 0x0052, '', 0, '', 'gl', '5-33'),
    ('CRT13_S', 'crt', 0x0053, '', 0, '', 'gl', '5-33'),
    ('CRT14_S', 'crt', 0x0054, '', 0, '', 'gl', '5-34'),
    ('CRT15_S', 'crt', 0x0055, '', 0, '', 'gl', '5-34'),
    ('CRT16_S', 'crt', 0x0056, '', 0, '', 'gl', '5-34'),
    ('CRT17_S', 'crt', 0x0057, '', 0, '', 'gl', '5-34'),
    ('CRT18_S', 'crt', 0x0058, '', 0, '', 'gl', '5-35'),
    ('CRT1E_S', 'crt', 0x005E, '', 0, '', 'gl', '5-35'),
    ('CRT1F_S', 'crt', 0x005F, '', 0, '', 'gl', '5-35'),
    # --- attr ---
    ('ATTR10', 'attr', 0x0010, '', 0, '', 'gl', '5-44'),
    ('ATTR11', 'attr', 0x0011, '', 0, '', 'gl', '5-45'),
    ('ATTR12', 'attr', 0x0012, '', 0, '', 'gl', '5-45'),
    ('ATTR13', 'attr', 0x0013, '', 0, '', 'gl', '5-46'),
    ('ATTR14', 'attr', 0x0014, '', 0, '', 'gl', '5-47'),
    # --- seq ---
    ('SEQ04', 'seq', 0x0004, 'RW', 8, '8/16/32', 'gl', ''),
    # --- gra ---
    ('GRA05', 'gra', 0x0005, 'RW', 8, '8/16/32', 'gl', ''),
    # --- vga_io ---
    ('CRTC_DEBUG', 'vga_io', 0x021C, '', 0, '', 'gl', '5-36'),
    ('GENMO_WT', 'vga_io', 0x03C2, 'W', 8, '8/16/32', 'gl', ''),
    ('GENMO_RD', 'vga_io', 0x03CC, 'R', 8, '8/16/32', 'gl', ''),
)

REGISTERS = tuple(Register(*(r + ("",) * (9 - len(r)))) for r in _ROWS)

BY_NAME = {r.name: r for r in REGISTERS}

# The config-space mirror in the register aperture (GL guide Table 2-1).
CFG_MIRROR_BASE = 0x0F00

# The register-aperture map (GL guide Table 2-1): which block an MMR offset
# falls in, which also says what access widths it takes (Table 2-2).
APERTURE_BLOCKS = (
    (0x0000, 0x00FF, "non-GUI, also I/O-mapped"),
    (0x0100, 0x0EFF, "non-GUI, memory-mapped only"),
    (0x0F00, 0x0FFF, "PCI configuration space (read-only copy)"),
    (0x1000, 0x13FF, "CCE (dword access only)"),
    (0x1400, 0x1FFF, "GUI 2D/3D engine (dword access only)"),
)


def mmr_offset(reg):
    """The register-aperture offset `reg` answers at, or None (PLL, VGA)."""
    if reg.space == "mmr":
        return reg.offset
    if reg.space == "cfg":
        return CFG_MIRROR_BASE + reg.offset
    return None


def at_mmr(offset):
    """Every register answering at register-aperture offset `offset`."""
    return [r for r in REGISTERS if mmr_offset(r) == offset]


def block(offset):
    """The aperture block an MMR offset falls in."""
    for lo, hi, what in APERTURE_BLOCKS:
        if lo <= offset <= hi:
            return what
    return "outside the 8 KB register aperture"


def describe(r):
    where = f"{r.space} ${r.offset:04X}"
    mmr = mmr_offset(r)
    if r.space == "cfg":
        where += f" (MMR ${mmr:04X})"
    bits = f"[{r.rw}] {r.bits}-bit, access {r.access}" if r.rw else "[index only]"
    page = f"p. {r.page}" if r.page else ""
    note = f" -- {r.note}" if r.note else ""
    return f"{r.name:32s} {where:22s} {bits:28s} {r.src:6s} {page}{note}"


# The Xclaim VR 128 FCode's own register-offset constants (113-57406-108),
# decoded: offset -> the register a decode of the FCode's accessors says it
# is.  If a transcription error shifts any of these, the table is wrong.
FCODE_ANCHORS = {
    0x0008: "CLOCK_CNTL_INDEX",
    0x000C: "CLOCK_CNTL_DATA",
    0x0040: "GEN_INT_CNTL",
    0x0044: "GEN_INT_STATUS",
    0x0058: "DAC_CNTL",
    0x0068: "GPIO_MONID",
    0x006C: "GPIO_MONIDB",
    0x00E0: "CONFIG_CNTL",
    0x00F8: "CONFIG_MEMSIZE",
    0x0100: "CONFIG_APER_0_BASE",
    0x010C: "CONFIG_REG_1_BASE",
    0x0110: "CONFIG_REG_APER_SIZE",
}

# The CCE contract (SDK chapter 5) and the 3D context the r128 DRM uploads.
CCE_ANCHORS = {
    0x0700: "PM4_BUFFER_OFFSET",
    0x0704: "PM4_BUFFER_CNTL",
    0x070C: "PM4_BUFFER_DL_RPTR_ADDR",
    0x0710: "PM4_BUFFER_DL_RPTR",
    0x0714: "PM4_BUFFER_DL_WPTR",
    0x071C: "PM4_VC_FPU_SETUP",
    0x07B8: "PM4_STAT",
    0x07D4: "PM4_MICROCODE_ADDR",
    0x07DC: "PM4_MICROCODE_DATAH",
    0x07E0: "PM4_MICROCODE_DATAL",
    0x07FC: "PM4_MICRO_CNTL",
    0x1000: "PM4_FIFO_DATA_EVEN",
    0x1004: "PM4_FIFO_DATA_ODD",
    0x146C: "DP_GUI_MASTER_CNTL",
    0x1740: "GUI_STAT",
    0x1A00: "SCALE_3D_CNTL",
    0x1BC4: "SETUP_CNTL",
    0x1C80: "DST_PITCH_OFFSET_C",
    0x1C98: "Z_STEN_CNTL_C",
    0x1CB0: "PRIM_TEX_CNTL_C",
    0x1D00: "SEC_TEX_CNTL_C",
    0x1D44: "PLANE_3D_MASK_C",
}


def selftest():
    names = [r.name for r in REGISTERS]
    dup = {n for n in names if names.count(n) > 1}
    assert not dup, f"duplicate names: {sorted(dup)}"
    for r in REGISTERS:
        assert r.space in ("mmr", "cfg", "pll", "crt", "attr", "seq", "gra", "vga_io"), r
        assert r.src in ("gl", "cce", "pro", "driver"), r
        if r.space == "mmr":
            assert r.offset < 0x2000, f"{r.name} ${r.offset:X} is outside the 8 KB aperture"
        if r.space == "cfg":
            assert r.offset < 0x100, f"{r.name} config ${r.offset:X} is outside the header"
        if r.space == "pll":
            assert r.offset < 0x40, f"{r.name} PLL index ${r.offset:X}"
    for anchors in (FCODE_ANCHORS, CCE_ANCHORS):
        for off, want in anchors.items():
            got = [r.name for r in at_mmr(off)]
            assert want in got, f"${off:04X} decodes to {got}, not {want}"
    # The PLL registers the mode set programs, by index (aty128fb).
    for name, idx in (("PPLL_CNTL", 2), ("PPLL_REF_DIV", 3), ("PPLL_DIV_3", 7), ("HTOTAL_CNTL", 9),
                      ("X_MPLL_REF_FB_DIV", 0xA), ("MCLK_CNTL", 0xF)):
        r = BY_NAME[name]
        assert r.space == "pll" and r.offset == idx, r
    # The capability pointer the FCode reads first, through the mirror.
    assert mmr_offset(BY_NAME["CAPABILITIES_PTR"]) == 0x0F34
    print(f"rage128_regs: {len(REGISTERS)} registers, self-test OK")


def main(argv):
    ap = argparse.ArgumentParser(description="ATI Rage 128 GL register cross-reference")
    ap.add_argument("query", nargs="*", help="register name or MMR offset (hex, $ or 0x prefix optional)")
    ap.add_argument("--list", nargs="?", const="", metavar="SPACE", help="list the table (optionally one space)")
    ap.add_argument("--c-defines", nargs="?", const="mmr", metavar="SPACE", help="emit C #defines for one space")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args(argv)
    if a.selftest:
        selftest()
        return 0
    if a.list is not None:
        for r in REGISTERS:
            if not a.list or r.space == a.list:
                print(describe(r))
        return 0
    if a.c_defines:
        for r in REGISTERS:
            if r.space == a.c_defines:
                print(f"#define R128_{r.name:32s} 0x{r.offset:04X}u")
        return 0
    rc = 0
    for q in a.query:
        r = BY_NAME.get(q.upper())
        if r:
            print(describe(r))
            continue
        try:
            off = int(q.lstrip("$"), 16)
        except ValueError:
            print(f"{q}: no such register", file=sys.stderr)
            rc = 1
            continue
        hits = at_mmr(off)
        print(f"${off:04X}: {block(off)}")
        for r in hits:
            print("  " + describe(r))
        if not hits:
            print("  (no register defined here)")
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
