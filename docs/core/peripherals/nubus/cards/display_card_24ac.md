# Apple Macintosh Display Card 24AC

Third-party OEM NuBus display card (Apple-branded, 1992) with a hardware
QuickDraw fill/raster **acceleration engine**. Modelled in
[`src/core/peripherals/nubus/cards/display_card_24ac.c`](../../../../../src/core/peripherals/nubus/cards/display_card_24ac.c).
Catalogued in [`video.md`](../../video.md) §2.3; vROM byte-layout reference in
[`nubus_vrom.md`](../../nubus_vrom.md).

| | |
|---|---|
| **Card kind** | `display_card_24ac` |
| **vROM** | `display-card-24ac-d8daab87.vrom` (carries its own System 7 video driver) |
| **Slot** | any NuBus socket — IIcx video slot `$9` in the tests, and slot `$C` of a Power Macintosh 7100/8100 behind BART (selectable via `machine.boot video_card=` or `slot[N].card_id`) |
| **VRAM** | 4 MB, flat aperture at slot offset 0 (no VideoBase/RowWords register) |
| **Depths** | 1 / 4 / 8 / 16 / 32 bpp (no 2 bpp) |
| **Distinctive** | a fill/copy/ROP accelerator the OS's cdev drives in 32-bit mode |

## 1. What is modelled

Two halves (proposal `proposal-nubus-card-display-card-24ac.md` §0):

- **Display** — loads the genuine vROM and presents a linear framebuffer + CLUT
  + VBL slot IRQ. The card's own vROM video driver programs the standard video
  registers; we model the ones it touches (CLUT, depth/mode latch, VBL mask/ACK,
  monitor sense) and **accept-and-log** the rest (CRTC timing file, RAMDAC
  command, serial PLL) so an OS write that should be a no-op never bus-errors.
- **Engine** — STATUS/CONFIG/CONTROL registers, the operand aperture (+ commit
  alias), and the `+0x400000` active-bank alias that transforms writes into
  run-length fills / block copies / ROPs. Implemented as a synchronous
  software-equivalent straight into the passive VRAM model; its output must
  match the driver's own CPU fallback (the built-in oracle, §3.5). **The engine
  only engages in 32-bit addressing mode with the cdev installed** — a 24-bit
  IIcx boot uses the plain framebuffer path.

Notable vs the 8•24 ([`display_card_8_24.md`](display_card_8_24.md), the JMFB
ASIC): no VideoBase/RowWords register
(the framebuffer base is a driver-private VRAM pointer, so VRAM is a flat
aperture at offset 0), and the standard video registers share the high slot
pages with the engine behind one dispatcher.

## 2. Slot register map

All display registers are byte-wide on lane 3. Offsets are slot-relative
(add `nubus_slot_base($9)` = `0xF9000000` on the IIcx).

| Offset | Register | Notes |
|---|---|---|
| `0x000000`–`0x37FFFF` | **VRAM** (host-mapped visible bank) | flat; `0x380000` = `VRAM_VISIBLE` |
| *(driver's choice)* | **Operand aperture** | where the driver stages the operand / pattern bytes — **not a fixed offset**, see below |
| `0x400000`+ | **Active-bank alias** | engine-transforming write window (see §3) |
| aperture + `0x400000` | operand **commit** / pattern-row select | write `4` = latch 4 bytes; write `8` = latch this row's 8 pattern bytes |

The operand aperture's **address is chosen by the driver at init and must not
be hardcoded**. Measured with the same card and the same cdev: a IIcx uses
`0x3FE000` and a Power Macintosh 8100 uses `0x3F8000` (the hardware spec also
documents `0x0FE000` for a small-VRAM card). The model therefore recognises
these commands by their *shape* — a longword written through the alias, aimed
above `VRAM_VISIBLE`, which is far above the largest raster this card scans
out, so a real fill can never land there — and takes the pattern bytes from
the VRAM the driver just staged them in. That works at any aperture base,
including one below `VRAM_VISIBLE`, whose passive writes reach VRAM through
the host mapping without the card model seeing them at all.

Two command widths arrive, and both mean "these N bytes are the current
pattern, aligned to absolute VRAM columns":

- **`4` — operand commit.** The solid-colour path: one longword, replicated.
- **`8` — pattern-row select.** QuickDraw expands an 8×8 pattern into 8 rows
  of 8 bytes and the driver stages all 64 at the aperture; before each
  scanline's fill it points the engine at the row that scanline needs,
  cycling 0–7. This is the ROM's per-row pattern selector, offloaded. The
  selects are interleaved *between* the fills, one per row, and are **not**
  followed by a commit — which is what makes a dither alternate line to line.
  Treating them as fills collapses every patterned fill to a solid colour
  (the symptom: a white or black Finder desktop instead of the 50% gray).
| `0xC8000A` / `0xC8000E` | CLUT init data / index | |
| `0xC80016` | CLUT end-of-load strobe | accept-and-log |
| `0xC8001A` / `0xC8001E` | CLUT runtime data / index | index write resets the R/G/B sub-counter; 3 data writes load R,G,B |
| `0xD00402` | **STATUS** (read) | `[2:0]` depth code, `[3]` class, `[4]` busy/sync toggle |
| `0xD00403` | **VIDCTL** (r/w) | bit 7 VBL mask, bit 5 commit strobe, bit 3 direct/"magic" |
| `0xD40402` | **CONFIG** (read) | `[0]` geometry variant |
| `0xD40403` | **CONTROL** (write) | latches the engine op mode (§3) |
| `0xD80001` | **MODE** | bits 7-5 depth code, bits 4-0 timing (= CRTC[9]) |
| `0xD80005` | **DEPTH** | low nibble depth/clock |
| `0xD8000D` | **SENSE / PLL** | sense read (bits 7-5, inverted) + serial-PLL bit-bang |

⚠ The RAMDAC/CLUT port is **active-low**: both the palette index and each R/G/B
component arrive as the one's-complement; the model inverts them back
(`clut_set_index` / `clut_write_data`). See `video.md` and errata **E7**.

**Depth ladder** (vROM `cscSetMode` table, chip `0x1F88`) — there is no 2-bpp mode:

| MODE[7:5] | `0x00` | `0x20` | `0x40` | `0xA0` | `0xC0` |
|---|---|---|---|---|---|
| bpp | 1 | 4 | 8 | 16 | 32 |
| depth code | 0 | 1 | 2 | 3 | 4 |

## 3. Acceleration engine

`CONTROL` (`0xD40403`) latches the op mode; writes through the **active-bank
alias** (`dst + 0x400000`) then drive it. Modes: `$01` **fill**, `$03`
**stretch**, `$7F` **block copy**; computed ROP codes `$00`–`$3F` fall back to a
straight copy with a log (still oracle-checkable, never corrupts the image).

A single **command flag, bit 30 (`0x40000000`)**, is shared by fill and copy to
mark the "execute" write; the byte count / length is always the low bits.

### 3.1 Fill (`$01`) — errata E8
The cdev OR-s the command flag into every run-length write (a 640-byte scanline
fill arrives as `$40000280`). The run length is the low bits; **mask the flag**
before using it as a byte count. Modelled in `engine_fill_run`, which replicates
the latched operand across `len` bytes.

### 3.2 Copy (`$7F`) — two-write handshake, errata E9
A VRAM→VRAM block copy is **not** "store the written longword as source pixels".
It is a two-write handshake:

```
write LONG [srcPos + 0x400000] = L                 ; no flag  → LATCH source = srcPos, len = L
write LONG [dstPos + 0x400000] = 0x40000000 | L    ; flag bit → COPY L bytes srcPos → dstPos
```

Modelled in `engine_store_long`: latch on the no-flag write, `memmove` on the
flagged write (source/dest may overlap; the driver issues scanlines in
overlap-safe order).

### 3.3 Source pointer auto-increments — errata E10
The latched source is an **auto-incrementing pointer**: it advances by each
executed length, so one latch can feed several executes. The driver uses this
when a copy's *destination* would straddle an **8 KB VRAM boundary**
(`dst & 0x1FFF`) — it splits into two executes from one latch and lets the engine
continue the source:

```
LATCH src=$a788 len=512
COPY  dst=$7f88 len=120        ; 0x7f88+120 = 0x8000 (boundary)
COPY  dst=$8000 len=392        ; src auto-advances 0xa788+120 = 0xa800
```

`engine_store_long` does `engine_copy_src += len` after every execute. Pinning
the source made the second execute re-read the source head, leaving a **dotted
"....." artifact** on every boundary-crossing scanline — visible only on
one-line (scrollbar / drag) scrolls, not page jumps. Trace tell: COPY writes
outnumber LATCH writes by exactly the number of boundary crossings.

## 4. vROM identity

`machine.vrom.identify` keys the card off the declaration ROM's **NuBus
Format-Block CRC** (`chip[size-12..size-9]`, big-endian; TestPattern
`$5A932BC7` at `chip[size-6..size-3]`), the analog of `rom.identify`'s checksum.
It returns `card_id = "display_card_24ac"` and `compatible = [...]`. The
human-readable card name is owned by the card kind (`machine.profile`), not the
identify result. See [`nubus_vrom.md`](../../nubus_vrom.md) and
[`src/core/memory/vrom.c`](../../../../../src/core/memory/vrom.c).

## 5. Tests

| Test | Coverage |
|---|---|
| `tests/integration/iicx-24ac` | end-to-end 32-bit SCSI boot → 8-bpp colour Finder; Apple-menu fill (E8); Control-Panels list scroll (E9/E10) — all pixel-exact |
| `tests/integration/iicx-display-card-24ac` | engine decode + engine-vs-CPU-fallback oracle; copy-handshake Part A; object-model nodes |
| `tests/integration/vrom-identify` | CRC identity + `card_id` / `compatible` |

## 6. Provenance

This page is the canonical reference for the card. It distils the
reverse-engineering behind the model — register map, engine semantics, errata
E7–E10 — so the load-bearing facts live here rather than in working notes; the
model in
[display_card_24ac.c](../../../../../src/core/peripherals/nubus/cards/display_card_24ac.c)
is the implementation those facts describe.
