# MEMC family — Centris 610/650, Quadra 610/650/800, LC 475/575, Quadra 605

`src/machines/memc/memc.c` implements the five MEMC machines and the
three MEMCjr machines as one substrate (`memc_substrate`) with a per-model
board descriptor (`memc_board_desc_t`, reached through
`hw_profile_t.board`); `src/machines/memc/wombat.c` holds the MEMC
descriptors and profiles, `src/machines/memc/memcjr.c` the MEMCjr ones,
and `src/machines/memc/iosb.c` the IOSB's register file (which the MEMCjr
boards' PrimeTime shares). Hardware reference:
[memc.md](../../../reference/machines/memc/memc.md) (family) and its
machine pages ([c610.md](../../../reference/machines/memc/c610.md),
[c650.md](../../../reference/machines/memc/c650.md),
[q610.md](../../../reference/machines/memc/q610.md),
[q650.md](../../../reference/machines/memc/q650.md),
[q800.md](../../../reference/machines/memc/q800.md),
[lc475.md](../../../reference/machines/memc/lc475.md),
[lc575.md](../../../reference/machines/memc/lc575.md),
[q605.md](../../../reference/machines/memc/q605.md)).

"MEMC" and "MEMCjr" are the chip names Apple's developer notes use. The
MEMC is also known as "djMEMC".

## 1. Responsibilities & design

The five models are one board generation, the "Wombat" (three NuBus sockets)
and the "WLCD" (one PDS adapter slot) logic boards. What varies is data:

| Model | id | board | clock | CPU | soldered RAM | SIMM sockets | slots | BoxFlag |
|---|---|---|---|---|---|---|---|---|
| Centris 610 | `c610` | WLCD | 20 MHz | 68LC040 (no FPU) | 4 MB | 2 | E (adapter) | 46 |
| Quadra 610 | `q610` | WLCD | 25 MHz | 68040 | 4 MB | 2 | E (adapter) | 47 |
| Centris 650 | `c650` | Wombat | 25 MHz | 68040 | 8 MB | 4 | C, D, E | 24 |
| Quadra 650 | `q650` | Wombat | 33 MHz | 68040 | 8 MB | 4 | C, D, E | 30 |
| Quadra 800 | `q800` | Wombat | 33 MHz | 68040 | 8 MB | 4 | C, D, E | 29 |

The ROM tells the models apart by the CPU-ID register ($5FFFFFFC, $A55A2BAD
on every member) and the VIA1 port-A straps PA6/PA4/PA2/PA1
(`memc_board_desc_t.via1_id`). Both ROMs (`F1A6F343`, `F1ACAD13`) boot all
five models; the BoxFlag column is what each derives.

The substrate reuses the Quadra 700's pieces where the hardware is
software-compatible, through the MCU family's shared 040 Quadra block
(`mcu/quadra040.c`, [../mcu/mcu.md](../mcu/mcu.md) "Shared 040 Quadra
block"): `memc_state_t` opens with a `quadra040_t`, and the init prefix
and suffix, the device builders, island bind and mappings, `/RESET`,
teardown, IRQ wiring, the SONIC / MAC PROM / Kiwi handler rows and the
monitor table are that file's. The DAFB model (`mcu/dafb.c`) serves the
built-in video, with `quadra040_video_t.ac842a` and `.antelope` making it
the Wombat DAFB plus the Antelope CLUT/DAC (PCBR1's mfg/rev nibble reads
non-zero); the Kiwi is a `yancc_t` (`mcu/yancc.c`) logging as "Kiwi". The
shared `board_io` engine runs the I/O island at $50000000
(`memc_io_ranges`); the rows both islands decode are one
`MEMC_ISLAND_ROWS` list. The CPU-ID page is the shared
`runtime/cpuid_page.c`. The DAFB startup-mode table the MCU boards carry
is not adopted: no MEMC startup records are measured.

The Centris 610's FPU is absent through `hw_profile_t.fpu_absent`, which
`m68k_board_build_core` passes to `cpu_init_fpu`: every FPU instruction then
takes the F-line trap and the System's software floating point runs.

**The MEMCjr boards.** `memc_board_desc_t.chip` selects the memory
controller (`MEMC_CHIP_MEMC`, the default, or `MEMC_CHIP_MEMCJR`),
whose differences are one `memc_chip_ops_t` per chip in `memc.c`
(register count, bank count, RAM-bank decomposition, bank start, power-up
registers, read side effects, the bank-landed latch hook). The board
features that came with it are descriptor fields of their own: `has_sonic`,
`adb` (`MEMC_ADB_VIA` or `MEMC_ADB_CUDA`) and `video_latch`.
`MEMC_DESC_COMMON` / `MEMCJR_DESC_COMMON` set them, with the chip and the
VRAM size, for every board of each kind (`WLCD_BOARD`, `WOMBAT_BOARD`,
`MEMCJR_BOARD`). `.cpuid` is the CPU-ID register's value (0 is the
MEMC's $A55A2BAD):

| Model | id | board | clock | CPU | CPU-ID | BoxFlag |
|---|---|---|---|---|---|---|
| LC 475 | `lc475` | Primus | 25 MHz | 68LC040 | $A55A2221 | 83 |
| Quadra 605 | `q605` | Aladdin | 25 MHz | 68LC040 | $A55A2225 | 88 |
| LC 575 | `lc575` | Optimus | 33 MHz | 68LC040 | $A55A222E | 86 |

The ROM (`FF7439EE`) finds them by the CPU-ID alone (bit 11 clear: no VIA
supplement); its low two bits name the clock, from which the ROM picks the
MEMCjr and PrimeTime timing. A MEMCjr board differs from a MEMC one in:

- RAM: 4 MB soldered (bank 0) and one SIMM socket whose sides are banks 1
  and 2 (`memcjr_chip_ram_banks`; 1/4/16 MB single-sided, 2/8/32 MB
  double-sided), 4-36 MB.
- The register interface: the MEMCjr's registers are six bits wide on the
  data bus, the bits above going through the latch at $7C. Software writes
  the latch (bits 6-11) just before a register, and after reading one reads
  its bits 6-11 back from the latch; a read here loads the latch
  (`memcjr_latch_load`). The frame-buffer controller's registers at
  $F9800000 are read the same way on a `video_latch` board (`memcjr_video_*`
  front the DAFB model);
  without it the video driver's save-and-restore of the horizontal timing
  around its line-compare probe writes back garbage. The register window
  decodes 32 longwords instead of the MEMC's 16.
- The bank decode (`memcjr_chip_bank_start`): five bank registers in 1 MB units
  with a 10-bit start: the low six bits in the register, the high four from
  the latch as the register lands (recorded per bank, `jr_bank_hi`).
- ADB, PRAM and the clock on a Cuda (VIA1 SR and PB3-PB5, as on the AV
  models) instead of the VIA transceiver and the RTC chip.
- No SONIC, MAC PROM or Kiwi (`memcjr_io_ranges`); the LC PDS and the
  LC 575's communication slot are not modelled (`nubus_slots` NULL). The
  communication slot's window ($0A000) bus-errors, which is how the ROM's
  probe for the slot's Ethernet card finds it empty: with a floating bus it
  would wait forever on the card's command register. The row is a plain
  `.berr` row: the `board_io` engine raises the fault at the full bus
  address itself.
- The LC 575's display is built in (`lc575_builtin_video`, the 14" colour
  CRT with the 640x480 RGB sense code); the LC 475 and Quadra 605 take the
  same external monitors as the MEMC boards.

## 2. Key types & files

| Type / function | File | Purpose |
|---|---|---|
| `memc_board_desc_t` | `memc.h` | per-model data: chip, board features, identity straps, soldered RAM, sockets, VRAM |
| `memc_state_t` | `memc.h` | the shared `quadra040_t` first, then the register files, IOSB, Cuda, CPU-ID page |
| `memc_chip_ops_t` | `memc.c` | what the MEMC and the MEMCjr do differently |
| `memc_ram_banks` | `memc.c` | decomposes a RAM total into the chip's banks |
| `memc_map_ram` | `memc.c` | applies the bank registers to the page table (`m68k_board_map_banks`) |
| `memc_io_ranges` | `memc.c` | the I/O island decode table |
| `memcjr_io_ranges` | `memc.c` | the MEMCjr boards' I/O island decode |
| `iosb_t` | `iosb.h` | the IOSB configuration registers and VIA2's DREQ flag |
| `quadra040_t` | `../mcu/quadra040.h` | the devices and plumbing shared with the MCU family |

## 3. Behaviour/algorithms

**RAM banks.** The MEMC has ten bank registers ($04-$28); each bank's start
field counts 4 MB. A populated bank decodes from its start up to the next
populated bank above it, or 64 MB, and mirrors its size through that span:
the boot ROM sizes a bank by parking it on a 64 MB boundary and looking for
the repeat, then stitches the banks contiguous. Before a remap the pages the
last mapping reached (`ram_extent`, seeded to the whole 640 MB span at the
layout and the `/RESET` re-arm) are cleared, so a moved bank leaves no stale
image. A bank register landing while the ROM overlay is armed remaps
nothing; the overlay drop maps the banks as they then stand. A RAM total
that is no SIMM population of the board is presented as one bank. Register
$00 bits 9-11 (the chip version) read 0, the part the ROM does not enable
interleaving on, which keeps the bank map linear.

**VRAM.** The VRAM decode spans the whole 2 MB aperture at $F9000000 with
the fitted VRAM repeating through it: PrimaryInit reads the top of every
possible size before it sizes, and must not bus-error.

**SONIC.** Every board is built with its Ethernet; the ROM resets the SONIC
unconditionally on all of them.

**Kiwi.** The NuBus controller's register file is the YANCC's layout (one
bit per longword, `mcu/yancc.c`); with the write-buffer-error flag and its
enable both set it requests level 7. `/RESET` clears it and drops the
request; the memory controller's bank registers and the IOSB's timing
registers survive `/RESET` (the RAM decode must outlive a RESET
instruction, and the ROM reprograms them).

**SCSI.** The SCSI Manager on these ROMs moves data through the 53C96
pseudo-DMA port as longwords (the BIOS path), leaving an odd byte for the
FIFO. The 53C96 model moves that residual into the FIFO and lets the target
advance before it posts the Terminal Count interrupt, which is what lets the
System 7.5 driver see the phase change. Boards whose DMA engine drains the
port (`scsi_53c96_set_bus_master`: PDM, TNT) have no residual.

## 4. Object-model / shell surface

Nothing family-specific: the machine's devices appear under the usual
`machine.*` nodes (`machine.scsi`, `machine.floppy`, `machine.nubus`).

## 5. Checkpointing

The `memc` part saves the overlay state, the MEMC and Kiwi register
files, the MEMCjr per-bank latch bits, the slot-IRQ mask, the SONIC byte
latch and the IOSB; the MEMCjr boards' Cuda is its own `cuda` part; a restore
re-applies the bank map through the overlay drop. The DAFB's monitor strap
is its own identity part (`dafb.monitor`). `suite-memc`'s
`q800-checkpoint` row round-trips a booted Finder, `suite-memcjr`'s
`q605-checkpoint` the MEMCjr side.

## 6. Testing

`tests/integration/suite-memc`: eleven rows booting all five models from
SCSI and floppy (System 7.1 with System Enabler 040, 7.5, 7.5.3, 7.6) to
About This Macintosh, asserting machine id, ROM id and BoxFlag, plus the
checkpoint row. `tests/integration/suite-memcjr`: seven rows booting the
LC 475, LC 575 and Quadra 605 from SCSI and floppy (System 7.1 with System
Enabler 065, 7.5, 7.5.3, 7.6) to About This Macintosh, plus a checkpoint
row. `machine-capabilities` and `machine-profile-schema` pin the profiles.

## 7. Known debts

- The IOSB's wait states and the I/O penalties are the Quadra 700's, not
  measured on this board.
- Interleaving (chip version > 0) is not modelled.
- Nothing on the modelled NuBus raises the Kiwi write-buffer error.
- MEMCjr: bank register bit 10 (a mode bit the ROM preserves) and the
  timing registers ($30, $34) latch and read back without effect; the
  PrimeTime's sound input and DFAC II are not modelled beyond the IOSB's
  sound block; no card is offered for the LC PDS or the communication slot.

## 8. See also

[../mcu/mcu.md](../mcu/mcu.md) (the DAFB and the Quadra 700 wiring this
family reuses), [../../core/peripherals/scsi_53c96.md](../../core/peripherals/scsi_53c96.md).
