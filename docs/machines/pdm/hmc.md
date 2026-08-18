# HMC — the PDM memory controller

`src/machines/pdm/hmc.c`.  Sources: Apple, *Power Macintosh Computers*
Developer Note (1994), the 8100 schematic set, and the shipping ROM's
hardware-init sequence (the behavioral oracle for every claim below).

## The serial configuration register

One software-visible register, 35 bits, accessed bit-serially through the
AMIC-decoded window at `$50F40000`:

- any byte write to `$50F40008` resets the bit pointer;
- each byte write to `$50F40000` shifts in bit 0 of the value, LSB first;
- each byte read shifts out one bit the same way.

Commit is per-bit (nothing in the ROM distinguishes immediate vs
latch-on-35th, and its checksum-failure path does a read-modify-write of
the whole register).  Bits 0–1 read back the L2 cache-SIMM size-sense
pins: no L2 is modeled, so they read 0 ("no cache SIMM") — fully
self-consistent with every ROM path (POST then skips the L2 test with the
`Lisa` marker).  Bits this model *honors* beyond store-and-readback:

| Bits | Function |
|---|---|
| 2–15 | DRAM timing field — all-zero (the power-on state and the ROM's `$00090000` test pattern) = slow: physical page 0 charges the board's wait-state penalty per access.  The ROM's bus-ratio measurement times a 1024×4 `lwz` loop once in this state and once with a timing bit set (`$00090100`); the delta over its add-loop timebase is the CPU:bus ratio.  The subtract order in the ROM's own arithmetic proves the zero-field run is the slow one; every production timing value leaves the field non-zero. |
| 29–30 | SIMM_BANK_SIZE (6100 only): 0 = reset map, 1/2/3 = 2/8/32 MB banks — writing it **moves the SIMM banks** (below) |
| 31 | motherboard bank is 4 MB (halves its alias wrap) |

## RAM banks

8 MB soldered ("motherboard") plus SIMM banks carved from the configured
RAM size (banks of 32/8/2 MB, largest first).  Decode rules the ROM's
sizing probe depends on:

- undersized banks **alias (wrap)** through their decode window — the
  probe finds the aliases and sizes through them;
- empty windows must **not** echo the last write — unmapped reads return
  0, so the probe's signature compare fails;
- **6100** (`PDM_BANKS_MOVABLE`): with code 0, bank 1 decodes at
  `$10000000` and bank 2 at `$08000000` (128 MB windows each); writing a
  size code relocates them contiguously after motherboard RAM, each with
  a bank-size window.  The ROM writes the code and immediately stores its
  bank table through the new map;
- **7100/8100** (`PDM_BANKS_FIXED`): banks sit at
  `$01000000 + n × $04000000` (32 MB usable per window), never moving.

## Machine ID (`$5FFFFFFC`)

Byte reads deliver `$A55A30xx` (`$3010` 6100 — the `$3011` "PDM"
ProductInfo value is a 68k software promotion, never in hardware;
`$3012` 7100; `$3013` 8100).  A 32-bit read returns only the low half —
the ROM's long-probe must fail to find the `$A55A` signature before it
falls back to byte reads.  Writes are ignored.  The register must work
from reset (HWInit reads it before any config write).
