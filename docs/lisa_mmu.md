# Lisa Segment MMU — implementation notes

`src/core/memory/lisa_mmu.{c,h}` implements the Apple Lisa custom segment MMU,
the first non-Motorola memory translator in the emulator. This document records
the *implementation* decisions and the hardware facts verified against the
rev-H boot ROM source; [docs/lisa.md](lisa.md) §4–7 is the hardware reference of
record.

## Where it plugs in

The Lisa MMU is **not** the Motorola PMMU (`mmu.c`). It coexists behind the same
`memory.c` translation seam, selected by the global `g_lisa_mmu` (non-NULL only
while a Lisa/XL machine is active). Each `memory.c` slow-path function — the six
CPU read/write paths plus the six side-effect-free debug read/write paths —
begins with a predicted-not-taken `if (g_lisa_mmu) return lisa_mmu_*(...)`
delegate. For every Mac-architecture machine `g_lisa_mmu` is NULL and the
behaviour is byte-for-byte unchanged.

**Page-size decision (proposal R1).** The hot-path SoA cache hard-codes
`PAGE_SHIFT 12` (4 KB). Rather than parameterise it (approach A, which perturbs
the tuned 68030/A-UX paths), the Lisa takes approach **B**: its SoA arrays are
left empty, so every Lisa access misses the fast path and is fully translated in
the slow-path delegate. A boot-ROM self-test is a few million instructions, so
pure-slow-path is fine for bring-up; SoA fast-path filling (legal because a 4 KB
logical page always lies within one 128 KB-aligned segment and maps to a
contiguous physical run) is a later performance optimisation.

## Translation model (verified against the ROM source)

24-bit logical address = `segment(23-17) | page(16-9) | byte(8-0)`; pages are
512 bytes. Each of 128 segments × 4 contexts has a 12-bit **SOR** (origin
physical page number) and 12-bit **SLR** (bits 11-8 = access/space code, bits
7-0 = length in pages, two's complement).

- **Space codes** (SLR 11-8): `01xx` main memory (bit1=R/W, bit0=non-stack),
  `1001` I/O, `1111` special-I/O (ROM + descriptor RAM), `1100` invalid.
- **Limit check** (non-stack): the hardware adds the page displacement to the
  two's-complement length byte and faults on an 8-bit carry — i.e. valid iff
  `(page + limit_byte) < 256`. `MEMLMT = $0700` → length byte `$00` → all 256
  pages valid (a full 128 KB segment). Stack segments invert the carry sense:
  the *top* `(limit_byte + 1)` pages of the segment are valid and it grows
  downward (limit byte `$00` ⇒ 1 page valid, `$FF` ⇒ all 128 KB) — `lisa_mmu.c`
  approximates this as `(page + limit_byte) >= 256`.
  - **The Lisa OS does exercise stack segments.** `superstkmmu = 101`
    (`source-MMPRIM.TEXT:78`, *"mmu used to map the supervisor's stack"*) makes
    **logical segment 101 (`$ca0000–$cc0000`) the supervisor stack**, mapped in
    context 0 (supervisor); the kernel SSP base is `$cc0000` and grows down into
    it. So a wrong stack-limit sense (or an unmapped seg 101) makes every
    supervisor-mode exception-frame push fault — watch this when debugging
    post-mount faults. (The 68000 group-0 exception frame is only 14 bytes; do
    not push the 68030 92-byte Format-$B frame on the Lisa.)
- `physical_page = (SOR + page) & 0xFFF` (high nibble forced to 0);
  `physical = (physical_page << 9) | byte`.

### START / setup mode

START is set at power-on. While START is set, **logical bit 14 is a switch**:

- `bit14 = 0` → special-I/O *directly*, MMU bypassed. Sub-decoded by **bit15**:
  `0` → boot ROM (offset `addr & $3FFF`); `1` → descriptor RAM
  (segment = bits 23-17, SOR if bit3 else SLR).
- `bit14 = 1` → normal segment translation.

The boot ROM programs the descriptor RAM under START via `$xx8000`/`$xx8008`
writes (`SETMMU`), then strobes **SETUP off** at the `START` label to enter
"map land". Its runtime descriptor-write helper `WRTMMU` toggles SETUP on →
write → off.

### Contexts

Active context = `(SEG2<<1)|SEG1` from the strobe latches. **Normal translation
forces context 0 in supervisor mode**; **descriptor-RAM access uses the raw
latch context with no supervisor override** — proven by the ROM's `CONCHK`
context test, which strobes `SEG1ON` and expects the descriptor regs to read
back context-1's (unprogrammed) values, not context 0's.

## Control strobes ($00E000-$00E01E)

Each is a *strobe*: any access (read or write, value ignored) toggles the latch.
Decoded by `phys & 0x1E`:

| addr | latch | addr | latch |
| --- | --- | --- | --- |
| `$E008` | SEG1 = 0 | `$E00A` | SEG1 = 1 |
| `$E00C` | SEG2 = 0 | `$E00E` | SEG2 = 1 |
| `$E010` | **START = 1** | `$E012` | **START = 0** |
| `$E018` | VTMSK = 0 | `$E01A` | VTMSK = 1 |

> **Doc discrepancy (flagged).** [docs/lisa.md](lisa.md) §6.1 lists `$E010` as
> "reset SETUP/START" and `$E012` as "set" — the **reverse** of reality. The
> rev-H ROM equates are authoritative: `SETUPON .EQU $00FCE010` ("turn SETUP
> on") and `SETUP .EQU $00FCE012` ("turn SETUP bit off"), and `WRTMMU` does
> `TST.B SETUPON` (enable MMU-reg access) … `CLR.B SETUP` (back to map land).
> This module implements the ROM's polarity. docs/lisa.md §6.1 should be
> corrected.

## Status / video / I/O

- `$E800` Video Address Latch (write): holds A15-A20; framebuffer base =
  `latch << 15` (consumed by the display in Step 3).
- `$F800` Status Register (read): bit 2 = vertical retrace, modelled
  cycle-accurately and **active-low** — `lisa_status_byte()` derives it from the
  cycle counter (84896-cycle frame, 458-cycle retrace window; clock supplied by
  `lisa_mmu_set_clock()`), with the `vertical` latch set on the retrace-window edge
  and cleared during active scan **and by the VTIRDIS strobe (`$E018`)**. This
  matches the real hardware the ROM's VIDTST expects (wait bit 2 = 0 → strobe
  VTIRDIS → bit 2 = 1) and gives the OS a real-time retrace cadence. (Earlier this
  was an active-high per-read toggle — a hack that passed VIDTST but ran the OS
  clock ~150× fast; see docs/lisa.md §7.4.) Other status bits read 0 for now.
- `$F000` Memory Error Address latch (read): 0 (no error model yet).
- Peripheral devices (VIA/SCC/floppy/Widget) register physical I/O ranges via
  `lisa_mmu_map_io()`; unmapped I/O currently reads all-ones (a faithful
  bus-error-on-absent-I/O refinement waits until the devices land).

## Bring-up status

The Lisa machine (`src/machines/lisa.c`: 68000 + RAM + 16 KB ROM + this MMU +
the two 6522 VIAs) boots the rev-H boot ROM headlessly through: ROM checksum →
MMU register R/W + address + context tests → `SETMMU` default map (RAM
contiguous from 0, segment 126 → I/O, segment 127 → special-I/O) → START→map-land
transition → memory sizing → preliminary memory test → **VIA timer self-test**
(both VIAs, via the stride-2/stride-8 adapter), and reaches the **COPS POST**.

With the COPS ([cops.c](cops.md)), video (Step 3), the SCC, the floppy
controller ([lisa_fdc.c](lisa_fdc.md)), the **serial-number PROM**, and the
**parity-error NMI** all modelled, the rev-H boot ROM **completes the entire
power-on self-test and auto-boots**, reading the inserted floppy. The chain of
gates cleared along the way: video self-test (`VIDTST`, frame-accurate retrace
bit) → serial number (`RDSERN`, the `$FE8000` bit-serial PROM emitter, §serial
below) → parity (`PARTST`, needs a level-7 NMI — see the CPU fix below) → SCC
RS-232 loopback → COPS clock read.

### Serial-number PROM ($FE8000)

`RDSERN` reads the machine serial bit-serially from `$FE8000` (sampling bit 15
of each word over two 56-word blocks), finds a sync byte, extracts nibbles, and
validates a decimal checksum. lisa_mmu emits a synthetic stream — an 8-bit sync
at the start of each half, zero elsewhere — whose decoded all-zero nibbles make
the checksum reduce to the four `$0F` sync nibbles (4×$0F = 60) cancelling the
ROM's −60, i.e. 0 == 0. Faithful: a real engraved serial would only change the
displayed/stored number, not the boot outcome.

### Parity-error NMI + a CPU fix

`PARTST` forces a wrong-parity write (DG2ON) then reads it back expecting a
**level-7 NMI** + the Memory Error Address latch. lisa_mmu marks the
written 32-byte granule "bad" and, on a parity-detect read of it, latches the
address and asserts the level-7 NMI (a scheduled one-shot, since the level-IPL
model would otherwise re-fire). This surfaced a genuine CPU bug:
`cpu_check_interrupt` used `ipl > mask`, which blocks level 7 at mask 7 — but
**level 7 is non-maskable** on the 68000. Fixed to `ipl > mask || ipl == 7`
(regression-clean across the Mac machines).

**Remaining gate to running an OS:** the boot block's sector tag — see
[lisa_fdc.md](lisa_fdc.md) (needs DC42 tag-load infrastructure).
