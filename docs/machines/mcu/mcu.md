# The MCU family substrate (Quadra 700 / 900 / 950)

The **MCU/Orwell generation** — the 68040 Quadras — is Granny Smith's first
68040 family. The shared substrate lives in
[src/machines/mcu/mcu.c](../../../src/machines/mcu/mcu.c) /
[mcu.h](../../../src/machines/mcu/mcu.h); the machines bind it through an
`mcu_board_t` (per-machine hooks) + `mcu_board_desc_t` (per-machine data),
the same pattern the GLUE/MDU/OSS families use. MCU is the memory
controller that defines the generation (like GLUE, MDU, OSS before it);
the family also carries the JDB/Relayer datapath, the YANCC NuBus bridge,
and the stand-alone DAFB video controller ([dafb.md](dafb.md)).

Machine pages: [q700.md](q700.md), [q900.md](q900.md), [q950.md](q950.md).
Evidence labels below follow the DAFB-family implementation reference:
**[A]** Apple-documented, **[R]** reverse-engineered, **[U]** unresolved,
**[I]** inferred, **[D]** datasheet.

## Family traits

- **Access-triggered ROM overlay [A].** Out of reset the 1 MiB boot ROM is
  mapped at `$00000000`. The **first access to the normal ROM aperture**
  (`$40000000–$4FFFFFFF`) drops the overlay — RAM appears at zero and the
  aperture becomes direct ROM mirrors. Unlike GLUE/MDU there is no VIA
  overlay bit; `mcu.c` registers a trigger device over the aperture whose
  handler calls `mcu_overlay_drop`.
- **MCU register file at `$5000E000` [U].** Register semantics are not
  publicly documented, so the file is **accept-and-log with readback**: 64
  longword slots latch writes and read back verbatim, and every first touch
  is logged (`debug.log mcu`) so the ROM's access sequence itself becomes a
  reverse-engineering artifact. The YANCC bridge file at `$50028000` gets
  the same policy.
- **256 KiB I/O island at `$50000000`**, mirror mask `$3FFFF`, run on the
  shared mac030 I/O engine. Q700 decode: VIA1 `$0000`, VIA2 `$2000`, MAC
  PROM `$8000`, SONIC `$A000`, SCC `$C000`, MCU `$E000`, 53C96 `$F000`,
  SCSI pseudo-DMA `$F100`, EASC `$14000`, SWIM `$1E000`, YANCC `$28000`.
  The towers swap direct SCC/SWIM for IOP apertures and add the external
  53C96 windows (see [q900.md](q900.md)).
- **Interrupts:** VIA1→IPL1, VIA2→IPL2, SCC→IPL4, NMI→IPL7 — the same
  routing table as the GLUE family, so `mac030_glue_update_ipl` is reused
  verbatim. Slot-level sources aggregate on **/SLOTIRQ** (below).
- **Bus-side physical resolver.** The 68040's MMU is on-chip
  ([mmu040.md](../../core/memory/mmu040.md)); each machine builds a bus
  `mmu_state_t` (flat RAM at 0 + ROM-aperture mirrors) and attaches the
  CPU-owned register file with `mmu_attach_mmu040`, so table walks and
  physical DMA (SONIC) resolve through one place.

## /SLOTIRQ aggregate

VIA2 port A carries the slot-level interrupt requests, all active-low:
PA0 Ethernet (SONIC), PA1–PA5 NuBus slots $A–$E, PA6 built-in video
(DAFB). `mcu_slot_irq_source` maintains the `slot_pa_mask` aggregate:
each source drives its PA input line, and CA1 (/SLOTIRQ) follows the OR
of all sources. NuBus card /NMRQ lines route in via the substrate's
`nubus_slot_irq` hook (slot $A → PA1 … $E → PA5).

## Checkpoint / save-state

`mcu_checkpoint_save` writes the device stream **in the exact order each
board's `build_devices` constructs (= restores) them** — q900's superset
order with the tower entries guarded, so one save set serves all three
machines:

```
memory_map, cpu (incl. 040 MMU regs), scheduler, irq,
rtc, scc, via1, via2, adb, images,
scsi, 53c96, [scsi_ext, 53c96_ext], sonic, asc, floppy,
[caboose, scc_iop, swim_iop], dafb,
private tail: overlay flag, MCU regs, YANCC regs, slot_pa_mask,
              sonic write latch, tower wire-OR IRQ masks
```

`mcu_restore_private` (called at the end of each board's restore path)
reads the private tail and **re-drives the derived interrupt lines**: the
/SLOTIRQ PA lines + CA1 from the restored mask, and on the towers the
VIA2 CB2 (dual-53C96 wire-OR) and level-4 SCC (chip INT | SCC IOP host
INT) sources. The `q900-checkpoint` integration test pins the whole set:
a mid-boot save (IOP mailboxes, Caboose, and both 53C96s live), restored
in a fresh process, must finish booting to the pixel-exact desktop.

## Debug surfaces

- `debug.log mcu` / `debug.log dafb` / `debug.log scsi96` / `debug.log
  sonic` — per-chip categories; level 3 logs every register access.
- `machine.cpu.mmu` — the 040 MMU inspector
  ([mmu040.md](../../core/memory/mmu040.md)).
- RE artifacts from bring-up (boot-time MCU and DAFB access logs) are
  archived outside the repo tree; regenerate with the log categories
  above.

## Known debts

- MCU/YANCC register semantics remain accept-and-log [U]; annotating the
  captured access logs against the ROM listing would harden them.
- EASC is still the plain ASC-compatible core (boot chime works; the
  recording path and EASC FIFO extensions are deferred).
- The DAFB block-write engine register is a latch only [U].
