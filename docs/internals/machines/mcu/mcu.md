# The MCU family substrate (Quadra 700 / 900 / 950)

The **MCU/Orwell generation** — the 68040 Quadras — is Granny Smith's first
68040 family. The shared substrate lives in
[src/machines/mcu/mcu.c](../../../../src/machines/mcu/mcu.c) /
[mcu.h](../../../../src/machines/mcu/mcu.h); the machines bind it through an
`mcu_board_t` (per-machine hooks) + `mcu_board_desc_t` (per-machine data),
the same pattern the GLUE/MDU/OSS families use. MCU is the memory
controller that defines the generation (like GLUE, MDU, OSS before it);
the family also carries the JDB/Relayer datapath, the YANCC NuBus bridge,
and the stand-alone DAFB video controller ([dafb.md](dafb.md)).

The devices and plumbing this generation shares with the djMEMC family
(Centris/Quadra 610/650/800, LC 475/575, Quadra 605 —
[../djmemc/djmemc.md](../djmemc/djmemc.md)) live in
[quadra040.c](../../../../src/machines/mcu/quadra040.c) /
[quadra040.h](../../../../src/machines/mcu/quadra040.h), and the YANCC
register file in [yancc.c](../../../../src/machines/mcu/yancc.c) /
[yancc.h](../../../../src/machines/mcu/yancc.h) (see "Shared 040 Quadra
block" below).

Machine pages: [q700.md](q700.md), [q900.md](q900.md), [q950.md](q950.md).
Hardware reference: [mcu.md](../../../reference/machines/mcu/mcu.md) (family),
[q700.md](../../../reference/machines/mcu/q700.md),
[q900.md](../../../reference/machines/mcu/q900.md),
[q950.md](../../../reference/machines/mcu/q950.md) (machines),
[dafb.md](../../../reference/machines/mcu/dafb.md) — cited by section below.
Evidence labels below follow the DAFB-family implementation reference:
**[A]** Apple-documented, **[R]** reverse-engineered, **[U]** unresolved,
**[I]** inferred, **[D]** datasheet.

## Family traits

- **Access-triggered ROM overlay [A].** Out of reset the 1 MiB boot ROM is
  mapped at `$00000000`. The **first access to the normal ROM aperture**
  (`$40000000–$4FFFFFFF`) drops the overlay — RAM appears at zero and the
  aperture becomes direct ROM mirrors. Unlike GLUE/MDU there is no VIA
  overlay bit; `mcu.c` registers the shared `m68k_board_overlay` trigger
  device over the aperture and arms it (`m68k_board_overlay_arm`), and the
  first access drops the overlay.
- **MCU/Orwell register file at `$5000E000`.** Orwell has a single config
  data bit, written and read one bit at a time through a run of longword
  addresses (bit N at byte offset N*4, only bit 0 of the datum wired). The
  model implements the semantics recovered from the ROM sources
  (`HardwarePrivateEqu.a` "Orwell Memory Controller Equates": bank start
  addresses, DRAM/ROM speed, refresh, page mode — the `orwell_cfg` word,
  with the bank starts latched when the matching latch address is poked).
  (Reference: [mcu.md](../../../reference/machines/mcu/mcu.md) §2.4–§2.7.)
  The YANCC bridge file at `$50028000` is **accept-and-log with readback**
  (`yancc.c`): 64 longword slots latch writes and read back verbatim, and
  every first touch is logged (`log.set board`). Bits 7 (write-buffer
  error) and 10 (its enable) together request level 7; `/RESET` clears the
  file and drops the request.
- **256 KiB I/O island at `$50000000`**, mirror mask `$3FFFF`, run on the
  shared board_io engine (reference: [mcu.md](../../../reference/machines/mcu/mcu.md)
  §3.2). Q700 decode: VIA1 `$0000`, VIA2 `$2000`, MAC
  PROM `$8000`, SONIC `$A000`, SCC `$C000`, MCU `$E000`, 53C96 `$F000`,
  SCSI pseudo-DMA `$F100`, EASC `$14000`, SWIM `$1E000`, YANCC `$28000`.
  The towers swap direct SCC/SWIM for IOP apertures and add the external
  53C96 windows (see [q900.md](q900.md)).
- **Interrupts:** VIA1→IPL1, VIA2→IPL2, SCC→IPL4, NMI→IPL7 — the same
  routing table as the GLUE family, so `m68k_board_update_ipl` is reused
  verbatim. Slot-level sources aggregate on **/SLOTIRQ** (below).
  (Reference: [mcu.md](../../../reference/machines/mcu/mcu.md) §5.1–§5.2.)
- **Bus-side physical resolver.** The 68040's MMU is on-chip
  ([mmu040.md](../../core/memory/mmu040.md)); each machine builds a bus
  `mmu_state_t` (flat RAM at 0 + ROM-aperture mirrors) and attaches the
  CPU-owned register file with `mmu_attach_mmu040`, so table walks and
  physical DMA (SONIC) resolve through one place.

## /SLOTIRQ aggregate

VIA2 port A carries the slot-level interrupt requests, all active-low:
PA0 Ethernet (SONIC), PA1–PA5 NuBus slots $A–$E, PA6 built-in video
(DAFB). `quadra040_slot_irq_source` maintains the `slot_pa_mask` aggregate:
each source drives its PA input line, and CA1 (/SLOTIRQ) follows the OR
of all sources. NuBus card /NMRQ lines route in via the substrate's
`nubus_slot_irq` hook (slot $A → PA1 … $E → PA5).

## Shared 040 Quadra block

`quadra040_t` is the first member of `mcu_state_t` (and of the djMEMC
family's `djmemc_state_t`), so `quadra040_st(cfg)` reaches it whichever
family runs. It holds the devices both generations carry — EASC, DAFB,
the first 53C96, SONIC and the MAC-PROM bytes, the `/SLOTIRQ` mask, the
YANCC/Kiwi `yancc_t`, the ROM overlay, the bus MMU and the I/O engine —
and `quadra040.c` the code over them: the init prefix
(`quadra040_build_core`: core, IRQ part, low-speed spine, VIAs) and suffix
(`quadra040_build_finish`: NuBus, host regions, finish), the idle VIA
lines, the SCSI / SONIC-EASC-SWIM / DAFB / bus-MMU builders, the island
bind and the island, video (VRAM repeating through the 2 MB aperture) and
overlay mappings, the shared half of `/RESET`, teardown, restore
re-drives, the VBL and display hooks, the IRQ callbacks, the PROM / SONIC /
YANCC island handler rows and the DAFB monitor table
(`quadra040_monitor_*`). `M68K040_BOARD_COMMON(chipset, io)` is the
descriptor `.common` block both families use (`MCU_BOARD_COMMON(io)` is
it with `"MCU+DAFB"`).

RAM banks are mapped by the shared `m68k_board_map_banks`
(`runtime/m68k_board.c`): each populated bank mirrors from its start to the
next bank above it or its 64 MB window. Before a remap the pages the last
mapping reached (`ram_extent`, seeded to the Orwell's 316 MB reach at the
layout and at the `/RESET` re-arm) are cleared, so a bank the ROM moves
while sizing leaves no stale image, and the bus MMU's TLB is flushed.

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

- `log.set board` — the machine/board glue (model-independent, so the same
  spec works on every model); `log.set video` (the DAFB) / `log.set 53c96` /
  `log.set sonic` — per-chip categories; level 3 logs every register access.
- `machine.cpu.mmu` — the 040 MMU inspector
  ([mmu040.md](../../core/memory/mmu040.md)).
- RE artifacts from bring-up (boot-time MCU and DAFB access logs) are
  archived outside the repo tree; regenerate with the log categories
  above.

## Known debts

- YANCC register semantics remain accept-and-log [U]; annotating the
  captured access logs against the ROM listing would harden them.
- EASC is still the plain ASC-compatible core (boot chime works; the
  recording path and EASC FIFO extensions are deferred).
- The DAFB block-write engine register is a latch only [U].
