# djMEMC family — Centris 610/650, Quadra 610/650/800

`src/machines/djmemc/djmemc.c` implements the five djMEMC machines as one
substrate (`djmemc_substrate`) with a per-model board descriptor
(`djmemc_board_desc_t`, reached through `hw_profile_t.board`);
`src/machines/djmemc/wombat.c` holds the descriptors and profiles, and
`src/machines/djmemc/iosb.c` the IOSB's register file. Hardware reference:
[djmemc.md](../../../reference/machines/djmemc/djmemc.md) (family) and its
machine pages ([c610.md](../../../reference/machines/djmemc/c610.md),
[c650.md](../../../reference/machines/djmemc/c650.md),
[q610.md](../../../reference/machines/djmemc/q610.md),
[q650.md](../../../reference/machines/djmemc/q650.md),
[q800.md](../../../reference/machines/djmemc/q800.md)).

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
(`djmemc_board_desc_t.via1_id`). Both ROMs (`F1A6F343`, `F1ACAD13`) boot all
five models; the BoxFlag column is what each derives.

The substrate reuses the Quadra 700's pieces where the hardware is
software-compatible: the DAFB model (`mcu/dafb.c`) for the built-in video,
with `dafb_set_ac842a` and `dafb_set_antelope` making it the Wombat DAFB plus
the Antelope CLUT/DAC (PCBR1's mfg/rev nibble reads non-zero); the 53C96,
SONIC, SWIM II, ASC and the two VIAs as on the Quadra 700; the shared
`board_io` engine for the I/O island at $50000000 (`djmemc_io_ranges`).

The Centris 610's FPU is absent through `hw_profile_t.fpu_absent`, which
`m68k_board_build_core` passes to `cpu_init_fpu`: every FPU instruction then
takes the F-line trap and the System's software floating point runs.

## 2. Key types & files

| Type / function | File | Purpose |
|---|---|---|
| `djmemc_board_desc_t` | `djmemc.h` | per-model data: identity straps, soldered RAM, sockets, VRAM |
| `djmemc_state_t` | `djmemc.h` | register files, devices, overlay, bus MMU |
| `djmemc_ram_banks` | `djmemc.c` | decomposes a RAM total into the ten 4/16 MB banks |
| `djmemc_map_ram` | `djmemc.c` | applies the bank registers to the page table |
| `djmemc_io_ranges` | `djmemc.c` | the I/O island decode table |
| `iosb_t` | `iosb.h` | the IOSB configuration registers and VIA2's DREQ flag |

## 3. Behaviour/algorithms

**RAM banks.** The djMEMC has ten bank registers ($04-$28); each bank's start
field counts 4 MB. A populated bank decodes from its start up to the next
populated bank above it, or 64 MB, and mirrors its size through that span:
the boot ROM sizes a bank by parking it on a 64 MB boundary and looking for
the repeat, then stitches the banks contiguous. Every page of the 640 MB span
is cleared before a remap, so a moved bank leaves no stale image. A RAM total
that is no SIMM population of the board is presented as one bank. Register
$00 bits 9-11 (the chip version) read 0, the part the ROM does not enable
interleaving on, which keeps the bank map linear.

**VRAM.** The VRAM decode spans the whole 2 MB aperture at $F9000000 with
the fitted VRAM repeating through it: PrimaryInit reads the top of every
possible size before it sizes, and must not bus-error.

**SONIC.** Every board is built with its Ethernet; the ROM resets the SONIC
unconditionally on all of them.

**Kiwi.** The NuBus controller's register file is the YANCC's layout (one
bit per longword); with the write-buffer-error flag and its enable both set
it requests level 7.

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

The `djmemc` part saves the overlay state, the djMEMC and Kiwi register
files, the slot-IRQ mask, the SONIC byte latch and the IOSB; a restore
re-applies the bank map through the overlay drop. The DAFB's monitor strap
is its own identity part (`dafb.monitor`). `suite-djmemc`'s
`q800-checkpoint` row round-trips a booted Finder.

## 6. Testing

`tests/integration/suite-djmemc`: eleven rows booting all five models from
SCSI and floppy (System 7.1 with System Enabler 040, 7.5, 7.5.3, 7.6) to
About This Macintosh, asserting machine id, ROM id and BoxFlag, plus the
checkpoint row. `machine-capabilities` and `machine-profile-schema` pin the
profiles.

## 7. Known debts

- The IOSB's wait states and the I/O penalties are the Quadra 700's, not
  measured on this board.
- Interleaving (chip version > 0) is not modelled.
- Nothing on the modelled NuBus raises the Kiwi write-buffer error.

## 8. See also

[../mcu/mcu.md](../mcu/mcu.md) (the DAFB and the Quadra 700 wiring this
family reuses), [../../core/peripherals/scsi_53c96.md](../../core/peripherals/scsi_53c96.md).
