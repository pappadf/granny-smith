# ATI 3D RAGE PRO

**Contents:**

1. [Overview](#1-overview)
2. [Register file](#2-register-file) — what this page adds to the Rage II page's map: GUI_STAT's FIFO
   count, the DP_SET_GUI_ENGINE2 shortcut, HOST_CNTL, the BAR2 register aperture
3. [Behaviour](#3-behaviour) — colour host data
4. [Programming model](#4-programming-model) — what ATI's Mac OS accelerator does with it
5. [Quirks & errata](#5-quirks--errata)
6. [Open questions](#6-open-questions)

---

## 1. Overview

The 3D RAGE PRO is the mach64GT generation's last member: the mach64 2D core
([mach64.md](mach64.md)) with the GT deltas the Rage II page documents
([rage-ii.md](rage-ii.md) §2.9, §2.10), a larger command FIFO, a bus-master GUI engine and a set of
write-only set-up shortcuts [1] Ch. 5, 6. On the Macintosh it is the on-board graphics
of the enhanced beige Power Macintosh G3 boards — PCI device `$12`, `AAPL,slot-name "F1"`,
`$1002:4750` ([g3.md](../../../machines/g3/g3.md) §4.6, §6.5) — and the controller of several
third-party cards.

This page is not yet the whole part. It records what is established from the manuals and from
the Mac OS driver's observed use (§4); everything else about the chip is the Rage II page's,
by the shared generation, until shown otherwise (§6).

## 2. Register file

### 2.1 GUI_STAT (block 0, dword `$CE`)

| Bits | Field | Meaning |
|---|---|---|
| 0 | GUI_ACTIVE | the engine is busy, or the 3D engine is, or the command FIFO is not empty, or a context is loading |
| 25:16 | FIFO_CNT | the number of available (empty) command-FIFO entries — "less than or equal to 32" |

[2] p. 4-106. FIFO_STAT keeps its one-bit-per-full-entry meaning for the top entries only; "the
number of free entries in an encoded form is available in the GUI_STAT register" [2] p. 4-101.
An idle engine with an empty FIFO reads FIFO_CNT = 32.

### 2.2 DP_SET_GUI_ENGINE2 (block 0, dword `$BE`, write-only)

One write loads the data path, the trajectory directions and the pitches of a common 2D
operation [1] pp. 5-56, 5-57:

| Bits | Field | Loads |
|---|---|---|
| 3:0 | DP_BKGD_MIX | DP_MIX background mix |
| 7:4 | DP_FRGD_MIX | DP_MIX foreground mix |
| 10:8 | DP_BKGD_SRC | DP_SRC background source |
| 13:11 | DP_FRGD_SRC | DP_SRC foreground source |
| 15:14 | DP_MONO_SRC | DP_SRC monochrome source |
| 16 | DST_X_DIR | as in GUI_TRAJ_CNTL |
| 17 | DST_Y_DIR | as in GUI_TRAJ_CNTL |
| 18 | PAT_MONO_EN | as in GUI_TRAJ_CNTL |
| 19 | SRC_PATT_ROT_EN | as in GUI_TRAJ_CNTL |
| 20 | FAST_FILL_EN | as in SRC_CNTL |
| 21 | BLOCK_WRITE_EN | as in SRC_CNTL |
| 22 | SET_DP_WRITE_MASK | 1 = set DP_WRITE_MSK to `$FFFFFFFF`; 0 = leave it |
| 25:23 | DST_PIX_WIDTH | DP_PIX_WIDTH's destination width |
| 26 | SET_SRC_PIX_WIDTH | 0 = source width mono; 1 = the same as the destination's |
| 30:27 | SET_DST_PITCH | DST_OFF_PITCH's pitch, from the table below |
| 31 | SRC_OFFPITCH_COPY | 0 = SRC_OFF_PITCH set to 0; 1 = set to DST_OFF_PITCH |

The pitch codes, in pixels, are DP_SET_GUI_ENGINE's [1] p. 5-53: 0 = USR_DST_PITCH (dword `$BC`,
[1] p. 5-52), 1 = 320, 2 = 352, 3 = 384, 4 = 640, 5 = 800, 6 = 896, 7 = 512, 8 = 1024, 9 = 1152,
10 = 1280, 11 = 400, 12 = 832, 13 = 1600, 14 = 448, 15 = 2048.

The write also presets: DST_Y_X = 0, DST_HEIGHT_WIDTH = 0, SRC_Y_X = 0, DP_PIX_WIDTH's
DP_HOST_PIX_WIDTH and DP_BYTE_PIX_ORDER = 0, CLR_CMP_CNTL = 0 [1] Table 5-13. The mixes are four
bits here against DP_MIX's five; the values used are the ordinary mix codes (7 = source).

A worked value: `$A5C30877` — SRC_OFFPITCH_COPY, pitch code 4 (640), source width as
destination, destination 15 bpp, write mask set, X and Y increasing, foreground source 1
(DP_FRGD_CLR), background source 0, both mixes 7 — a solid fill in 15 bpp on a 640-pixel
surface.

### 2.3 HOST_CNTL (block 0, dword `$90`)

| Bit | Field | Meaning |
|---|---|---|
| 0 | HOST_BYTE_ALIGN | 1 bpp host data: on a Y step, consume to the next byte boundary |
| 1 | HOST_BIG_ENDIAN_EN | byte-swap colour host data: within each 16-bit word at 15/16 bpp, the whole dword at 32 bpp |

[1] p. 5-34; the same two bits in the GX's register [4] (and [mach64.md](mach64.md) §3.1).

### 2.4 The register aperture (BAR2)

BAR2 is a 4 KB memory BAR holding both register blocks: block 1 at `+$000` and block 0 at
`+$400`. It is how drivers reach the registers when BUS_APER_REG_DIS has removed them from BAR0's
aperture. On the beige G3 the firmware places it at `$80800000`, with BAR0 at `$81000000` [5].

The two blocks repeat in the BAR's upper half, `+$800` to `+$FFF`, and that copy is the same
little-endian register face. ATI's driver depends on it (§4). It writes block 0's HOST_DATA at
`+$E00`, with HOST_BIG_ENDIAN_EN set and the pixels in big-endian order. Pixels come out in order
only if no second byte swap happens in the aperture [5].

## 3. Behaviour

### 3.1 Colour host data

With a colour source set to host (DP_SRC source 2) and a non-zero DP_HOST_PIX_WIDTH, HOST_DATA
carries packed pixels of that width, the first pixel in the low-order bytes of the dword;
HOST_BIG_ENDIAN_EN lets a big-endian host store its own pixels unconverted [1] pp. 5-33, 5-34.
HOST_DATA is "a single register mapped to 16 consecutive addresses"; writing any other draw
engine register while host data is expected makes the operation "panic and complete ... with a
garbage color", and HOST_DATA written when no host data is expected is discarded [1] p. 5-33.

## 4. Programming model

ATI's Mac OS accelerator for the part (the "ATI Graphics Accelerator" extension Mac OS 9.2.1
installs) is observed to [5]:

- wait for FIFO space by reading GUI_STAT's FIFO_CNT before each operation — not FIFO_STAT —
  so a part that reports no free entries stalls every drawing call;
- program every fill and blit through DP_SET_GUI_ENGINE2 and the colour registers, never writing
  DP_SRC or DP_MIX for them;
- draw offscreen copies and dialog contents as colour host-data operations at 15 bpp with
  HOST_BIG_ENDIAN_EN set;
- stream that host data with `stmw` (eight words a store), each store followed by `dcbst`. The
  stores go not to the BAR0 register alias but to the register aperture BAR2, at `+$E00`: block
  0's HOST_DATA through the upper copy of the two register blocks (§2.4).

## 5. Quirks & errata

- **FIFO_CNT is the wait the driver uses.** A model that reports an idle engine through FIFO_STAT
  alone hangs ATI's driver at its first operation.
- **HOST_DATA through BAR2's upper half.** A model that decodes only BAR2's lower 2 KB drops
  every pixel ATI's driver streams. The symptom is alerts, buttons and menu-bar text drawn as
  blank boxes.
- **The shortcut registers are write-only and stateful.** Their effects persist in the
  registers they load; DP_SRC read back after a DP_SET_GUI_ENGINE2 write shows the loaded
  sources.

## 6. Open questions

1. **DP_SET_GUI_ENGINE's bit positions.** Its fields are named and described [1] pp. 5-53, 5-54
   (destination and source widths, destination offset and pitch codes, the DRAWING_COMBO table,
   the bus-master controls), but their positions have not been read off the chart.
2. **The upper half of BAR2.** It is observed only through the driver's use (§2.4): a plain
   mirror of the lower 2 KB is the reading the driver's byte order requires. What the manuals say
   about it has not been checked.
3. **The PCI revision on the G3.** The beige G3 tree's published `revision-id` and the chip ID's
   revision byte have not been reconciled across board revisions.
4. **Everything this page defers to the Rage II page** — the scaler, overlay, 3D and bus-master
   registers — is unverified for the PRO beyond the manuals' statement that the generation
   shares them [3].

## References

1. ATI Technologies Inc., *3D RAGE LT PRO Register Reference Guide*, P/N RRG-G03300 Rev. 2.01,
   April 1998 — Ch. 5 "Draw Engine Control Registers": HOST_DATA p. 5-33, HOST_CNTL p. 5-34,
   USR_DST_PITCH p. 5-52, DP_SET_GUI_ENGINE pp. 5-53–5-55 (Tables 5-11, 5-12), DP_SET_GUI_ENGINE2
   pp. 5-56–5-57 (Table 5-13); Ch. 6 "Bus Mastering". The LT PRO's draw engine is the RAGE PRO's,
   its DP_SET_GUI_ENGINE noted as "modified (from the RAGE PRO version)" for the secondary
   display only (p. 5-52).
2. ATI Technologies Inc., *mach64 Register Reference Guide: ATI-264VT and 3D RAGE*, P/N
   RRG-G02700 Rev. 0.10, July 1996 — FIFO_STAT p. 4-101, GUI_STAT p. 4-106.
3. ATI Technologies Inc., *RAGE PRO and Derivatives Programmer's Guide*, P/N PRG-215R3-00-10
   Rev. 1.0, March 2000.
4. ATI Technologies Inc., *mach64 Register Reference Guide* (GX), 1994 — HOST_CNTL.
5. Mac OS 9.2.1 on a beige Power Macintosh G3 (Rev C ROM, `$78F57389`) with its installed ATI
   extensions. Observed: register traffic to the on-board RAGE PRO at the desktop and while an
   alert is drawn; the accelerator's code at the host-data path (its `stmw`/`dcbst` loop and the
   BAR2 base it writes through); and the device's configuration space, read through Grackle.
