# Compact family — Macintosh 128K, 512K, 512K enhanced, Plus, SE, SE FDHD, Classic

`src/machines/compact/compact.c` implements every 68000 compact Macintosh as
one substrate (`compact_substrate`) with a small per-model board descriptor
(`compact_board_t`, reached through `hw_profile_t.board`). Hardware
reference: [compact.md](../../../reference/machines/compact/compact.md)
(family) and its machine pages
([mac128k.md](../../../reference/machines/compact/mac128k.md),
[mac512k.md](../../../reference/machines/compact/mac512k.md),
[mac512ke.md](../../../reference/machines/compact/mac512ke.md),
[plus.md](../../../reference/machines/compact/plus.md),
[se.md](../../../reference/machines/compact/se.md),
[sefdhd.md](../../../reference/machines/compact/sefdhd.md),
[classic.md](../../../reference/machines/compact/classic.md)).

## 1. Responsibilities & design

The seven models are one machine shape: a 7.8336 MHz MC68000, RAM at zero
with its images repeating to $3FFFFF, the ROM at $400000, the SCC at
$9FFFF8/$BFFFF9, the floppy chip at $DFE1FF, one VIA at $EFE1FE, and a
512 × 342 1-bit screen fetched from MemTop − $5900. What varies is data:

| Model | id | board | ROM | SCSI | floppy chip | drives (fd0 / fd1) |
|---|---|---|---|---|---|---|
| Macintosh 128K | `mac128k` | PAL | 64 KB | — | IWM | 400K internal / 400K external |
| Macintosh 512K | `mac512k` | PAL | 64 KB | — | IWM | 400K internal / 400K external |
| Macintosh 512K enhanced | `mac512ke` | PAL | Plus 128 KB | — | IWM | 800K internal / 800K external |
| Macintosh Plus | `plus` | PAL | 128 KB | 5380 | IWM | 800K internal / 800K external |
| Macintosh SE | `se` | BBU | 256 KB | 5380 | IWM | 800K lower / 800K upper internal |
| Macintosh SE FDHD | `sefdhd` | BBU | 256 KB | 5380 | SWIM | SuperDrive lower / upper internal |
| Macintosh Classic | `classic` | BBU | 512 KB | 5380 | SWIM | SuperDrive internal / external |

`compact_board_t` carries `bbu` (the generation of general logic),
`has_scsi`, `floppy_chip`, `drive_select` (the SE and SE FDHD: PA4 steers
/ENBL1 between two internal drives) and `pwm_disk_speed` (the 128K and
512K: the disk-speed PWM sets the 400K drive's spindle; the 512K enhanced
has its own board for want of it). Every behavioural difference is a
branch on one of them.

**PAL boards** (128K, 512K, 512K enhanced, Plus) keep the Plus behaviour
that this file grew from: the overlay follows VIA PA4 — the shared
`m68k_board_overlay_t` under its FOLLOW_READS policy, which steers reads
only, so a write to low memory still reaches RAM — the keyboard is the M0110 on the VIA shift
register (`keyboard.c`), the mouse is quadrature through the SCC DCD lines
and VIA PB4/PB5 (`mouse.c`), PA3 selects the alternate sound buffer, and
the phase-read window at $F00000 answers 0. The overlay map also places
RAM at $600000 (Inside Macintosh, vol. III, ch. 2, `ovlyRAM`): the 64 KB
ROM runs its memory test and draws its error screen there before it
clears vOverlay. The image exists only while the overlay is on — it is
RAM from offset 0, wrapping on a board smaller than the 2 MB window — and
the range is empty again once the ROM drops the overlay; the overlay
object's `on_change` hook (`compact_pal_overlay_changed`) maps and clears
it. Both generations take the shared `m68k_board_bus_reset` (overlay
re-armed, then the common devices), and both map RAM back at zero with
`m68k_board_overlay_map_ram_identity`, which wraps into the installed RAM.

**BBU boards** (SE, SE FDHD, Classic) differ in four wired facts:

1. *Overlay.* The BBU drops the overlay on the first access to
   $400000-$5FFFFF. This is the shared access-trigger object
   `m68k_board_overlay_t` (DROP_ON_ACCESS) over $400000-$57FFFF; the armed
   flag is saved in the board's `compact` checkpoint part, since no VIA
   line re-derives it.
2. *ADB.* `adb.c` behind the VIA shift register and PB3-PB5, exactly as on
   the GLUE family (no shift-out callback; `adb.c` reads SR itself). There
   is no `keyboard.c` or `mouse.c` on these boards.
3. *PA4 is vDriveSel* (SE, SE FDHD): it steers /ENBL1 to the lower or the
   upper internal drive through `floppy_set_enbl_route`; /ENBL2 (the
   external port) reaches no drive on those two models. The Classic has
   one internal drive and leaves PA4 alone: its /ENBL2 is the external
   drive. PA3 is vSync, so there is no alternate sound buffer.
4. *SCSI interrupt.* The 5380's IRQ joins the VIA on /IPL0 behind the PB6
   mask (`compact_scsi_irq_update`, source `COMPACT_IRQ_SCSI`).

On the SE FDHD and Classic the SWIM sits in the IWM's window with the same
A9-A12 register decode: `compact_init` maps either chip with one call,
`floppy_map_stride_window` (`floppy.c`). The two chips' windows still
differ on a 16/32-bit access, which no compact ROM makes: the IWM's answers
open bus and logs the bus address, the SWIM's hands the access to the
chip's own open-bus handlers at the decoded register index (see §7).

## 2. Key types & files

- `compact_board_t` — the per-model data above.
- `compact_state_t` — the display descriptor, the PWM sound handle, the
  overlay object (either policy) and the latched SCSI IRQ level.
- `compact_substrate` — init, bus reset, teardown, PRAM seed, VBL, host I/O
  and media, and the display hook.

## 3. Behaviour/algorithms

**SCSI decode.** The 5380 answers over $580000-$5FFFFF on every board
with SCSI (`COMPACT_SCSI_BASE`/`SIZE`), the ROM decode ending where it
starts: A19 selects it and A18-A10 are don't-cares (Guide to the Macintosh
Family Hardware, 2nd ed., ch. 3), so the window is 512 KB. The Plus model
this family grew from mapped a 1 MB window from $500000, over the top of
the ROM's mirror range; the Plus now takes the same $580000 window as the
SE and Classic, where its ROM's SCSI addresses ($580000 read, $580001
write) fall.

**RAM decode.** RAM below 4 MB repeats up to $400000
(`memory_populate_ram_mirror`), except at 2.5 MB: there the two unequal
SIMM rows decode separately — the 2 MB row at $000000 and the 512 KB row
at $200000 repeating through $3FFFFF — and the SE ROM's sizing depends on
it (a plain wrap of the 2.5 MB total aliases $280000 onto $000000 and the
SE ROM crashes).

**The 400K drive's speed.** The 128K and 512K drive is single-sided and
has no speed control of its own: the board's disk-speed PWM sets its
spindle, and the 64 KB ROM calibrates that by timing /TACH at two speed
settings and dividing by the difference — a self-regulating drive gives a
zero difference and a divide-by-zero Sad Mac (0F0004). Once a frame
`compact_trigger_vbl` averages the disk-speed bytes (the low byte of each
of the 370 sound-buffer words) as positions along the PWM's 63-state
shift-register sequence and hands the resulting speed to
`floppy_set_pwm_rpm`, which `/TACH` then follows. The speed law (700 rpm
at position 0, 10 rpm less per step) is an assumption that spans the
five GCR zones; data timing still follows the track zone.

**PRAM.** The SE, SE FDHD and Classic ROMs validate XPRAM with 'Bugs', as
the Plus does, and take `pram_defaults_plus`. The 128K and 512K have the
20-byte RTC on hardware; the model uses the 256-byte chip
(`rtc_init(..., extended = true, ...)`), which their software never
addresses beyond the original 20 bytes (see §7).

## 4. Object-model / shell surface

Nothing family-specific: `machine.floppy`, `machine.scsi` (absent on the
128K/512K/512Ke), `machine.adb` (keyboard and mouse on every model — the
PAL boards' quadrature mouse and M0110 keyboard sit behind the same
`machine.adb.mouse`/`.keyboard` input surface), `machine.via1`,
`machine.sound`, `machine.screen`.

## 5. Checkpointing

Parts in build order: memory, cpu, scheduler, irq, rtc, scc, appletalk,
imagewriter, via1, sound, then `adb` (BBU) or `mouse` (PAL), images,
`scsi` (with SCSI), `keyboard` (PAL), floppy, and `compact` (BBU: the
overlay flag). The PAL overlay, the BBU drive-enable steering and the PWM
spindle speed are not saved: VIA re-drive and the next VBL restore them.

## 6. Testing

`tests/integration/suite-compact` boots every new model to the Finder off
floppy (and the SE, SE FDHD and Classic off SCSI too), opens the About box
and matches it; the Classic's ROM disk row holds Command-Option-X-O at
power-on. The Plus keeps `suite-plus`.

## 7. Known debts

- The 128K/512K RTC is modelled as the 256-byte part (rtc.h notes what the
  20-byte variant would need).
- The SE's processor-direct slot, the BBU's 265 ms bus-error timeout and
  its SCSI DRQ /DTACK handshake are not modelled (the 5380 completes a
  pseudo-DMA access at once).
- The SE/SE FDHD external floppy port and a third (external) drive are
  not modelled; the floppy controller has two drive positions.
- vSync (SE PA3, the modem's receive clock) is ignored.
- A wide access to the floppy window logs differently per chip (the IWM
  window names the bus address, the SWIM window the register index), and
  a wide debugger peek reads $FF.. from the IWM's but is composed from
  byte peeks of the SWIM's. Neither is guest-visible beyond open bus;
  kept as they were when the window moved into `floppy.c`.
- The SE, SE FDHD and Classic take the Plus's power-up SysParam; their own
  PRAMInit values were not measured.

## 8. See also

- [docs/reference/hardware/adb.md](../../../reference/hardware/adb.md)
- [docs/reference/hardware/iwm-floppy.md](../../../reference/hardware/iwm-floppy.md)
