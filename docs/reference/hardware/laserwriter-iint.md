# The Apple LaserWriter II NT controller board

The LaserWriter II NT (logic board Apple 640-4105, © 1987) is a PostScript printer whose controller is a self-contained 68000 computer: a 1 MiB mask ROM containing Adobe PostScript v47.0 and the machine's boot firmware, 2 MiB of DRAM serving as the PostScript virtual-memory heap and page buffer, and a VIA/SCC pair that fans out to the serial ports, the front panel, the configuration EEPROM, and the Canon LBP-SX print engine [3]. This page documents the controller board itself — chipset, address decode, reset and boot behaviour as the ROM executes it, and the engine/panel interfaces. The printing protocols the firmware speaks over the wire (AppleTalk, PAP, the LaserWriter session protocol) are documented separately and are out of scope here.

Everything below is drawn from the machine's own 1 MiB ROM (SHA-1 `efa102a84be8dfea08931d0b550af2782cdbc42c`) and a survey of the physical board. Facts read out of the firmware cite the disassembly [1] or the ROM image analysis [2]; parts identifications cite the board survey [3]. Statements that rest on interpretation rather than observation are marked *inferred — unverified*, and the rest of what is not known is collected in §6.

**Contents:** 1. Identity · 2. Board architecture · 3. Memory map & address decode · 4. Per-subsystem wiring · 5. Boot sequence · 6. Open questions · References

---

## 1. Identity

The LaserWriter II NT has no machine family around it, so this page carries
the family role as well as the machine role (per the proposal's documentation
set): §2 and §3 are the board architecture and memory map a family doc would
own, and no separate family page exists or is planned.

| | |
|---|---|
| Product | Apple LaserWriter II NT |
| Logic board | Apple 640-4105, © 1987 [3] |
| Firmware | Adobe PostScript v47.0, © 1984–87 Adobe Systems, © 1981 Linotype; self-identifying string `LaserWriter II NT` at ROM `$29D394` [2] |
| CPU | Motorola MC68000 at 11.16 MHz (22.3210 MHz crystal ÷ 2) — *inferred — unverified*; see §2.1 |
| ROM | 1 MiB, 8 × Toshiba TC531000CP mask ROM, 16-bit wide (four high/low pairs) [3] |
| DRAM | 2 MiB, 16 × 511000 [3] |
| SRAM | 2 KiB, 2 × AMD Am9128-10 [3] |
| EEPROM | 512-byte Xicor X2804 (Apple part 335-0022) [3] |
| Serial | Zilog Z8530B1C SCC, two channels, RS-422 line drivers/receivers (26LS30/26LS32) [3] |
| Timer / GPIO | Rockwell R65NC22 VIA (Apple part 338-6523) [3] |
| Print engine | Canon LBP-SX, 300 dpi, fed through 2 × MMI 67L401 64×4 FIFO (byte-wide in pairs) [3] |
| ADB | Apple 342-0440-A transceiver, bit-banged from the VIA [3] |
| Resident fonts | the 35-face LaserWriter Plus set (41 font-name strings observed in the ROM, covering Times, Helvetica, Helvetica Narrow, Courier, Symbol, Palatino, New Century Schoolbook, Bookman, Avant Garde, Zapf Chancery, Zapf Dingbats, with the Linotype trademark notice at `$280578`) [2] |

## 2. Board architecture

```
68000 ──16-bit bus──┬── ROM 1 MiB ($200000)          ── 4 chip pairs, self-checksummed
                    ├── DRAM 2 MiB ($400000)         ── plus OR-write alias at $600000 (§3.2)
                    ├── SRAM 2 KiB (low bank, §3.1)
                    ├── VIA R65NC22 ($E00001)        ── timers, panel switches, engine handshake, ADB
                    ├── SCC Z8530 ($A00000/$C00000)  ── two RS-422 channels
                    ├── X2804 EEPROM ($C00000 even)  ── 512 bytes of configuration
                    ├── LED/engine latch ($800000)   ── write-only; + FIFO port at $800001
                    └── debug aperture ($F80000)     ── bring-up pod hook (§3.7)

SCC /INT ──► VIA CA1 ──► VIA IRQ ──► 68000 level 1 (autovector 1)      [1]
VIA T1, T2 ──► VIA IFR  ─┘            the VIA is the only interrupt source [1]
```

The board has no FPU, no DMA engine and no bus master other than the CPU. Every peripheral — including the print engine's video FIFO — is polled or interrupt-driven through the VIA, and all device interrupts arrive at CPU level 1 (§4.1). The DRAM OR-write alias (§3.2) is the board's one concession to throughput: it turns glyph compositing into single write cycles.

Because the 68000 has no floating-point unit and PostScript reals are IEEE-754 doubles, the ROM carries a software double-precision library behind a 17-entry `JMP` thunk table at `$2DBA74`; one entry (`$2DBACE` → `$2DBB28`) alone has 398 call sites, and the int32→double routine at `$2DBADC` normalises with an exponent seed of `$41E` (bias 1023 + 31) [2]. The thunk table is the hottest call path in the firmware.

### 2.1 Clocks

| clock | value | status |
|---|---|---|
| CPU | 11.16 MHz (22.3210 MHz ÷ 2) | *inferred — unverified* (crystal value not traced to the board) |
| SCC | 3.6864 MHz | *inferred — unverified*; implied by the diagnostic monitor's baud arithmetic (§4.5) |
| VIA | unknown | T1 runs from a latch of `$08B8`; the real-time period cannot be derived without the VIA clock rate |
| Engine byte clock (VIA PB6) | unknown | T2 counts PB6 pulses (ACR `$68`); frequency in §6 |

The CPU figure is consistent with observed firmware behaviour: the ROM interleaves a `NOP` between every pair of SCC register accesses [1], the classic remedy for a CPU fast enough to violate the Z8530's inter-access recovery time.

## 3. Memory map & address decode

Addresses are as the 68000 sees them. Rows the ROM itself exercises are cited [1][2]; the two coarse gaps the firmware never touches are called out below and in §6.

| range | width | access | contents |
|---|---|---|---|
| `$000000`–`$1FFFFF` | 16 | R/W | **banked low window** — ROM alias while the overlay is asserted; otherwise the 2 KiB SRAM (§3.1) |
| `$200000`–`$2FFFFF` | 16 | R | ROM, 1 MiB (§4.2) |
| `$300000`–`$3FFFFF` | — | — | never accessed by the ROM; mapping unknown (§6) |
| `$400000`–`$5FFFFF` | 16 | R/W | DRAM, 2 MiB |
| `$600000`–`$7FFFFF` | 16 | R/W | DRAM alias — **writes bitwise-OR** (§3.2) |
| `$800000` | 8 | W | LED + engine-control latch (§3.3) |
| `$800001` | 8 | W | byte FIFO to the print engine (§4.6) |
| `$A00000`–`$BFFFFF` | 8 | R | SCC read window, four byte ports repeating every 8 (§3.4) |
| `$C00000`–`$C003FF` (even) | 8 | R/W | EEPROM, 512 bytes on even addresses only (§3.5) |
| `$C00000`–`$DFFFFF` (odd) | 8 | W | SCC write window, four byte ports repeating every 8 (§3.4) |
| `$E00001`–`$E0001F` (odd) | 8 | R/W | VIA registers 0–15 (§3.6) |
| `$F80000` | 32 | R | debug-pod aperture (§3.7) |

How far each region mirrors above its listed extent is not established: the decode devices are not traced, and the firmware never relies on a mirror [1]. Two windows deliberately overlap — the even bytes of `$C00000`–`$C003FF` are the EEPROM while the odd bytes of the same range are SCC writes (§3.5) — so the region cannot be decoded as one device.

### 3.1 The banked low window and the ROM overlay

Out of reset an **overlay** maps the ROM over `$000000`–`$1FFFFF`, so the 68000 fetches its reset vectors from the ROM's first bytes (`$200000` as the ROM appears at its native base, §5.1). Once the overlay is released, the window holds the 2 KiB SRAM, mirrored within the window; the POST's SRAM test marches `$000000`–`$000FFF`, twice the chip's physical size [1], which confirms the mirroring. The mirror period (`$800`) and the behaviour of reads beyond the tested range are *inferred — unverified* (§6).

The overlay is released as a **side effect, not by an explicit instruction**. VIA ORB holds `$00` out of reset; the moment the POST programs DDRB with `$A9` (`MOVE.B #$A9,$5(A1)` at `$2E2D9C`, making PB3 among the outputs), PB3 drives low and the decode swaps SRAM in for the ROM alias. There is no "disable overlay" write anywhere in the image [1]. Which physical signal actually gates the overlay — PB3 itself, or decode logic keyed to PB3's output state — is not settled (§6).

### 3.2 The OR-write DRAM alias

`$600000`–`$7FFFFF` reads DRAM normally, but a write **ORs the data into memory** instead of replacing it. This is a rasteriser accelerator: compositing a glyph or a fill into an existing page bitmap becomes a plain write instead of a read-modify-write, roughly halving the bus traffic of the inner blit loop.

The firmware treats the alias as part of the architecture, not a trick [1]:

- The POST explicitly verifies it (LED `$A`, routine `$2E35EA` at `$2E2F40`), and the memory-size table at `$2E38BE` records `$600000` as the top of RAM with `$400000` as the alias base to cross-check.
- System init sets the **supervisor stack pointer to `$600000`** (`$2E0006`): the supervisor stack lives in the aliased window, growing down from its base, so pushes land in real DRAM through the alias. A push writes a full longword to previously-cleared memory, where OR and MOVE are equivalent.

### 3.3 The LED / engine-control latch (`$800000`)

One write-only 8-bit latch at `$800000`, plus the engine FIFO port at `$800001` [1][3]:

| bit | name (inferred) | direction | meaning |
|---|---|---|---|
| 7–4 | LED 4–1 | out | the four diagnostic LEDs — the "upper nibble" LED codes of §5.2 |
| 3 | CPRDY | out | controller ready |
| 2 | VSYNC | out | vertical sync to the engine |
| 1 | PRNT | out | print |
| 0 | CBSY | out | controller busy |

The bit names for bits 3–0 are *inferred — unverified* (from engine-interface conventions); the bit positions, the write-only nature, and the LED nibble are ROM-evidenced [1]. Because the latch cannot be read back, the firmware keeps a **RAM shadow at `$41D530`** and always writes the whole byte from it — no read-modify-write exists anywhere [1]. System init seeds the shadow with `$08` (bit 3, controller-ready, asserted) at `$2E094C`; all seven runtime writers (`$2D919E`, `$2D91BE`, `$2D91E0`, `$2D921E`, `$2D92C8`, `$2D930A`, `$2D9382`) are `MOVE.B $41D530,$800000` [1]. A model that lets software read `$800000` back implements a port the hardware does not have; the firmware never tries [1].

### 3.4 The SCC windows: split read/write decode

The Z8530 is addressed through **two windows: reads from `$A00000`, writes to `$C00000`**, with the four byte ports of each window repeating every 8 bytes [1]. Note the parity flip — reads on even addresses, writes on odd:

| function | read | write |
|---|---|---|
| channel B control | `$A00000` | `$C00001` |
| channel A control | `$A00002` | `$C00003` |
| channel B data | `$A00004` | `$C00005` |
| channel A data | `$A00006` | `$C00007` |

The odd-parity write window is what frees the even bytes of `$C00000`–`$C003FF` for the EEPROM (§3.5). The firmware builds one driver structure per channel and stores the port addresses in them at `$2E1540`: channel A (control read `$A00002`, control write `$C00003`) anchored at `$41D558`, channel B (`$A00000`/`$C00001`) at `$41D55C` [1].

### 3.5 The EEPROM: even bytes only

A 512-byte X2804 occupies **1 KiB of address space** because it sits on the even bytes: EEPROM byte *n* lives at `$C00000 + 2n`. The ROM does not hard-code the doubling — it keeps a stride flag at `$416414` and shifts the index left by one when set (`$2D8F2E`) [1]. The odd bytes of the same range are SCC writes (§3.4): `$C00000` and `$C00001` are entirely different devices.

Access is wrapped in four routines [1]:

| routine | signature |
|---|---|
| `$2D8EB0` | `read_byte(offset) -> D0` |
| `$2D8EEE` | `write_byte(offset, value)` |
| `$29DBF4` | `read_long(offset) -> D0`, big-endian from `offset..offset+3` |
| `$29DC2A` | `write_long(offset, value)`, big-endian |

Writes are heavily guarded: interrupts off, **VIA PB3 asserted**, a single byte stored, PB3 released, interrupts on, with a ~20-tick settle wait either side taken from the millisecond counter at `$41D54C` and timestamped into `$41661C` (`$2D8F12`–`$2D8F4E`) [1]. Reads normally do not touch the device at all: init reads the EEPROM once, then repoints the base pointer at `$416410` from `$C00000` to a RAM shadow at `$41641C` (`$2D8FF6`), so all later reads come from the copy [1]. The PB3 strobe is the X2804's write-enable — see §4.4.

### 3.6 The VIA: odd-byte registers

VIA register *n* is at `$E00001 + 2n`, byte-wide on odd addresses — confirmed by every access the ROM makes [1]. `$E00000` serves as a base from which the POST addresses register *n* at offset `1+2n`.

| addr | reg | name | ROM usage [1] |
|---|---|---|---|
| `$E00001` | 0 | ORB/IRB | init `$80`; PB7 set/cleared per scanline; PB5 pulsed to reset the engine FIFO; PB3 pulsed around EEPROM writes |
| `$E00003` | 1 | ORA/IRA | — |
| `$E00005` | 2 | DDRB | `$A9` = PB0, PB3, PB5, PB7 outputs |
| `$E00007` | 3 | DDRA | `$05` = PA0, PA2 outputs (ADB ST1/ST2) |
| `$E00009` | 4 | T1C-L | `$B8` ┐ latch `$08B8`, free-running |
| `$E0000B` | 5 | T1C-H | `$08` ┘ |
| `$E00011` | 8 | T2C-L | `$0A` — armed at end of a scanline burst |
| `$E00013` | 9 | T2C-H | `$00` |
| `$E00017` | 11 | ACR | `$68` = T1 continuous interrupts with **PB7 output disabled**, T2 counts pulses on PB6 |
| `$E00019` | 12 | PCR | `$60` = CA1 interrupt on the **falling** edge (SCC `/INT` is active low); CB2 an independent interrupt input |
| `$E0001B` | 13 | IFR | bit 1 (CA1 = SCC) tested on every interrupt; bit 5 (T1) on the video path |
| `$E0001D` | 14 | IER | `$20` (bit 7 clear = *disable* T2), then `$C2` (bit 7 set = *enable* T1 + CA1); `$02` while a blit is in progress; `$C2` on IRQ exit |
| `$E0001F` | 15 | ORA no-handshake | all port-A reads go through here |

ACR `$68` leaves the VIA's own PB7 output **disabled**, which is why the video code drives PB7 by hand through ORB on every scanline rather than letting T1 toggle it — the two facts corroborate each other [1]. The port pin assignments follow; the *names* of the print-engine handshake lines are inferred from convention, while bit numbers, directions and every use cited are ROM-evidenced [1].

Port A:

| bit | name (inferred) | dir | ROM usage [1] |
|---|---|---|---|
| 0 | ST1 | out | ADB |
| 1 | print SBSY | in | polled at `$2E04BA`, `$2E0542`, `$2E05B2` |
| 2 | ST2 | out | ADB |
| 3 | ADB /INT | in | ADB transceiver interrupt |
| 4 | print VSREQ | in | polled with timeout at `$2E07F6`, `$2E0894` (§4.6) |
| 5 | switch 1 | in | front panel (§4.7) |
| 6 | switch 2 | in | front panel (§4.7) |
| 7 | print STATUS/COMMAND | in | engine status |

Port B:

| bit | name (inferred) | dir | ROM usage [1] |
|---|---|---|---|
| 0 | — | out | (DDRB output; purpose not identified) |
| 1 | print RDY | in | engine ready |
| 2 | print PPRDY | in | read back by the POST's `$800000` loopback check (§5.2, LED `$8`) |
| 3 | overlay / EEPROM /WE | out | drops the overlay when first made an output; pulsed around EEPROM byte writes (§3.1, §4.4) |
| 4 | SCC W/REQ | in | SCC wait/request |
| 5 | reset FIFO | out | pulsed at `$2E0B98` before each scanline burst |
| 6 | T2 clock (MROCLK) | in | the engine byte-rate clock, counted by T2 |
| 7 | VBL | out | set at `$2E0B68`, cleared at `$2E0B86` per scanline |

CB1/CB2 carry the ADB clock and data lines to the 342-0440-A transceiver — *inferred — unverified* for the pin assignment; the transceiver's presence and the port-A ST1/ST2 outputs are board-survey and ROM facts [1][3].

### 3.7 The debug aperture (`$F80000`)

The very first thing the POST does after clearing its registers — before the VIA, the LEDs, the overlay or any self-test — is probe `$F80000` [1]:

```
read  $F80000 as a longword ; if it != $AAAA5555, continue booting
MOVEM.L $F80000,D0/A0       ; second read supplies an argument and a vector
JMP (A0)                    ; the pod owns the machine
```

A bring-up pod that answers the magic value and then supplies `D0`/`A0` on the second read takes control with nothing initialised and nothing tested — the cleanest possible entry point ahead of the self-test. With no pod, the read returns something else and the probe falls through [1].

## 4. Per-subsystem wiring

### 4.1 CPU and interrupts

**Everything interrupts at level 1.** The SCC's `/INT` output is wired to VIA CA1, so the VIA is the only interrupt source the CPU sees, and autovector 1 is the only vector that matters at run time [1]. Cold boot fills exception vectors 2–255 with `$20000C`, the reset entry, so *any* unexpected fault reboots the printer rather than hanging it (`$2E00A0`) [1]. Vector 25 (autovector 1, at `$0064`) is then pointed at the dispatcher `$2E0E3C`, and vector 5 (divide by zero, `$0014`) at `$2E2778` [1].

The dispatcher [1]:

1. Read VIA IFR (`$E0001B`); test bit 1 (CA1 = SCC).
2. Not set → software tick path: a divide-by-10 counter at `$416C5E`, which on underflow still polls the SCC as a missed-interrupt guard.
3. Set → acknowledge CA1, write `$02` to SCC WR0 channel B to select RR2, read the vector from `$A00000`, mask with `$0E`, and use it as a byte index into an 8-entry jump table at `$416BF4` (×2 for longword entries).
4. After the handler, write `$03` to WR0 channel A (reset highest IUS) and poll `$A00002 & $3F` for anything still pending, looping if so.
5. Restore, write `$C2` to VIA IER, `RTE`.

The `$416BF4` table follows the Z8530's status-affects-vector RR2 encoding exactly [1]:

| RR2 & `$0E` | table slot | source |
|---|---|---|
| `$0` | `$416BF4` | ch B Tx buffer empty |
| `$2` | `$416BF8` | ch B external/status change |
| `$4` | `$416BFC` | ch B Rx character available |
| `$6` | `$416C00` | ch B special Rx condition |
| `$8` | `$416C04` | ch A Tx buffer empty |
| `$A` | `$416C08` | ch A external/status change |
| `$C` | `$416C0C` | ch A Rx character available |
| `$E` | `$416C10` | ch A special Rx condition |

All eight slots start as a bare `RTS` at `$2E013A`; the serial open routine at `$2E144C` patches in the real handlers, keyed on a device ID of `$09` (channel B slots) or `$19` (channel A slots) [1]. During page transfer the video code **swaps the vector**: `$2E0810` installs `$2E0E50` at `$0064` and sets IER to `$02`, so scanline timing runs off VIA T1 instead of the serial path, then restores afterwards [1].

### 4.2 ROM

The 1 MiB image is eight 128 KiB chips in four high/low pairs, 16-bit wide [3]. Its layout divides cleanly [2]:

| ROM offset | CPU address | size | contents |
|---|---|---|---|
| `$00000`–`$9D3FF` | `$200000`–`$29D3FF` | 629 KiB | PostScript VM image — tokenised dictionaries, name/error string pool (densest ASCII at `$298000`–`$29A7FF`), 35-face font outline data; **no 68000 code** |
| `$9D400`–`$E38E3` | `$29D400`–`$2E38E3` | 281 KiB | 68000 executable — the PostScript interpreter plus the low-level machine layer (boot, POST, monitor, drivers) in the last ~10 KiB from `$2DFFF0` up |
| `$E38E4`–`$FFFDB` | `$2E38E4`–`$2FFFDB` | 114 KiB | unused, all `$00` |
| `$FFFE0`–`$FFFFF` | `$2FFFE0`–`$2FFFFF` | 32 B | per-chip checksum table |

The **checksum table** is eight big-endian longwords: the plain 8-bit sum of each physical chip's 131072 bytes, accumulated into a 32-bit register, with the table region itself excluded from the sum — verified two ways against the chip dumps (the first six entries equal the raw chip sums; entries 6–7 differ by exactly the table's own bytes in each lane) [2]. The bank/chip pairs are 342-0546/342-0545 (bank 0), 342-0548/342-0547 (bank 1), 342-0550/342-0549 (bank 2), 342-0552/342-0551 (bank 3), high chip first in each pair [2]. The POST recomputes all of it at `$2E3740` (§5.2, LED `$F`). The Adobe/Linotype copyright notice appears five times, each **byte-duplicated across both ROM lanes** (`$29D33A`, `$2AA56E`, `$2BE476`, `$2CFF72`, `$2E2822`), so the notice is physically present in each chip's mask, not merely in the assembled image [2].

### 4.3 DRAM and SRAM

The 2 MiB of DRAM is 16 × 511000 [3], addressed at `$400000` with the OR-write alias at `$600000` (§3.2). The ROM's memory-size support is a single-record table at `$2E38BE` — top of RAM `$600000`, alias base `$400000`, wrap target `$600000`, end marker `$08070605` — because the II NT shipped in exactly one configuration; the sizing routine at `$2E37F4` writes `$A5B4C3D2` at the top and checks the record's claimed alias address follows [1]. The supervisor stack is based at `$600000`; the heap is `$400000`–`$41D560`, zeroed at boot, its limit published to `$416F08` and the stack base to `$416F30` [1].

The 2 KiB SRAM (2 × Am9128-10 [3]) appears at `$000000` once the overlay is off, mirrored in the low window (§3.1); the exception vector table and the POST's scratch live there.

### 4.4 EEPROM: layout and write protocol

Byte *n* of the 512-byte X2804 lives at `$C00000 + 2n` (§3.5). The write protocol around every single byte is [1]:

```
wait ~20 ticks ; disable interrupts
ORI.B  #$8,(A4)      ; VIA PB3 high   — write-enable
MOVE.B D6,(A5)       ; one byte to the EEPROM
ANDI.B #$F7,(A4)     ; PB3 low        — write-protect again
enable interrupts ; wait ~20 ticks
```

**PB3's steady-state role is the EEPROM write-enable, not the ROM overlay.** Toggling the ROM overlay around each EEPROM byte would be meaningless — and actively harmful, since the interrupt-enable sequence that follows runs with the stack in DRAM — whereas a write-enable/protect strobe with settle delays either side is precisely the X2804's requirement [1]. This is *inferred — unverified* from this one call site; what is certain is that (a) the overlay does drop when DDRB is first written, so PB3's output state governs boot-time banking, and (b) PB3 is toggled around EEPROM byte writes and nowhere else in the firmware [1]. A model that re-banks ROM over low memory on every PB3 rising edge will corrupt `setpassword`, `setmargins` and page-count updates. Whether the pin is shared, or the overlay attribution is simply wrong, is §6.

The ROM's `statusdict` operator table (name→code bindings at `$29E87C`–`$29E973`, 27 entries, name pool at `$29E9A4` [2]) pins the stored configuration layout exactly [1][2]:

| offset | size | field | bound operator(s) |
|---|---|---|---|
| `$080` | 4 | signature `$7A53DA71` — validity marker | init (`$2D8FC8`) |
| `$0A4` | 4 | default manual-feed timeout, stored with a −60 s bias | `defaulttimeouts` |
| `$0A8` | 4 | margin 1 | `margins` / `setmargins` |
| `$0AC` | 4 | margin 2 | `margins` / `setmargins` |
| `$0B0` | 1 | print-start-page flag | `dostartpage` / `setdostartpage` |
| `$0B1` | 4 | password | `checkpassword` / `setpassword` |
| `$0B5` | 4 | default job timeout | `defaulttimeouts` |
| `$0F9` | 1 | idle-font state | `idlefonts` / `setidlefonts` |
| `$190` | 4 | default wait timeout | `defaulttimeouts` |
| `$194` | 1 | page type | `pagetype` / `setpagetype` |
| `$1AD` | 1 | a boolean, read inverted at `$2D909E` — purpose not identified (§6) | — |

Fields are packed without alignment (`$B0` is one byte; `$B1` immediately starts a longword). Timeouts are validated against 15 … `$20C49B` seconds — `$20C49B` = 2 147 483 is exactly 2³¹/1000, the largest value that still fits a signed 32-bit millisecond count; out-of-range values read back as 0 (job/wait) or 60 (manual feed) [1]. Seven `statusdict` entries bind to RAM variables rather than code: `jobname $400044`, `product $40004C`, `jobsource $400034`, `jobstate $40003C`, `manualfeed $400024`, `manualfeedtimeout $40002C`, `waittimeout $416D40` [2].

### 4.5 Serial: the Z8530 SCC

Both channels are built symmetrically by the driver (structures at `$41D558`/`$41D55C`, §3.4), with RS-422 drivers and receivers (26LS30/26LS32) [3]. Which physical connector is channel A and which is channel B is not identified by the firmware (§6); the diagnostic monitor runs on **channel B**.

The monitor's channel-B init at `$2E351E` is table-driven from `$2E354E` (`$FF`-terminated register/value pairs) and is the cleanest record of how the SCC is clocked [1]:

```
WR9=$C0  force hardware reset        WR14=$00  BRG off while loading
WR15=$00 no external/status IRQs     WR12=$0A  BRG time constant low ┐ TC=10
WR4=$4C  x16 clock, 2 stop, no par.  WR13=$00  BRG time constant high ┘
WR11=$50  Rx clock = BRG, Tx = BRG   WR14=$01  BRG enable
                                  →  WR10=$00  NRZ
                                      WR3=$C1  Rx 8 bits/char, Rx enable
                                      WR5=$6A  Tx 8 bits, Tx enable, RTS
```

i.e. 9600 baud, 8-N-2, polled. A time constant of 10 at ×16 gives 9600 baud from a 3.6864 MHz PCLK — the arithmetic that pins the SCC clock rate (*inferred — unverified*, §2.1). The POST's SCC presence check is a channel-A RTS → channel-B CTS loopback through the cable-side wiring: assert RTS in WR5, read RR0 ch B bit 5, drop RTS, confirm it clears [1]. Every register access in the ROM's SCC code is followed by a `NOP` for the SCC's inter-access recovery time [1].

### 4.6 Print-engine interface

The controller talks to the Canon LBP-SX engine over the VIA handshake lines (PA1, PA4, PA7, PB1, PB2; names inferred — §3.6) plus the byte FIFO at `$800001`, backed by the 67L401 pair [1][3]. Control bits CPRDY/VSYNC/PRNT/CBSY go out through the `$800000` latch (§3.3); PB5 resets the FIFO; PB7 is the scanline strobe, driven by hand because ACR `$68` disables T1's PB7 output; T2 counts the engine's byte-rate clock on PB6 [1].

**Per-scanline path** (`$2E0B60`, called from the video interrupt) [1]:

1. Set VIA PB7 (VBL) — `ORI.B #$80,$E00001`.
2. Decrement the line counter at `$416C40+$1C`.
3. Lines remaining: set PB5 (reset FIFO), then a computed jump (`JMP *+$44(D3.W*1)` at `$2E0BAE`) into an **unrolled blitter** that pushes bytes to `$800001`. The selector word comes from a descriptor the setup routine `$2E0A58` builds at `$416C34`: scanline entry point `$2E0B60`, the caller's buffer pointer, the FIFO address `$800001`, and two words derived from the page width — `-(width mod 16)` and a value from a table at `$2E0BF8` indexed by `width & 15` — which select the unrolled entry and handle the partial trailing byte.
4. Page done: load VIA T2 with `$000A` (`$E00011`/`$E00013`), clear PB7, exit through `$2E0E30`.

**Page start** (`$2E07E8`): poll PA4 (VSREQ) against a deadline built from the tick counter (`$2E01B4` computes now+delta, `$2E01C0` tests it). When VSREQ asserts, set VIA IER to `$02` and install `$2E0E50` as autovector 1 for the duration of the transfer (§4.1), so each scanline is paced by VIA T1 interrupts rather than the serial path [1].

### 4.7 Front panel: switches and LEDs

Two switches read on VIA PA5 and PA6; the ROM decodes them at `$2E0A2C` [1]:

```
value = 0
if PA5 then value += 2
if PA6 then value += 1
```

| value | PA6 | PA5 | mode |
|---|---|---|---|
| 0 | 0 | 0 | serial batch, 1200 baud |
| 1 | 1 | 0 | serial batch, 9600 baud |
| 2 | 0 | 1 | Diablo emulation, or Executive mode if a "special switch" flag is set |
| 3 | 1 | 1 | AppleTalk |

The shipped switch position (the middle pair, value 2) is *inferred — unverified*. The four diagnostic LEDs are bits 7–4 of the `$800000` latch and double as the POST's progress display (§5.2) [1].

## 5. Boot sequence

```
reset ──► $20000C ──► $2DFFF0 ──► [$2FFFDC] = $2E2D4E ──► POST ──► $2E0002 ──► PostScript
          (vectors)    (trampoline)  (POST entry)         (LED codes)  (system init)  ($2A008E)
                                                                │ any test failed
                                                                ▼
                                                    $2E2FE8 serial diagnostic monitor
                                                    (SCC ch B, 9600 8-N-2)
```

### 5.1 Reset entry

While the overlay is asserted, the 68000 fetches its vectors from the ROM's first bytes [1][2]. Only three longwords exist: SSP `$00001000`, PC `$0020000C`, and a zero bus-error vector; the rest of ROM page 0 is PostScript data, and the real vector table is built in SRAM later [2]. The reset stub at `$20000C` is three instructions: `MOVE #$2700,SR` (supervisor, all interrupts masked), `MOVEA.L #$1000,A7`, `JMP $2DFFF0` [1].

`$2DFFF0` is a trampoline: it loads the POST entry pointer stored at `$2FFFDC` (value `$2E2D4E`), plants `$2E0002` in A7 as the "POST finished" continuation, and jumps — one level of indirection so the POST entry can move without touching the reset stub. A `BEQ` immediately after the pointer load is a null-pointer guard, never taken in this build [1].

### 5.2 Power-on self test

The POST (`$2E2D4E`–`$2E38E3`) runs register-convention discipline throughout [1]:

- **D7** carries state — bits 31–16 flags, bits 15–8 the current LED code, bits 7–0 the current test ID (reported by the monitor's `R` command).
- **D6** is the error accumulator; any failing test ORs a non-zero value into it, and the caller branches to the diagnostic monitor on non-zero.
- Calls use the `LEA <ret>,A6` / `JMP <test>` / `JMP (A6)` idiom rather than `JSR`/`RTS`, **because the stack pointer is itself under test**.

The very first act (after clearing D6/D7, still with the overlay on and SP at `$2000`) is the `$F80000` debug-pod probe of §3.7 — ahead of the VIA, the LEDs and every self-test [1]. Then:

| LED | stage | routine | what it does [1] |
|---|---|---|---|
| `$8` | VIA + SCC presence | inline from `$2E2D84` | programs DDRA (`$05`), writes ORA-no-handshake; **DDRB ← `$A9`, which drops the overlay** (§3.1); LED `$80` out; loopback-checks the `$800000` latch by driving bit 7 both ways and reading it back on VIA PB2; resets the SCC (WR9 `$C0`) and runs the RTS→CTS loopback check (§4.5) |
| `$F` | ROM checksum | `$2E3740` | per-chip byte sums vs the table at `$2FFFE0`; walks `$200000` upward in 256 KiB bank steps, end clamped to `$2FFFE0`, summing the two byte lanes of every word separately, `EOR` against the table — one error nibble per bank lands in D6 (bit 0 = high chip, bit 1 = low chip of the pair) [1][2] |
| `$E` | DRAM low half | `$2E35C8` | walking ones/zeros: 256 rotations of `$00000001`/`$FFFFFFFE` written as a `MOVEM.L D0-D1` pair at `$400000`, residue ORed into D6 |
| `$D` | top-of-RAM probe | `$2E35BC` | returns the constant `$600000` (this build hardwires the shipped size); the caller adopts it as the new A7, then re-tests the top 8 bytes with `$2E35C8` |
| `$8` | DRAM high half | `$2E35C8` | same walking-bit test at `A7-8` |
| `$C` | DRAM march, 1st MiB | `$2E35EA` | 3-pattern fill + EOR-propagation march (below) |
| `$B` | DRAM march, 2nd MiB | `$2E35EA` | same, upper half, clamped to top of RAM |
| `$A` | alias check | `$2E35EA` | verifies `$600000` mirrors `$400000` (§3.2) |
| `$9` | DRAM sizing | `$2E37F4` | matches top-of-RAM against the config table at `$2E38BE`, probing the alias with `$A5B4C3D2` (§4.3) |
| `$8` | SRAM | `$2E35EA` | march over `$000000`–`$000FFF` — the 2 KiB chip twice through the mirror |
| `$0` | success | — | installs the default vector image and jumps to `$2E0002` |

The march test fills with the 12-byte triple `6DB6DB6D B6DB6DB6 DB6DB6DB` — a repeating 3-bit `011` pattern, the classic choice for catching coupling faults between adjacent DRAM cells — then runs two EOR-propagation passes (upward at `$2E3642`, mirrored in a descending variant at `$2E3696`) and compares the tail against the pattern, offset by the region's 0/4/8/12-byte remainder [1].

If any stage sets D6, control drops to `$2E2FE8` and the serial diagnostic monitor (§5.3). If the VIA/SCC presence check flagged the control path dead (flag bit 26), the monitor is entered with the distinctive "no serial available" code `$87654321` in D6 instead [1]. LED `$0` is written **only on success**, immediately before the jump to system init [1].

### 5.3 The serial diagnostic monitor

`$2E2FE8`–`$2E3567` is an interactive command monitor on SCC channel B at 9600 8-N-2 (init table of §4.5), reached whenever a POST stage fails. Commands are introduced by `*` and dispatched by a flat `CMPI.B` chain at `$2E3040` [1]:

| cmd | action |
|---|---|
| `S` | show/clear status — clears flag bits 16/22/30 and writes D7's LED byte to the latch |
| `L` | set a 32-bit working address into A4, echo it |
| `B` | set a byte count into D4, echo it |
| `D` | deposit `D4`+1 bytes at `(A4)+`, accumulating a checksum in D6 |
| `C` | compare: read a longword, EOR into D6; reports `G` (good) or `N` |
| `G` | go — read an address into A3, echo `*G`, jump with A6 as return |
| `0`/`1` | set A0 / A1 |
| `2`/`3` | set / clear the "verbose echo" flag |
| `4` | clear D6 and D7 (reset error state) |
| `5` | re-initialise the VIA timer path (`$2E3568`) |
| `6` | assert the 68000 RESET line |
| `A`/`H` | select ASCII-hex vs raw-binary number entry (flag bit 20) — the monitor is drivable by a terminal or by a host program pushing raw bytes |
| `R` | report: echo D6 (32-bit) then D7's low byte |
| `E` | set the "echo results" flag (bit 21) |
| `M` | vestigial — the compare result is discarded; a removed command whose `CMPI.B` was left behind [1] |
| `I` | re-enter POST from the top (`JMP $2E2D4E`) |
| `T` | run test *n* from the 8-entry table at `$2E38D4`, USP as repeat counter |

The `T` tests [1]: 0 top-of-RAM probe (`$2E35BC`), 1 walking ones/zeros (`$2E35C8`), 2 ascending march (`$2E35EA`), 3 sizing/alias probe (`$2E37F4`), 4 ROM checksum (`$2E3740`), 5 descending march (`$2E3696`), 6 `$00`/`$FF` march (`$2E37A2`), 7 always-pass stub (`$2E38CE`). Character I/O is polled: receive at `$2E33C8` (RR0 bit 0, RR1 error bits reset on parity/overrun/framing), transmit at `$2E34F0` [1]. An unattended failed boot shows the top LED bit oscillating: that is this monitor idling with no terminal attached [1].

### 5.4 System initialisation (`$2E0002`)

On POST success, the 256-byte default vector image at `$2E2B60` is copied to `$000000` first. In this build only entry 0 is ever meaningful (SSP `$00002000`); every handler entry points at `$200100`–`$2001EE`, which here hold PostScript VM data, not code — a leftover from an Adobe link map that placed per-vector stubs at ROM offset `$100`. It is harmless because the steps below overwrite vectors 2–255 immediately [1][2]. Then [1]:

1. `MOVE #$2700,SR`; `A7 = $600000` — the supervisor stack lives in the OR-write alias (§3.2).
2. `CLR.B $800000` — LEDs off, engine control lines low.
3. VIA setup: DDRB `$A9`, DDRA `$05`, ORB `$80` (PB7 high, PB3 low — overlay stays off / EEPROM write-protected), ACR `$68`, PCR `$60`, IER `$20` then `$C2`, T1 latch `$08B8` (§3.6).
4. Zero DRAM `$400000`–`$41D560`; publish heap limit `$41D560` → `$416F08` and stack base `$600000` → `$416F30`.
5. **Cold boot only** (warm-boot marker at `$416C18` zero): fill exception vectors 2–255 with `$20000C` — every unexpected fault reboots the printer. A warm boot keeps the table and instead carries two longwords from `$400054` to `$416C0C` across the restart.
6. Install autovector 1 (`$0064`) ← `$2E0E3C` (the dispatcher, §4.1); fill the 8-entry SCC dispatch table at `$416BF4` with the `RTS` stub `$2E013A`.
7. Vector 5 ← `$2E2778`; device and heap init (`$2DDA16`, `$2DA4CE`, `$2E036C`, console open `$2DD6D8(1)`, numeric runtime `$2DC97C`); `MOVE #$2000,SR` — interrupts on.
8. `$2E0900` registers two periodic tick callbacks (a 1 000 000-unit one at `$2E02A4`, a 1000-unit one at `$2E0288`).
9. `JSR $2A008E` — enter the PostScript interpreter; the printer is ready.

## 6. Open questions

- **VIA PB3: overlay, EEPROM write-enable, or both?** The overlay provably drops when DDRB is first written (PB3 becomes an output driving low, `$2E2D9C`), and PB3 is provably pulsed around every EEPROM byte store and nowhere else (`$2D8F30`/`$2D8F3C`). The steady-state write-enable reading fits the X2804's requirements; a schematic or board trace is needed to settle whether the pin is shared or the overlay is gated elsewhere.
- The SCC clock crystal: 3.6864 MHz is implied by the monitor's baud arithmetic (§4.5) but not confirmed from the board.
- The engine byte-rate clock on VIA PB6 (T2's pulse source): frequency not established; one derivation puts it near 116 kHz from the print engine's ~1.86 MHz video clock, unverified against a primary source.
- Mirror extents: how far each region of §3 mirrors upward (the decode logic is not traced); the mapping of `$300000`–`$3FFFFF`, which the ROM never touches; the SRAM mirror period (an `$800` period is inferred; reads above `$040000` in the low window are unverified).
- The VIA's clock rate, hence the real-time period of the T1 latch `$08B8`, the tick calibration behind the "millisecond" counter at `$41D54C`, and the engine timing the scanline interrupt pacing implies.
- EEPROM offset `$1AD` (a boolean read inverted at `$2D909E`): purpose unidentified.
- Which physical connector (serial vs AppleTalk port) is SCC channel A and which is channel B; the firmware builds both channels symmetrically and only the monitor's use of channel B is a hint.
- The ADB bit-bang protocol to the 342-0440-A transceiver, around `$2E0B2C`: not decoded.
- The front-panel switches' shipped default position (value 2 inferred from the mode table's middle setting, not verified on a physical machine), and the "special switch" flag that selects Executive mode over Diablo emulation.
- The engine-side electrical and timing contract beyond what the ROM polls (PA1/PA4/PA7, PB1/PB2 handshake sequencing): no engine-side documentation was available for this page.

## References

1. LaserWriter II NT ROM disassembly — annotated boot, POST, diagnostic-monitor, system-init and interrupt-dispatcher listing; 1 MiB ROM image, SHA-1 `efa102a84be8dfea08931d0b550af2782cdbc42c` (addresses cited in text as `$xxxxxxxx`).
2. LaserWriter II NT ROM image analysis — layout, per-chip checksum table, `statusdict` name→code bindings and font/string survey; same 1 MiB image as [1].
3. LaserWriter II NT logic-board chip survey — Apple board 640-4105 (© 1987) part inventory and crystal observations.
