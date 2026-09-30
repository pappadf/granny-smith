# The Lisa floppy-disk controller

**Contents:**

1. [Overview](#1-overview) — what the part is, the two board generations, division of labor, addressing, clocking
2. [Register file](#2-register-file) — the 68000-visible window, the I/O block, shared constants, status block, the 6504's private I/O space
3. [Behaviour](#3-behaviour) — command handshake, the main loop, bus lockout, interrupts, seek/recalibrate, spindle speed, GCR and the sector format, retries
4. [Programming model](#4-programming-model) — GoByte commands, RWTS parameters, interrupt protocol, the boot ROM's boot sequence, the loader and the OS driver
5. [Quirks & errata](#5-quirks--errata)
6. [Open questions](#6-open-questions)

---

## 1. Overview

### 1.1 What the part is

The Lisa's floppy-disk controller is not a register-level disk chip like a
Western Digital FD1771 or an Apple IWM that the 68000 programs directly. It is a
**self-contained intelligent coprocessor**: a **6504A 8-bit microcomputer** with
4 KB of private program ROM, plus a **1 KB buffer RAM shared with the 68000**,
living on the I/O board [1] §2.5.1 p. 2-11, §6.2.1 pp. 6-4c – 6-5b. The 68000
never touches a drive signal; it writes a **command block** into the shared RAM,
strokes a single "go" byte, and the 6504 performs the entire sector-level
operation — head positioning, spindle-speed control, GCR encoding and decoding,
header search, checksum creation and verification — and reports completion by
raising an interrupt line back to the processor [1] §6.3 pp. 6-31b – 6-36.

The controller's own firmware exists in two documented generations matching the
two I/O-board generations:

| Generation | Machine | Drives | Coprocessor firmware | Evidence |
|---|---|---|---|---|
| Lisa 1 I/O board (1983) | Lisa 1 | two 5.25″ "Twiggy" drives | Twiggy I/O ROM | [1] Chapters 6, 9 (all Twiggy parameters); the Boot ROM's old-Twiggy conditional paths [3] |
| Lisa 2 / Macintosh XL I/O board | Lisa 2/5, Lisa 2/10, Macintosh XL | one Sony 3.5″ microfloppy (double-sided-capable mechanism) | I/O ROM **version 88**, the "Sony Driver for Lisa" | [4], [5]; the self-test displays the pairing "H/88" — Boot ROM rev H, Floppy ROM 88 [2] |

The hardware manual (April 1983) documents the first generation in detail — its
chapter 6 walk-through describes the Twiggy data path — while the released I/O
ROM 88 source [4] and its December 1983 assembled listing [5] document the
Sony-generation firmware that actually shipped in every Lisa 2 and Macintosh
XL. The two describe the same controller skeleton (6504A, private ROM, shared
RAM, addressable latch bank, timing counter) with a different drive interface on
the far side: the Twiggy board's custom data-separator state machine ([1]
§6.2.4 pp. 6-22e – 6-24) versus the Sony board's IWM-class separator with Q6/Q7
softswitches and CA0–CA2/LSTRB control lines to the drive's monitor PAL [4]
(`Interface`, `VAR` modules). This page covers the controller as a whole, using
[1] for the board hardware and timing, [4]/[5] for the Sony-generation register
semantics and behaviour, and flags generation-specific facts as such.

The Lisa 1's twin Twiggy drives are a different machine ([1] ch. 9) and are
covered here only where the same controller hardware and command set serve them;
the Sony 3.5″ mechanism of the Lisa 2 / Macintosh XL is this page's subject.

### 1.2 The part's place in the I/O board

Physically, the controller occupies sheet 4 of I/O board schematic 050-4008 [1]
§6.2 p. 6-2a. Its documented components are [1] §6.2.1 pp. 6-4c – 6-5b, §6.2.2
pp. 6-6a – 6-7b, §6.2.3 pp. 6-6a – 6-7b:

| Component | Location | Role |
|---|---|---|
| 6504A microcomputer | D-3 | the coprocessor CPU; 8 KB address space on internal bus MA0–MA12 |
| 4 KB program PROM (2732) | D-3 | the controller firmware, private to the 6504 |
| 2 × 1 K×4 RAM (444C class) | D-3, D-4 | the 1 KB shared buffer RAM, nibble-sliced across the two devices; battery-backed [1] §2.5.1 p. 2-11 |
| LS245 octal transceiver | D-4 | separates the controller's internal data bus from system bus BD0–BD7 |
| 3 × LS157 multiplexer | C-4 – A-4 | multiplexes the 68000's address into the RAM access path |
| LS161A 4-bit counter | B-3 | the controller's timing state machine, driven by a 16 MHz oscillator at B-3 |
| LS259 addressable latches | B-1, C-2 | the drive control lines: motor, drive select, head/phases, and the DIS / FDIR / DSKDIAG housekeeping lines |
| LS139 dual 2-to-4 decoder | B-2, B-3 | internal address decode |
| LS174 hex D-flop + LS323 shift register + 256×8 PROM | D-1, D-2 | the Twiggy-generation data-separator state machine [1] §6.2.4 |

The 6504A runs from the timing counter's Q2 output as its φ2 clock (§1.5). Its
8 KB address space is laid out as: 1 KB shared RAM at $0000–$03FF, its private
I/O latch space at $0800 up, and the 4 KB program ROM at $1000–$1FFF [4] (`VAR`,
`SY`, `HIMEM`: program `.ORG $1000`; reset jump vector and copyright string
"C83APPLE" at the top of ROM, the last 13 bytes below $2000) [1] Figure 6-3 p.
6-5b.

The 1 KB shared RAM is **battery-backed**: "This RAM is provided with power
backup by a battery. Parameters stored in the floppy-disk controller RAM are
therefore not lost during power down" [1] §2.5.1 p. 2-11. This is why the
64-byte system parameter block at $C181–$C1FF survives a power cycle — see
[pram.md](pram.md) §1 — and why the firmware carries a `GO AWAY` command whose
stated purpose is "to prevent an erroneous WRITE operation to parameter memory
when the Lisa is turned off" [1] §6.3.2 p. 6-35.

### 1.3 Division of labor

The controller splits the floppy work cleanly, and the split is the reason the
68000-side interface is so small:

| Concern | Owner |
|---|---|
| Boot-device selection, icon UI, command-block construction, timeout handling | the 68000 (boot ROM [3], loader [6], OS driver [6]) |
| Command parsing, parameter validation, retry and recalibration policy | the 6504 [4] (`CMD`, `READ`) |
| Head positioning, 1/8-track stepping, optical recalibration | the 6504 over the LS259 phase lines [1] §6.2.5, §6.3.5; [4] (`Interface`, `RECAL`) |
| Spindle-speed control and verification | the 6504 over the PWM counter/comparator and the drive's tach line [4] (`Npwm`, `Interface`); [1] §6.3.5 |
| GCR encoding/decoding, sector headers, checksums | the 6504 with the data-separator state machine [1] §6.3.3; [4] (`TABLES`, `NREAD16`, `WRITE16`, `CRECKSUM`) |
| Sector data transfer to/from the 68000 | the shared 524-byte I/O buffer, one sector at a time [1] Figure 6-3; [4] (`VAR`) |
| Interrupt generation (insertion, eject request, command completion) | the 6504's IST latch and IMsk gate, raised to the 68000 as FDIR [1] Figure 2-9; [4] (`LOOP`) |

Consequently the 68000-visible register surface is one command byte, a
14-parameter I/O block, a status/interrupt area, a handful of host-tunable
timing constants, and the data buffer — nothing else (§2). All raw drive state
(write-protect, disk-in-place, the eject switch) is polled by the 6504 itself
over the SNS line [1] §6.3.5.

### 1.4 Addressing: the odd-byte window

The controller occupies physical I/O space **$00C001–$00C7FF** — odd bytes only
[1] §2.5.1 p. 2-11. The Lisa's MMU maps logical **$00FCxxxx** to physical I/O
space, so driver sources use the logical form (`$FCC001`); the hardware manual's
print tables use the physical form (`$00C001`); a 1985 Apple errata corrects the
manual's I/O-space chapter to the `$FCxxxx` convention [2]. They are the same
bytes — see [lisa.md](lisa.md) §3.3 for the segment mapping.

The odd-byte-only arrangement is the 6504's doing: the 6504 is an 8-bit machine
with a 13-line address bus, and the I/O board maps its 1 KB RAM so that 6504
address $n appears to the 68000 at odd address

> 68000 address = $C001 + 2·n  (equivalently $FCC000 + (2n+1))

Every 68000 access to controller RAM therefore uses byte accesses or `MOVEP`
(which strides two bytes per operand byte) — the Boot ROM builds the entire
command block with one `MOVEP.L` [3] (`TWGREAD`), and the OS driver transfers
512-byte sectors with `MOVEP` loops [6] (`SOURCE-SONYASM`, `FINISH_READ`). The
even bytes in $C000–$C7FF are not backed by the controller's RAM.

The mapping also fixes the famous addresses: the 6504's zero-page locations $18,
$2F and $C0 become the 68000's $FCC031 (controller ROM id), $FCC05F (interrupt
status) and $FCC181 (parameter memory) — [4] (`VAR`) names them exactly this way:
`ROMIDNUM` "($0018/FCC031)", `IST` at $2F, and "RAM from 'C0' to 'FF' is used by
the 68K as parameter memory".

### 1.5 Clocking and timing generation

The controller runs **asynchronously to the 68000**. Its timing is generated by a
16 MHz oscillator at B-3 driving an LS161A 4-bit counter; the counter's P inputs
are reloaded at rollover with either 8 (system loop, when the 68000 is permitted
to access the RAM) or 9 (local loop, when the controller has the bus locked out
to itself) [1] §6.2.3 pp. 6-6a – 6-7b, Figure 6-4. The counter outputs each have
one job:

| Output | Role |
|---|---|
| Q0 | clocks the disable (DIS) flop pair; the second flop adds a stable period to the asynchronous clocking |
| Q1 | clocks the disk data state machine; also gates the DTACK flop, halting the counter until the 68000 completes its cycle |
| Q2 | the 6504A's φ2 input — defining the processor's clock period |
| Q3 | resets the counter to its reload value |

With 16 MHz and an 8-state system loop, the 6504's φ2 runs at **2 MHz (0.5 µs
cycle)** — the firmware's timing comments assume exactly this ("The following
codes assume a cycle time of 0.5 uSec" [4] `HIMEM`/`WAIT`). The data state
machine steps at 250 ns per state; its WRITE and WRITE LOAD programs are 16
states each, i.e. **two 2 µs bit times**, and the 6504's write loops are paced
so that exactly eight bit times elapse between shift-register loads [1] §6.3.5
pp. 6-43a – 6-45b.


## 2. Register file

### 2.1 The 68000-visible window

The complete 68000-visible surface is the 1 KB shared RAM at odd physical
addresses $C001–$C7FF, functionally divided as follows [1] Figure 6-3 p. 6-5b;
[4] (`VAR`). 6504-side addresses (n) are given for reference; the 68000 address
is $C001 + 2n (§1.4).

| 68000 range | 6504 range | Contents | Access from the 68000 |
|---|---|---|---|
| $C001–$C01B | $00–$0D | **I/O block (IOB)** — command byte, parameters, results (§2.2) | read/write |
| $C021–$C039 | $10–$1C | **shared constants** — host-tunable timing/speed table, ROM id (§2.3) | read/write (never range-checked) |
| $C041–$C05F | $20–$2F | **status block** — drive state and the interrupt latch/mask bytes (§2.4) | read-only |
| $C081–$C0FF | $40–$7F | 6504 internal variables and error counters (§2.5) | readable; not a stable interface |
| $C101–$C17F | $80–$BF | **command-save ring** — the last eight IOBs, kept for diagnostics (§2.5) | readable |
| $C181–$C1FF | $C0–$FF | **parameter memory** — 64 bytes for the 68000, never touched by the 6504 (§2.6) | read/write |
| $C201–$C3FF | $100–$1FF | 6504 stack page and general storage (§2.5) | not for the 68000 |
| $C3E9–$C7FF | $1F4–$3FF | **I/O buffer** — the 524-byte sector transfer buffer (§2.7) | read/write while unlocked |

The hardware manual states the controller's shared area as $C001–$C7FF, the CPU
parameter-storage area as $C181–$C1FF, and "the memory area used for information
transfer to and from the disk controller" as $C501–$C7FF [1] §2.5.1 pp. 2-11,
2-14. The source-derived layout above is finer-grained
and internally consistent across three independent programs [3] [4] [6]; the
manual's $C501 figure does not match any of them (the derived buffer begins at
$C3E9, its data portion at $C401) — noted as an open question in §6.

### 2.2 The I/O block (IOB)

"The low 16 words of the 6504 address space are treated as a command block" [1]
§2.5.1 p. 2-11. The IOB is the command interface proper; its first eight bytes
are the parameters the 68000 writes and the rest are results and live state
maintained by the 6504. Layout per the manual's Figure 6-8 p. 6-32b and [4]
(`VAR`):

| 68K addr | 6504 | Name | Meaning |
|---|---|---|---|
| $FCC001 | $00 | **GOBYTE** | the command/handshake byte (§3.1): 0 = idle/command taken; $80–$89 = command codes (§4.1); other values reserved [4] |
| $FCC003 | $01 | COMMAND / MASK / ADRL | triple duty: RWTS command code when GOBYTE = $81 (§4.2); mask byte for the $85/$86/$87 interrupt commands (§4.3); low byte of the called address for the $84 call command |
| $FCC005 | $02 | ADRH | high byte of the called address for $84; "a call to $1FFB will reset the 6504" [4] |
| $FCC007 | $03 | DRIVE | **$00 = upper drive ("drive 0"), $80 = lower drive ("drive 80")** [4]; the manual's Figure 2-7 prints this reversed (§5) |
| $FCC009 | $04 | SIDE | 0 = upper side, 1 = lower side [1] Fig. 2-7; only valid on a dual-sided drive (the 6504 rejects side 1 when the drive reports single-sided [4] `ValidSI`) |
| $FCC00B | $05 | SECTOR | 0–11 on the Sony (zone-dependent max: 12/11/10/9/8 sectors per track class); 0–21 on Twiggy [4] `SecPrTrk`; [1] Fig. 6-6a |
| $FCC00D | $06 | TRACK | 0–79 on the Sony (track 0 outermost [6]); 0–45 on Twiggy [1] Fig. 6-11 |
| $FCC00F | $07 | SPEED / FMTCNF | speed byte for RWTS; **format-confirmation byte** $FF required for FORMAT / FORMAT TRACK to run [4] |
| $FCC011 | $08 | ERRSTAT | result code of the last command (§2.2.1) |
| $FCC013 | $09 | DISKID | the fourth byte of the last sector address field read: format/volume identity of the medium (§2.2.2) |
| $FCC015 | $0A | NoSides / DRVTYPE | drive geometry: **1 = single-sided Sony, 2 = double-sided Sony** (0 is read as Twiggy by the boot ROM) [4] `ChkDrv`; [3] (`DRVTYPE .EQU $FCC015`) |
| $FCC017 | $0B | DrvError | hard drive-level errors (timeouts, no track zero) [4] |
| $FCC019 | $0C | HostSeek | **$FF while the 6504 is moving or parking the head** — the OS driver refuses to issue a seek while this is set [6] (`DISKSKING`) |
| $FCC01B | $0D | SekErr | set to $0F when a step handshake fails — "for external debugging aid" [4] |

The speed byte deserves a note: `SPEED = 00` means "use the zone's nominal
speed"; $01–$FF is "modifier value added to nominal speed" [4]. The Boot ROM
always writes 0 [3] (`TWGRD`). The family page additionally records the speed
byte's use as a host-visible busy flag around the $84 call command — the host
writes $FF, issues the call, and polls the byte back to 0 ([lisa.md](lisa.md)
§13.2).

### 2.2.1 ERRSTAT — result codes

| Code | Meaning | Code | Meaning |
|---|---|---|---|
| $01 | invalid GoByte command | $11 | program error: ROM self-test failed |
| $02 | invalid drive number | $12 | program error: unexpected IRQ/NMI/BRK |
| $03 | invalid side number | $13 | drive error: timeout looking for track zero |
| $04 | invalid sector number | $14 | fatal: the IWM does not respond |
| $05 | invalid track number | $15 | step handshake did not occur |
| $06 | invalid mask | $16 | unable to leave the track-zero position |
| $07 | no clamped disk in drive | $20 | write-protect error |
| $08 | drive not enabled | $21 | can't verify disk |
| $09 | pending interrupts not cleared | $22 | unable to clamp disk |
| $0A | invalid format parameter | $23 | read error |
| | | $24 | write error |
| | | $25 | unable to unclamp diskette |
| | | $26 | cannot find A9 bytes during speed check |
| | | $27 | unable to adjust speed within timeout |
| | | $28 | cannot write speed track |

The $01–$0A codes are command-validation failures reported without an
interrupting operation completing; $11–$16 are hardware/firmware faults; $20–$28
are medium-level errors from the operation itself [4] (`VAR`, error-number
constants); the manual's Figure 2-8 p. 2-14 and Figure 6-9 p. 6-37 print the
same list with one ordering difference — the manual swaps the side/sector codes
($03 invalid sector, $04 invalid side) [1] §2.5.1, §6.3.2. The OS driver reports
these to its clients with 1800 added (read error → 1823, write error → 1824) [6]
(`SOURCE-SONYASM`).

### 2.2.2 DISKID — the address-field identity byte

On every sector header read, the 6504 copies the address field's fourth byte
into DISKID ("Tell host about what type of disk it is" [4] `RdAdr`). The byte
has three documented interpretations, all of the same physical field:

- the Twiggy-era **volume** identification: $00 = Apple II / Apple /// disk,
  $01 = Lisa disk, $02 = Macintosh disk [1] §6.3.4 p. 6-40 [4] (`VOLFND`);
- the Sony-era **format/interleave** encoding — $02 = single-sided 2:1
  interleave, $22 = double-sided 2:1 interleave — byte-identical to the
  Macintosh format byte ([iwm-floppy.md](../../hardware/iwm-floppy.md)
  `Header Field`), which is what the controller itself writes there when
  formatting (`FmtType` = $02 or $22, sensed from the drive [4] `ChkDrv`);
- the OS driver names it simply "flag for single or double sided disk" [6]
  (`SOURCE-SONYASM`, `DISKFLG`), and the Boot ROM's rev-H equates call the same
  byte an "interleave factor" (`INTLV`, IOB offset 18) [3].

That a Macintosh-formatted disk reads $02 here — the same value the Twiggy
comment calls "Macintosh disk" and the Sony format reads as "single-sided" — is
not a coincidence: the Lisa's Sony on-disk sector format is the Macintosh GCR
sector format (§3.7).

### 2.3 Shared constants block ($C021–$C039)

Sixteen bytes of host-tunable operating constants. They are initialized by the
6504 from its ROM at every cold start [1] §6.3.1 p. 6-32b; [4] (`LOOP`,
`Restart`), and the 68000 may change them at any time — with the source's own
warning: "THERE IS NO CHECKING OF THE RANGE OF THESE VALUES SO THE NEW ONE
BETTER MAKE SENSE OR THE 6504 MIGHT GO OFF TO NEVER, NEVER LAND" [4] (`VAR`).

| 68K addr | Default | Name | Meaning |
|---|---|---|---|
| $C021–$C029 | D5 C0 A7 89 64 | **MSpdTbl** | PWM speed codes for track classes 0–4 (class 0 = tracks 0–15 … class 4 = tracks 64–79); runtime-adjusted by the speed-check loop (§3.6) [4] `TABLES` |
| $C02B | $1E | SCDLY | speed-change delay, 5 ms units (default 150 ms) |
| $C02D | 4 | HEADELAY | head settling time, 5 ms units (default 30 ms; comment: "10 for SpdChk" — the speed check is given a 20 ms share) |
| $C02F | 9 | MAXDDLY | motor-off / disk-in-place sample delay, in ~2/3 s units |
| $C031 | $88 | **ROMIDNUM** | the controller ROM identification number — "($0018/FCC031)" [4]; the value $88 pairs with boot ROM rev H as the self-test's "H/88" [2] |
| $C033 | 100 ($64) | MAXRETRY | maximum read/write retries per operation |
| $C035 | 2 | MAXRECAL | maximum recalibrations per operation |
| $C037 | 130 | STPDLY | step delay in 100 µs units (13 ms) |
| $C039 | $4F | MONDLY | motor-on settling delay, 5 ms units (default ~400 ms; "−10 for SpdChk") |

The ROM id at $FCC031 is load-bearing beyond the controller: the boot ROM and
the OS both read it to identify the I/O board and choose the Twiggy or Sony
driver — a zeroed byte is read as a Lisa 1 ([lisa.md](lisa.md) §16.2).

### 2.4 Status block ($C041–$C05F)

Read-only state, refreshed by the 6504 ("READ ONLY STATUS FROM THE 6504" [4]):

| 68K addr | 6504 | Default | Name | Meaning |
|---|---|---|---|---|
| $C041 | $20 | 0 / $FF | **Clamped** | 0 = no disk clamped, $FF = disk clamped and ready [4]; the OS polls it as "disk is in drive" [6] (`DISKIN`) |
| $C043 | $21 | 0 / $FF | MtrOn | drive motor select state |
| $C045 | $22 | — | CurTrack | the drive's current track register |
| $C047 | $23 | — | CurClass | current track class (0–4) |
| $C049 | $24 | $00→$FF | DrvConn | $FF once a drive is sensed present (`Dec DrvConn`) [4] `ChkDrv` |
| $C04B | $25 | $02/$22 | FmtType | what the formatter writes in the address field: $02 single-sided, $22 double-sided |
| $C04D | $26 | — | RetryCnt | remaining retries of the current operation |
| $C04F | $27 | — | RecalCnt | remaining recalibrations of the current operation |
| $C051 | $28 | — | ImAlive | "spins as long as the main loop is executing" — incremented every loop pass, a watchdog-visible heartbeat |
| $C053 | $29 | — | Counter | scratch |
| $C055 | $2A | — | HoldInx | scratch (command index) |
| $C057 | $2B | — | FmtGap | sets of self-sync FFs the formatter writes between fields (set to 7 by FORMAT) |
| $C059 | $2C | 0 | **IMSK** | the interrupt mask: bits 3 and 7 gate their drive's IST nibble (§3.4) |
| $C05B | $2D | — | DIPINTR | flag reflecting the disk-in-place interrupt path |
| $C05D | $2E | — | OKTOGO | "reflects FDIR" — the 6504's own image of whether an interrupt is pending to the host |
| $C05F | $2F | — | **IST** | the interrupt status byte (§3.4) — the byte the manual calls "the status of the controller… at location 00C05F" [1] §2.5.1 |

### 2.5 Error counters, the save ring, and the stack page

The 6504 keeps eight one-byte error counters for the last operation — three
data-field counters (starting bitslip, ending bitslip, checksum) and five
address-field counters (starting bitslip, ending bitslip, wrong sector, wrong
track, header checksum) [1] Figure 6-9 p. 6-37; [4] (`VAR`: `STSLP`…`RACSUM`).
The Sony-generation ROM places them at 6504 $48–$4F (68K odd $C091–$C09F); the
manual's Twiggy figure places them at 6504 $5B–$62 — the layout moved between
ROM generations (§5). The OS driver reads two of them (data checksum count at
$C095, ending bitslip at $C0B9 in its own offsets) and sums the pair into one
soft-error report [6] (`SOURCE-SONYASM`, `INTERRUPT`).

During every command the 6504 also appends the eight IOB bytes to a 64-byte ring
buffer at 6504 $80–$BF (68K odd $C101–$C17F) — "64 byte ring buffer of 8 byte
IOB's", kept "for posterity" [4] (`VAR`: `SavIndex = $80`; `CMD`) — so the last eight commands with their parameters survive for
diagnostics. The Boot ROM reads this saved copy back when reporting a boot
failure [3] (`CMDINDEX`/`INDX_OFST` equates).

Page 1 of the 6504's RAM ($100–$1FF, 68K odd $C201–$C3FF) is its stack page —
the stack pointer is initialized to $1CF and grows down [1] Figure 6-3; [4]
(`Restart`: "Init stack to '01CF' -- push down stack") — with the VERIFY
bad-sector report (count, track, side, and a list of sector numbers) occupying
$1D0–$1F3, directly above the stack top [4] (`SctrCnt`, `TrkNumb`, `SidNumb`,
`SctrSav`). The 68000 should not touch this page while any command might be
executing.

### 2.6 Parameter memory ($C181–$C1FF)

The 64 bytes at 6504 $C0–$FF are reserved for the 68000 — "Used only by the
68K" [1] Figure 6-3 — and the 6504 never reads or writes them in normal
operation (that is the entire point of the `GO AWAY` command, which parks the
6504 in ROM at power-down so that a spurious write cannot corrupt them, [1]
§6.3.2 p. 6-35). The block holds the system parameter memory — boot volume,
device code, screen/mouse settings, and a 16-bit validity checksum — whose
layout and checksum algorithm are documented in [pram.md](pram.md) §2–§5. The
Boot ROM keeps its own 16-byte sub-area at $FCC161 with a checksum word at
$FCC17D [3].

### 2.7 The I/O buffer ($C3E9–$C7FF)

The 524-byte transfer buffer occupies the last 524 bytes of the shared RAM:
twelve header/tag bytes at 6504 $1F4–$1FF (68K odd $C3E9–$C3FF) followed by 512
data bytes at $200–$3FF (68K odd $C401–$C7FF) [4] (`VAR`: `Bufr12`, `Page02`,
`Page03`; page-crossing composite bytes handled by `PRENIB`). The three
consumers agree: the Boot ROM reads the header at buffer offset $3E8 from
$FCC001 and the data 24 bytes higher [3] (`DSKBUFF`, `DSKDATA`); the OS driver
uses `$3E8`/`$400` from $FCC000 with `MOVEP` (+1) addressing [6]; the manual
says "524 bytes used to transfer data" [1] §6.3.1 p. 6-32b. The buffer is the
`FORMAT`/`VERIFY` workspace as well: bad-sector numbers are collected in the
stack page and the buffer is cleared by `FORMAT` before writing [4] (`FORMAT`,
`CLRTRK`).

The 524-byte size is not arbitrary: it is exactly one on-disk sector's data
field (§3.7) — twelve tag bytes plus 512 bytes of data, the same sector payload
the Macintosh carries ([iwm-floppy.md](../../hardware/iwm-floppy.md) `Data
Field`).

### 2.8 The 6504's private I/O space (not 68000-visible)

For completeness — the register set the 6504 itself drives, at its own addresses
$0800 up [4] (`VAR`, `IOSpace`). None of this is reachable by the 68000; it is
listed because the timing and control semantics above are all implemented
against it:

| 6504 offset | Name | Role |
|---|---|---|
| $800/$802/$804 | CA0/CA1/CA2 | control signals 0–2 "for MCI PAL in Sony drive" — mode/serial-link commands to the drive's monitor PAL |
| $806 | LSTRB | load strobe: 0→1→0 strobes the PAL |
| $808 | MTENA | enables the DRVENA output (motor power) |
| $80A | DRENA | 0 selects drive 0 (upper), 1 selects drive 80 (lower) |
| $80C/$80D | Q6L/Q6H | IWM softswitch Q6 — with Q7 selects Sense / Read / Write / Write Load [1] §6.3.5 |
| $80E/$80F | Q7L/Q7H | IWM softswitch Q7 — Q7L reads the handshake/status bit (bit 7), Q7H loads the mode register (`IWMMode = $1F`, "asynch, 8 MHz, latch, 2 µs calls, no timer") |
| $810 | CNTENA | low enables the PWM counter/comparator |
| $816 | PWMENA | high gates the PWM pulse train to the drive |
| $818/$819 | DISL/DISH | **DIS** — deassert/assert the 68000's access to the shared RAM (§3.3) |
| $81A/$81B | SIDE0SEL/SIDE1SEL | head (side) select |
| $81C/$81D | BOOTL/BOOTH | the **DSKDIAG** line: "when high then I'm listening" — the controller-busy flag the Boot ROM polls before every command [3] (`CMDCHK`) |
| $81E/$81F | FDIRL/FDIRH | **FDIR** — lower/raise the interrupt to the 68000 (§3.4) |
| $820 | PWMREG | the PWM speed register (§3.6) |


## 3. Behaviour

### 3.1 The command handshake

All commands follow one protocol [1] §6.3.2 pp. 6-32b – 6-36, Figure 6-7; [4]
(`CMD`, `LOOP`):

1. The 68000 builds the IOB (drive/side/track/sector and command-specific
   bytes) with byte writes or `MOVEP`, then writes the **GoByte** with its
   high bit set ($80–$89). A GoByte with bit 7 clear is "no command".
2. The 6504's idle loop polls the GoByte once per pass ("10 microsecond
   intervals" [1] §6.3.1). On seeing bit 7 set it **copies the first eight IOB
   bytes into its private internal IOB** so the 68000 can start building the
   next command immediately, then checks for pending interrupts — **if any
   interrupt is pending, an $81 RWTS command is refused with code $09
   ("interrupts pending")** and the completion interrupt is still raised [4]
   (`Rwts`).
3. The command number is range-checked ($83–$89), then **validated**: each
   command has a table-driven set of parameter tests (drive present and
   clamped, side legal for the drive, sector within the track class's
   maximum, track ≤ 79, mask well-formed, write protection, format/verify
   precondition of track = side = 0) [4] (`Validate`, `TestTbl`, `TestGoB`).
   A failed validation writes the error code to ERRSTAT and aborts.
4. The command executes. Interrupting RWTS commands **lock the shared RAM**
   (§3.3) for their duration; non-interrupting commands ($83 seek, $84 call,
   $85–$87, $88, $89) do not.
5. The 6504 **clears the GoByte** — this is the "command taken" signal every
   host polls before issuing anything further [3] (`CMDCHK`); [6]
   (`START_RWTS`). For RWTS commands the GoByte is cleared at dispatch and
   again at completion, with ERRSTAT holding the result [4] (`Rwts`).
6. An RWTS command ends by setting its completion bit in IST and gating FDIR
   through the mask (§3.4).

The 68000-side discipline that falls out: never write a new command while the
GoByte is non-zero, never queue an RWTS while an interrupt is uncleared, and
always read ERRSTAT after the completion interrupt.

### 3.2 The main loop, disk-in-place polling and motor-off

"The 6504 firmware always executes in one of three possible states: waiting
for a macro command from the 68000, checking the status of the drives, or
executing a macro command" [1] §6.3.1 p. 6-32b. The idle loop spins the
`ImAlive` heartbeat byte, decrements a three-byte counter (low two bytes every
pass ≈ 10 µs each; the high byte in units of ~2/3 s), and acts at the
boundaries [4] (`LOOP`); [1] §6.3.1:

- **When the two low bytes reach zero (~once per second)**: sample the drives'
  status. The check is deliberately infrequent — "it takes time and blocks any
  response to the 68000 while it is executing. It also involves turning on the
  phi0 line to the disk motor, which causes noise" [1] §6.3.1.
- **When the high byte reaches zero**: park the heads and turn the motors off
  [4] (`LOOP`); the high byte is directly modifiable by the 68000 (it lives in
  the shared constants block as MAXDDLY, §2.3).
- Every pass: examine the GoByte for a pending command.

On a disk-in-place transition the behavior depends on the mask: on a drive
whose interrupt is **enabled**, the insertion latches the IST disk-inserted bit
and raises FDIR — "the 68000 must clear the interrupt to issue the command
that clamps the disk" [1] §6.3.1; on a **disabled** drive the disk is clamped
automatically and no interrupt is raised. In the Sony-generation firmware the
clamp is performed by the controller itself: on seeing disk-in-place with
nothing clamped it waits one second "for Sony drive to return to its senses",
recalibrates, and marks the disk clamped [4] (`GetDIP`). An eject-button press
with no disk in place is ignored; with a disk in place it either interrupts
(enabled drive) or unclamps automatically (disabled drive) [1] §6.3.1.

### 3.3 Memory lockout (DIS) and bus-error behavior

Time-critical firmware cannot tolerate 68000 accesses to the shared RAM, so
the 6504 can **lock the host out**: "The processor is capable of locking out
any communication on the main bus by means of the DIS signal… many routines
within the disk controller are time-critical" [1] §6.2.2 pp. 6-6a – 6-7b.

The mechanism is a deliberate hang, not a fault flag. When DIS is asserted the
bus transceiver is disabled and the DTACK/ generation flop can never be
satisfied: "any attempt to communicate with the controller while DIS is high
does not result in a DTACK/ acknowledgement signal. The access cycle hangs,
waiting for this signal until the bus timeout on the processor board triggers
and the BERR/ signal is generated" [1] §6.2.2. (The processor board's AS/-based
timeout is the Lisa's general 30–300 µs bus watchdog — [lisa.md](lisa.md)
§7.4.) The firmware asserts DIS as late as possible before a transfer routine
("disable address at last possible moment") and deasserts it the moment the
routine exits, also clearing the busy line for the host [4] (`Rwts7`).

The hardware manual adds a second, stronger requirement on the host: "the
interrupt flag must first be enabled or a bus error will occur. The enable bit
must be high in order to be able to access the floppy RAM that is shared by the
floppy-disk controller and the processor board" [1] §2.5.1 p. 2-13. All shipped
software honors it — the boot ROM enables the drive's interrupt mask before its
first command [3] (`TWGBOOT`), the OS driver's `INITDISK` clears then enables
[6], and the loader does the same [6] — but the manual does not explain the
mechanism, and the firmware sources contain no such gate on DIS (§6, open
question).

### 3.4 Interrupts: IST, IMsk and FDIR

Interrupt events are latched in the **IST** byte at $C05F; each drive owns one
nibble [1] Figure 2-9 p. 2-15; [4] (`VAR`):

| Bit | 6504 name (drive 0 = upper, $00) | Bit | 6504 name (drive 80 = lower, $80) |
|---|---|---|---|
| 0 | drive 0 disk inserted | 4 | drive 80 disk inserted |
| 1 | drive 0 button pressed | 5 | drive 80 button pressed |
| 2 | drive 0 R/W command completed | 6 | drive 80 R/W command completed |
| 3 | OR of bits 0–2 | 7 | OR of bits 4–6 |

These are **latched event bits**, not a live drive-state snapshot: the 6504 ORs
the event in, recomputes the OR bits, and gates the result with the **IMSK**
byte — `FDIR` is raised whenever `(IST AND IMsk) ≠ 0` and dropped when it is
zero [4] (`UpdInt`). The mask has exactly two meaningful bits: **bit 3 gates
drive 0's nibble, bit 7 gates drive 80's nibble** (the $86/$87 commands set and
clear them; $85 clears selected IST bits through the mask byte) [4] (`VAR`
mask encoding; `EnblTest`).

The per-event writes in the Sony-generation firmware are explicit: an insertion
on the (single, lower) drive sets bit 4 (`LDA #$10; ORA IST`) [4] (`GetDIP`), and
an RWTS completion sets bit 6 (`LDA #$40; ORA IST; STA IST; JMP UpdInt` — the
assembled listing's comment: "use drive 80 / set bit in interrupt status") [4]
[5] (`RWTS5`). With the mask's bit 7 set, either event also sets the nibble's OR
bit and raises FDIR.

FDIR reaches the 68000 by two parallel paths [1] §6.2.6 p. 6-12:

- **as a level on VIA1 port B bit 4**, where it can be polled — the Boot ROM
  programs the VIA's data-direction bit and tests it in a loop [3] (`FDIR .EQU 4
  — port B bit 4 has FDIR state`; `CHKFIN` polls an ~8.8 µs loop); and
- **as the controller's interrupt request** (IOIR/), which the I/O board wires
  at **IPL 1**, shared with the parallel-port hard disk and the video
  vertical-retrace — the level-1 handler must poll the floppy status byte
  $C05F, the parallel VIA's IFR and the Status Register's retrace bit to find
  the source ([lisa.md](lisa.md) §7.1, §12).

A second housekeeping line, **DSKDIAG** ("the Disk Diagnostic line… indicates
that the controller is performing disk diagnostics" [1] §6.2.6), is the
controller's busy signal; the firmware drives it low ("tell host I'm busy")
whenever it is in a state where commands may be missed [4] (`GetDIP`), and every
host command-wait loop polls it alongside the GoByte [3] (`CMDCHK`, ~9.6 µs
loop); [6] (`wait_drv`). The manual describes DSKDIAG on PB7 of the parallel
port VIA while the boot ROM's equate and the loader both use port B **bit 6**
[3] [6] — noted in §5.

The host drains events by writing a mask of the bits to clear to $FCC003 and
issuing **$85 (CLEAR INTERRUPT STATUS)**; the 6504 ANDs IST with the complement
of the mask and recomputes FDIR [4] (`ClrIst`). The boot ROM clears with $FF
(all), $CC (both drives' R/W-complete bits) after a read, and $08/$88-shaped
masks for enable/disable [3]; the OS driver clears exactly the bits it read
back ("the interrupt status byte is echoed as the clear mask") [6]; the loader
clears all, then $77, then disables with $88 [6] (`ldmicro`).

### 3.5 Seek, recalibrate and head positioning

Head positioning is a cooperative dance between the 6504's step timing and the
drive's carriage:

- **Stepping is in 1/8-track increments** over the four phase lines, so all
  tracks are eight steps apart; seeks are completed "in the same direction,
  towards the calibration point, for the last half track", with an overshoot
  of four steps on reverse-direction seeks [1] §9.3.2 pp. 9-4a – 9-4b, §6.3.5.
- Each step is acknowledged by the drive: the firmware pulses the phase lines
  and then **waits up to 12 ms twice for the drive's /STEP handshake**; a
  missing handshake sets the SekErr flag ($0F) in the IOB [4] (`DoStep`).
- Step pacing uses three distinct phase durations to manage carriage inertia
  [1] §9.3.2, and the step delay itself is host-tunable (STPDLY, §2.3). A
  direction reversal costs a fixed 40 ms turnaround ("5×8 = 40 msec turn around
  time for changing directions") [4] (`VAR`, `RECAL`).
- **Recalibration** uses the drive's optical track-zero sensor: the firmware
  first steps a few carriage increments (about half a track) away from track
  zero — the source comment's reason: "for bad power supply" — waits the
  turnaround, then steps inward until the track-0 line
  drops, then outward until it asserts, counting steps, then waits ~150 ms and
  runs a speed check at track 0. Failure modes are a timeout leaving track
  zero (error $16) or a recal timeout (error $13) [4] (`Recalbrt`); [1] §6.3.5.
- A full seek selects the spindle speed for the destination's track class,
  waits the speed-change delay, steps, waits motor-on (if the motor was off)
  and head-settle times, and finishes with a speed check [4] (`Seek`).
- The **HostSeek** byte ($FCC019) is $FF for the whole seek/park, which is how
  the OS driver knows not to stack a seek on top of one [6].

The Twiggy-generation drive additionally derived its clamping action from
carriage motion — "this mechanism not only provides head-to-track positioning
but also actuates a disk clamping and unclamping mechanism" [1] §9.4.3 — which
is why the command set has an explicit CLAMP; on the Sony mechanism clamping is
automatic and the clamp entry point in the Sony firmware is a null operation
(§5).

### 3.6 Spindle-speed control

The Sony drive's spindle is **software-paced**: the 6504 loads a PWM register
(PWMREG, §2.8) from the per-class speed table, enables the counter/comparator
(CNTENA) and the pulse output (PWMENA), and the drive's motor control follows
the pulse train [4] (`HardInit`, `SetSpeed`). Low table values mean higher
speed (the firmware's own comment: "Low # = high speed"; the defined speed-code
bounds are MINSPEED $D4 / MAXSPEED $38) [4] (`VAR`).

| Track class | Tracks | Sectors/track | Default speed code |
|---|---|---|---|
| 0 | 0–15 | 12 | $D5 |
| 1 | 16–31 | 11 | $C0 |
| 2 | 32–47 | 10 | $A7 |
| 3 | 48–63 | 9 | $89 |
| 4 | 64–79 | 8 | $64 |

[4] (`TABLES`, `SecPrTrk`, `ShareRam`)

The zone scheme is a **constant-angular-velocity-with-zoned-sector-count**
layout: outer tracks carry more sectors at a lower rotational speed — the same
trick the Twiggy drive used with eight zones (Figure 6-11 of [1]: tracks 0–3,
22 sectors, 218.3 rpm … tracks 42–45, 15 sectors, 320.1 rpm, with a dedicated
**track −1 as a speed-synchronization track**, written only during format and
used to trim the speed "to within 0.4%") and the Macintosh 3.5″ mechanism uses
with these same five zones ([iwm-floppy.md](../../hardware/iwm-floppy.md) `Disk
Geometry`).

Speed is **measured and corrected in firmware**: the drive returns a tachometer
pulse train (the firmware switches the sense path to "tach mode" — side-select
high, a specific CA2 pattern — before measuring) [4] (`SetTach`); the
controller times six tach pulses at 18 µs resolution and compares the count
against per-class windows from two "empirically determined speed tables" —
`WideLow`/`WideH` and `ThinLow`/`ThinH`, five 16-bit bounds each [4]
(`TABLES`, `TimIt`). Correction is two-stage: adjust ±2% (steps of 5 in the
speed byte) until within the wide window, then ±0.5% (steps of 1) until within
the narrow window, up to 100 attempts, re-measuring after each change with the
speed-change delay in between; exhaustion reports error $27 "unable to adjust
speed within timeout" [4] (`SpdChk`). A speed check runs after every seek and
after recalibration [4] (`Seek`, `Recalbrt`); the manual states the drive's
integral control holds speed "within 3% of that desired" during normal
operation [1] §6.3.5.

### 3.7 GCR encoding and the on-disk sector format

Data is not written raw. Three 8-bit bytes become four encoded bytes by table
lookup; every encoded byte has its MSB set (so the read state machine can find
byte boundaries) and contains no more than two consecutive zero bits (so the
data itself supplies read clocking — "Transitions from the data bits themselves
occur frequently enough to permit the state machine to remain synchronized")
[1] §6.3.3 pp. 6-38b – 6-39a. The 64-entry codeword table [1] Figure 6-10:

```
      0    1    2    3    4    5    6    7    8    9    A    B    C    D    E    F
 0/8  96   97   9A   9B   9D   9E   9F   A6   A7   AB   AC   AD   AE   AF   B2   B3
1/9   B4   B5   B6   B7   B9   BA   BB   BC   BD   BE   BF   CB   CD   CE   CF   D3
2/A   D6   D7   D9   DA   DB   DC   DD   DE   DF   E5   E6   E7   E9   EA   EB   EC
3/B   ED   EE   EF   F2   F3   F4   F5   F6   F7   F9   FA   FB   FC   FD   FE   FF
```

This is byte-identical to the Macintosh's 6-bit-to-7-bit GCR table ([4]
`TABLES`, duplicated four times so any byte indexes it; compare
[iwm-floppy.md](../../hardware/iwm-floppy.md) `GCR Encoding`).

A sector on disk consists of four fields [1] §6.3.4 pp. 6-39a – 6-40; [4]
(`NEWRWADDR`, `NREAD16`, `WRITE16`):

| Field | Pattern |
|---|---|
| Header sync | self-sync bytes: $FF written with two zero bits between copies (10 bit times each); the run is terminated by the field's identification marks that follow (`D5 AA 96` for the header, `D5 AA AD` for the data field); the formatter writes `FmtGap` sets (default 7) between fields |
| Header (address) | `D5 AA 96`, track, sector, side, format/volume byte (§2.2.2), XOR checksum of the previous four, `DE AA`, one pad byte with write electronics off |
| Data sync | more self-sync $FF bytes |
| Data | `D5 AA AD`, sector number, **524 payload bytes** (12 tag + 512 data, GCR-encoded to just under 699 code bytes (the two bytes past the last whole three-byte group are packed into composite bytes across the buffer page boundaries [4] `PRENIB`)), **3 checksum bytes**, `DE AA`, pad |

The sync pattern the formatter writes between fields is the byte sequence
`FF FC F3 CF 3F FF` [4] (`CLRTRK`, `BsTbl`) — six code bytes carrying five
10-bit self-sync patterns, matching the manual's "32 Self Synch FFs" header-sync
description for Twiggy and the Macintosh's 6.25-byte sync fields
([iwm-floppy.md](../../hardware/iwm-floppy.md) `Sector Format`).

The **3-byte data checksum** is not a plain sum. The firmware's own algorithm
description [4] (`CRECKSUM`), executed over the 524-byte payload:

```
CSUMA, CSUMB, CSUMC are the three accumulating checksum bytes;
ByteA, ByteB, ByteC are three successive payload bytes.
repeat for every group of three:
    rotate CSUMC left one bit (through carry)
    CSUMA := CSUMA + ByteA + carry(from rotate)
    ByteA := ByteA XOR CSUMC          (and is stored back to the buffer!)
    CSUMC := CSUMC + ByteB + carry(from the CSUMA add)
    ByteB := ByteB XOR CSUMA
    CSUMB := CSUMB + ByteC + carry
    ByteC := ByteC XOR CSUMC
```

Note what this means for the medium: **the data on disk is the
checksum-scrambled payload** — the rotate/add/XOR cascade is applied in place
in the I/O buffer before encoding, and the read path undoes it (`VfyCksum`
runs the same cascade over the bytes read and then compares its three
accumulators with the three checksum bytes read from disk) [4] (`VFYCKSUM`).
The address-field checksum is a plain XOR of its four data bytes [1] §6.3.4;
[4] (`WAdr16`).

Reading a sector is a state-machine-plus-firmware pipeline: the separator
shifts a one in on each read-data transition and a zero when 2 µs pass without
one, aligning bytes on the MSB rule; the firmware scans for the address marks
(allowing 20 retries at nibble level — `NIBLRETR`), verifies the header's
checksum, compares track and sector against the command, then reads the 524
payload bytes plus 3 checksum bytes through the nibble-decode table, and
finally verifies the checksum [4] (`RdAdr`, `Read16`, `RdSynTop`); [1] §6.3.5.
Writing is the mirror image, paced so that no more than 49 6504 cycles (24.5
µs) elapse between the separator's handshake and the next byte load — the
firmware annotates the worst-case path cycle by cycle, and a failure of that
deadline is detected by the separator's own underrun bit and reported as
error $31 "under-run while writing data fields" [4] (`WRITE16`).

The address-field marks (`D5 AA 96` / `DE AA`) and the data-field marks
(`D5 AA AD` / `DE AA`) are kept in a **host-modifiable table** in the shared RAM — the source's stated reason:
"allows modification by high level software for copy protection" [4] (`TABLES`,
`SavAdr`/`SavDat`, copied to RAM at cold start).

### 3.8 Retries, off-track recovery and verify

The controller does its own remedia before giving up. On a read error the
firmware retries the sector; when the error signature changes between
attempts — or after exhausting the retry budget (MAXRETRY, default 100) — it
**recalibrates from the optical sensor and re-seeks**, up to MAXRECAL times,
refreshing the retry budget after each recalibration; a second consecutive
identical failure aborts [4] (`BadAddr`, `RtyFlg`). Beyond that, "the
floppy-disk controller attempts to retry an operation on the disk which results
in an error condition… Retries involve moving the head off track in increments
of 1/8th of a track and recalibrating the head using the optical sensor on the
drive" [1] §6.3.2 — the 1/8-track carriage resolution (§3.5) exists precisely
to allow off-track offset reads of marginal media.

All errors increment the eight per-operation error counters (§2.5), which is
how the host can distinguish a bad spot on the medium from a dead drive.

`VERIFY` and `VERIFY TRACK` read whole tracks without writing and collect the
bad sectors into the report area — count, track, side and a list of sector
numbers at 6504 $1D0 up [4] (`FORMAT`/`Verify`); [1] §6.3.2. `FORMAT` finishes
by verifying the whole disk from the outside in ("from inside of disk to outer
edge", i.e. track 79 downward), and `FORMAT TRACK`/`VERIFY TRACK` stop after
the one track [4] (`Format`).

### 3.9 Format and interleave

`FORMAT` (whole disk) and `FORMAT TRACK` write every field of every sector:
self-sync gaps, address fields, data fields — with the buffer pre-cleared and
the interleave **2:1** ("It will write sector 0, sector x, sector 1, sector y,
etc." [4] `WriTrk`; the OS driver's sector equate carries the same note:
"formatted 2:1 interleave" [6]). On a dual-sided drive both sides are written
per track before stepping [4] (`Format`). The whole-disk path starts with a
recalibration and formats from track 0 outward to track 79; a dedicated
sync-track writer exists for the Twiggy drive's speed-synchronization track
[4] (`WrSynTrK`) — the Sony format has no separate speed track; speed is
trimmed by the tach feedback loop of §3.6 instead.

Formatting requires the **format-confirmation byte** ($FF at $FCC00F) —
without it the command is refused with $0A "invalid format confirmation",
which is the guard against accidental erasure [4] (`ValidCF`); the Boot ROM
equates name it "confirmation for format" [3]. The address-field side byte
takes bit 5 for side 1, and bit 0 of the same byte carries the track number's
bit 6 (the side field doubles as the track-extension bit — the same encoding
the Macintosh uses, [iwm-floppy.md](../../hardware/iwm-floppy.md)
`Header Field`) [4] (`WAdr16`).


## 4. Programming model

This section is how real shipped software actually drives the controller: the
Boot ROM rev H [3], the boot loader-loader [6], and the Lisa OS Sony driver [6].

### 4.1 The GoByte command set

Commands are single bytes written to $FCC001 with bit 7 set; the 6504 clears the
byte when it has taken the command [1] Figure 2-6 p. 2-12; [4] (`VAR`):

| GoByte | Command | Interrupts the 68000 on completion? | Notes |
|---|---|---|---|
| $80 | **NULL** | no | "tests handshake" — a no-op the host can use to verify the command path [4] |
| $81 | **EXECUTE RWTS** | yes | the command code at $FCC003 selects the operation (§4.2) |
| $82 | — | — | reserved / "not used" [1] Fig. 2-6 |
| $83 | **SEEK** | no | move the head to drive/side/track; no data transfer [1] Fig. 6-6l |
| $84 | **CALL** | no | the 6504 parks the heads, turns the motors off, clears the GoByte and jumps to the 6504 address in ADRL/ADRH — used by diagnostics to run downloaded code [1] §6.3.2; [4] (`Call`) |
| $85 | **CLEAR INTERRUPT STATUS** | no | clears the IST bits selected by the mask at $FCC003 (§3.4) |
| $86 | **SET INTERRUPT MASK** | no | ORs the mask into IMsk — bit 3 enables drive 0's events, bit 7 drive 80's [4] (`VAR`) |
| $87 | **CLEAR INTERRUPT MASK** | no | ANDs the complement of the mask out of IMsk |
| $88 | **WAIT IN ROM** | no | parks the heads and loops in ROM watching RAM location $00 for the byte sequence $69 then $96, then performs a cold start — "primarily to remove the 6504 from the RAM area while diagnostics are being performed" [1] §6.3.2; [4] (`WaitRom`) |
| $89 | **GO AWAY** | no | parks the heads and loops in ROM forever, never touching RAM or I/O — "once issued, this command cannot be stopped without resetting the Lisa"; the boot ROM issues it before soft power-off [1] §6.3.2; [3] (`DSCONT`); [4] (`ESAD`) |

The interrupt commands take their mask in the same byte the RWTS commands use
for the command code ($FCC003, §2.2); the documented per-event mask bits are
$01/$10 (clear inserted, drive 0/80), $02/$20 (clear button), $04/$40 (clear
R/W-complete), $08/$80 (enable/disable the drive) [4] (`VAR` mask encoding).

### 4.2 The RWTS command codes

With GoByte $81, the byte at $FCC003 selects the sector-level operation [1]
Figure 2-7 p. 2-13; [4] (`VAR`); the OS driver's names in parentheses [6]:

| Code | Operation | Interrupts |
|---|---|---|
| $00 | READ (`readdisk`) — seek, find header, read 524 bytes, verify checksum | yes |
| $01 | WRITE (`writedisk`) — seek, find header, create checksum, write data field | yes |
| $02 | UNCLAMP (`unclamp`) — eject: release the disk so it can be removed; on the Sony the drive runs its auto-eject and the controller clears the clamped flag after ~750 ms | yes |
| $03 | FORMAT (`format`) — whole disk, both sides on a dual-sided drive, requires $FF in the confirmation byte | yes |
| $04 | VERIFY (`verify`) — read the whole disk without writing, report bad sectors | yes |
| $05 | FORMAT TRACK | yes |
| $06 | VERIFY TRACK | yes |
| $07 | READ BRUTE FORCE (`read_bf`) — "identical to a READ operation, except that agreement of the data with the three checksum bytes… is ignored. It enables partly erroneous data to be read" [1] §6.3.2 | yes |
| $08 | WRITE BRUTE FORCE (`write_bf`) — "identical to the WRITE operation, except that the command supplies the three checksum bytes" [1] §6.3.2 — used for recovering/generating nonstandard checksums (with $07) | yes |
| $09 | CLAMP (`clamp`) — clamp an inserted disk on an interrupt-enabled drive; a **null operation in the Sony-generation firmware** (§5) | yes |

Every code validates its own parameter subset before running (drive/clamp,
side, sector, track, mask, confirmation, write protection, format/verify
preconditions) [4] (`TestTbl`). The controller rejects a command addressed to
a drive that is not enabled in the mask with code $08 — so the enable/disable
commands are also a presence gate, not just an interrupt gate.

### 4.3 The host-side interrupt protocol

Putting the pieces of §3.4 into the sequence every host performs:

1. **Enable** the drive's events: mask $08 (drive 0) or $80 (drive 80) at
   $FCC003, GoByte $86. The Boot ROM enables the drive it is about to boot
   from [3] (`TWGBOOT`); the OS driver's `INITDISK` clears all pending
   interrupts first (mask $FF, GoByte $85), then enables with $80 [6]; the
   burn-in ROM enables both with $88 [3] (`TWGTST`).
2. **Drain**: mask $FF + GoByte $85, then poll VIA1 PB4 until FDIR drops
   (~200 µs timeout in the boot ROM, which treats a stuck line as an error)
   [3] (`CLRFDIR`).
3. **Issue** a command and wait for FDIR to rise — polling PB4 [3] [6] or
   blocking on the IPL-1 autovector ([lisa.md](lisa.md) §7.1); the boot ROM's
   timeouts are 15 s for command acceptance, 2 minutes for FDIR, 15 s for
   eject [3] (`CMDTIME`, `FDIRTIME`, `EJCTTIME`).
4. **Read** ERRSTAT at $FCC011, and the error counters if the error is
   medium-level.
5. **Clear** the completion bits: mask $CC (bits 7, 6, 3, 2 — the R/W-complete
   bits of both drives) + GoByte $85 [3] (`TWGREAD`).

The level-1 interrupt handler in the OS reads the IST byte, echoes it as the
clear mask, and decodes the drive-80 nibble: its interrupt record names the
four bits "bottom interrupt, bottom done, unused (SONY has no button), bottom
in", with the top drive's nibble reserved ("only one drive") [6]
(`SOURCE-SONY`).

### 4.4 The boot ROM's boot sequence (rev H)

The Boot ROM's floppy path [3], for a Sony-based machine (SYSTYPE ≠ Lisa 1 —
the ROM reads the controller ROM id at $FCC031 to decide, [lisa.md](lisa.md)
§16.2):

1. **Select the boot device** from parameter memory's device code; the
   default boot is the lower drive (drive $80) [3] (`DVCECHK`, `TWIG2`).
2. **Install a bus-error vector** — a controller access with the RAM locked
   out faults, and the boot path wants to catch it [3] (`TWGBOOT`).
3. **Enable the drive's interrupts** (mask $08 or $80, GoByte $86), program
   VIA1 PB4 as an input, then **clear all pending events** and wait for FDIR
   to drop [3].
4. **Read block 0**: build the IOB with one `MOVEP.L` (drive/side/sector/track
   packed into a longword), write speed 0 and command $00 (READ), write GoByte
   $81, poll FDIR, read ERRSTAT, clear the R/W-complete bits with mask $CC
   [3] (`TWGREAD`). The 12 header bytes go to a header buffer at $1FFF4 and the
   512 data bytes to $20000.
5. **Check the boot file ID** in the header against the boot pattern $AAAA; on
   a mismatch, read track 1 once (to let the head align), retry track 0, and
   check again — a bad header reports error 38, "bad header (not a boot file
   id)" [3] (`DOREAD`).
6. If no disk is in place the read fails with code 7 ("no disk in drive"); the
   ROM displays the insert-disk icon and **waits up to two minutes for the
   insertion interrupt**, clears it, issues a CLAMP ($81 with command $09 —
   harmless on the Sony, §5), and retries the read [3] (`DSKCHK` path).
7. **Jump to the loaded block** at $20000 — which contains the boot
   volume's stage-1 loader [3] (`STRTBOOT`).

On a read failure the ROM copies the controller's diagnosis out for display:
the error status, the data-checksum error count, the address-checksum error
count, and the retry count from the shared RAM [3] (`DSKBAD`). On failure or
normal exits it **ejects the disk (UNCLAMP)** and **disables both drives**
(mask $88, GoByte $87) [3] (`EJCTDSK`, `DSABLDSK`).

The self-test displays the controller's ROM id next to the boot ROM's — "if
H/88 was displayed… the Boot ROM is revision H and the Floppy ROM version is
88" [2].

### 4.5 The loader and the OS driver: block geometry

Block 0's stage-1 loader (the "loader-loader") is Sony-specific and
deliberately small: it walks the first zone assuming 12 blocks per track
("blocks per sony track, for tracks 0-15" — correct only while it stays inside
tracks 0–15, which is all it needs to load the stage-2 loader), always drives
the lower drive ($80), reads blocks by the same enable/clear/read/erase
sequence as the boot ROM, and **terminates the boot if it finds an
eject-button event in the IST byte** (mask $22 — both drives' button bits —
before every read) [6] (`ldmicro`).

The stage-2 code and the OS driver use the full geometry: a **table-driven
block-to-(side, track, sector) conversion** with 800 blocks per side
(`SNYSNGL = 800`), the five zone boundaries 192/368/528/672/800 blocks (12
sectors × 16 tracks, then 11 × 16, 10 × 16, 9 × 16, 8 × 16), side 0 first
(tracks walked 0→79), then side 1 (the table walks it from track 79 down to
0), for a maximum of 1600 blocks; blocks past the
table are rejected as bad parameters [6] (`SOURCE-SONYASM`, `CONVERT`).

This is the **400 KB / 800 KB question** answered on the drive's own terms:

- The **mechanism** senses as single- or double-sided at cold start
  (`ChkDrv`), setting the IOB's NoSides byte (1 or 2) and the formatter's
  FmtType ($02 or $22) [4].
- A **single-sided drive or medium** yields 800 blocks = 400 KB — and the OS
  driver initializes the device record with exactly `num_bloks := 800` [6]
  (`SOURCE-SONY`, `hdinit`) — the shipped machines ran their internal Sony as
  a 400 KB device (*inferred* from the driver's fixed 800 blocks together with
  the single-sided format encoding $02).
- A **double-sided mechanism** is fully supported by the controller firmware
  (side-1 reads/writes, double-sided format) and by the driver's conversion
  table up to 1600 blocks = 800 KB [4] [6].
- The **on-disk sector format is the Macintosh GCR format** — same codeword
  table, same 524-byte data field with 12 tag bytes, same marks, same $02/$22
  format byte, same 2:1 interleave (§3.7, §3.9; compare
  [iwm-floppy.md](../../hardware/iwm-floppy.md) `Sector Format`) — with the
  volume-layout difference that the Macintosh interleaves double-sided blocks
  by cylinder while the Lisa numbers a whole side then the other [6]
  (`CONVERT`); [iwm-floppy.md](../../hardware/iwm-floppy.md) `Block Numbering
  and Cylinder Interleaving`. A Macintosh single-sided disk therefore reads
  with DISKID = $02 ("Macintosh disk", §2.2.2) and a block layout that matches
  side 0 exactly.
- The **Twiggy** geometry the controller also serves is 46 tracks per side,
  eight zones (22…15 sectors), 1702 blocks per double-sided disk [1] Figure
  6-11; [3] (`DSKSIZE`); the boot ROM carries both code paths and the
  old-machine conditional (`NEWTWIG`) [3].

### 4.6 Observed boot behaviour

The observable end-to-end sequence on a Sony-based machine, assembled from the
shipped programs above:

1. Power-on: the 6504 cold-starts, initializes the IWM and latches, waits
   ~1.25 s "for Sony to power up", copies its default constants into the
   shared RAM, clamps any disk already in the drive, raises DSKDIAG and
   enters the idle loop [4] (`Restart`).
2. The 68000 self-test reads the ROM id ($FCC031) and displays the pairing
   (H/88 on a Macintosh XL) [2]; machine type is derived from the same byte
   [lisa.md](lisa.md) §16.2.
3. Boot attempt on drive $80: enable → clear → read block 0 → header check →
   jump to $20000 [3].
4. With no disk: the insert-disk icon, a two-minute FDIR wait; on insertion
   the controller auto-clamps (one-second settle, recalibrate, clamped flag
   set) and raises FDIR [4] (`GetDIP`); [3]; the ROM clears, clamps and
   retries [3].
5. The loader-loader loads the loader 12-blocks-at-a-time inside zone 0 and
   hands off; the loader reads the real file structure with the 800/1600-block
   conversion [6].
6. Under the OS, the driver runs the drive at IPL 1: every completion raises
   FDIR, the level-1 handler reads IST, echoes the clear mask, decodes the
   bottom-drive nibble, and completes the request; the OS scheduler idles in
   a halted-processor state that the floppy interrupt is one of the wake
   sources for [6] ([lisa.md](lisa.md) §7.1 records this interplay).
7. On eject (software UNCLAMP — the Sony drive has no eject button), the
   controller runs the drive's auto-eject (~750 ms), drops the clamped flag
   and interrupts; the OS field treats it as a disk-removal event [4]
   (`UnClamp`); [6].

## 5. Quirks & errata

- **Three drive-numbering conventions, one hardware.** The I/O ROM names the
  select values "drive 0 ($00, upper)" and "drive 80 ($80, lower)" [4]; the
  Boot ROM calls them "drive #1" ($00) and "drive #2" ($80) [3]; the hardware
  manual's Figure 2-7 prints "$00 = Drive 2 (lower), $80 = Drive 1 (upper)" [1]
  — reversed against both released sources. The sources win: every shipped
  program that drives the Sony writes $80 and calls it the lower/bottom drive
  [3] [6].
- **The status nibble is contentious.** The released I/O ROM source and its
  assembled December 1983 listing set bit 6 (with the OR bit 7) for a
  drive-$80 RWTS completion and bit 4 for its insertion — $C0 and $90 [4] [5];
  the OS driver decodes exactly that nibble as the bottom drive's events [6].
  The family page records a rev-H boot observation where a completion event
  arriving in bits 2–3 ($0C) boots successfully while bits 6–7 reset the early
  boot ([lisa.md](lisa.md) §13.3). Neither the boot ROM nor the loader tests
  IST bits at all — both wait on FDIR directly [3] [6] — so no shipped 68000
  boot code distinguishes the encodings; the discrepancy is unresolved and is
  Open question 1.
- **CLAMP is a null operation on the Sony board.** The command exists in the
  table ($81, code $09) and the boot ROM still issues it [3], but the
  Sony-generation firmware's clamp entry point is literally a single return
  instruction — the mechanism clamps itself on insertion [4] (`ClpEnty`). A
  Twiggy-era host's clamp command is silently accepted and does nothing.
- **The Sony drive has no eject button — but the button bits live on.** IST
  bits 1 and 5 (button pressed) and the $02/$20 clear masks exist in the
  register file for the Twiggy drives; on the Sony they can never fire, and
  the OS driver marks its button field "unused (SONY has no button)" [6]. The
  loader still tests the button bits before every read [6].
- **Eject is software.** There is no button; removal is the UNCLAMP RWTS
  command, which strobes the drive and waits ~750 ms for the auto-eject [4]
  (`UnClamp`). Hosts that "swap disks" unclamp and wait for the next
  disk-inserted event.
- **One parameter byte, three jobs.** $FCC003 is the RWTS command code, the
  interrupt mask, and the low byte of the $84 call address, depending on the
  GoByte [4]. Code that leaves a stale value there and issues the wrong
  command class gets the stale byte interpreted in the new role.
- **The interrupt-mask bits and the IST bits do not line up.** The mask gates
  a whole nibble with bits 3 and 7 — the same numeric positions as the IST's
  OR bits — while the per-event clear masks ($01/$02/$04, $10/$20/$40) are the
  event bits themselves [4]. Mixing the two roles of "mask" is the classic
  mistake against this interface.
- **An access during a locked-out window is a bus error by design.** DIS
  withholds DTACK until the processor board's watchdog faults the cycle [1]
  §6.2.2 — the same recoverable-bus-error path the Lisa uses for empty
  expansion slots ([lisa.md](lisa.md) §3.2). There is no retry inside the
  controller.
- **"Enable interrupts before touching the RAM, or bus-error."** The manual
  states the requirement flatly [1] §2.5.1 p. 2-13, all shipped software
  obeys it, but no released firmware or schematic text shows the gate; the
  mechanism is unknown (Open question 2).
- **The shared constants are unvalidated.** The 6504 copies its defaults on
  cold start and then trusts whatever the host writes — with the source's own
  never-never-land warning [4]. A bad MAXDDLY, STPDLY or speed table does not
  fail a validation; it misbehaves.
- **Error-counter addresses moved between ROM generations.** The Twiggy-era
  manual places the eight counters at 6504 $5B–$62 [1] Figure 6-9; ROM 88
  places them at $48–$4F [4], and the OS driver's offsets match the ROM [6].
  Software written against the manual's figure reads the wrong bytes on a
  Sony machine.
- **The manual's Figure 2-8 swaps the side/sector error codes** ($03 invalid
  sector, $04 invalid side) against the firmware's $03 invalid side, $04
  invalid sector [1]; [4]. The firmware is what a host actually receives.
- **DSKDIAG's documented port bit disagrees with the software.** The manual
  routes it to PB7 of the parallel-port VIA [1] §6.2.6; the boot ROM's equate
  and the loader both poll port B **bit 6** [3] [6] (Open question 9).
- **The I/O buffer's stated extent does not match the derived one.** Manual:
  transfer area $C501–$C7FF [1] §2.5.1 p. 2-14; derived from three programs:
  $C3E9–$C7FF (§2.7) (Open question 10).
- **A $84 call to $1FFB resets the coprocessor** — the firmware's own note on
  the call-address field [4]; the reset entry is the cold-start vector at the
  top of the program ROM.
- **The speed byte is also a busy flag.** Around the $84 call the family page
  records the host writing $FF to the speed byte, issuing the call, and
  polling the byte back to 0 for completion ([lisa.md](lisa.md) §13.2).
- **$88 and $89 are one-way doors.** WAIT IN ROM returns only via the $69/$96
  sequence on the GoByte address, and GO AWAY never returns at all — "cannot
  be stopped without resetting the Lisa" [1] §6.3.2. The boot ROM uses $89
  deliberately before soft power-off to protect parameter memory [3].

## 6. Open questions

1. **Which IST nibble does a real Lisa 2's Sony actually report in?** The
   released firmware source and its assembled listing say bits 4–7 (drive
   $80) [4] [5], and the OS driver decodes that nibble [6]; the recorded rev-H
   boot observation in [lisa.md](lisa.md) §13.3 required bits 0–3 and broke on
   bits 6–7. Since no boot-path 68000 code reads the byte, only a hardware
   capture of IST during a real read can settle it.
2. **The enable-before-access bus error.** What hardware (if any) makes an
   interrupt-disabled controller bus-error on RAM access, as the manual states
   [1] §2.5.1 p. 2-13? The DIS lockout is separately explained and gated
   differently (§3.3).
3. **The `Wide`/`Thin` tach tables.** Which drive variant does each of the two
   "empirically determined speed tables" [4] describe, and what physical
   quantity do the 16-bit bounds bound (pulse-period sums at 18 µs ticks over
   six pulses)? No source names the two drive populations.
4. **The IWM mode register's bit semantics.** `IWMMode = $1F` with the comment
   "asynch, 8 MHz, latch, 2 usec calls, no timer" [4] is the only description
   of the Sony-generation separator's mode word; a bit-level decode is not
   established here (the Macintosh-side IWM mode register is documented in
   [iwm-floppy.md](../../hardware/iwm-floppy.md) `Mode Register`, but the two
   have not been shown to be the same part).
5. **Which ROM generations place the error counters where.** Twiggy manual
   $5B–$62 versus ROM 88 $48–$4F (§5); the Twiggy-generation I/O ROM itself is
   not in the evidence set, so the drift cannot be dated.
6. **When does $FCC015 read 0?** The boot ROM comment maps 0 to "Twiggy" [3],
   but the Sony firmware initializes the byte to 1 or 2 only after sensing a
   drive [4]; the pre-sense and no-drive values are not pinned.
7. **Macintosh-disk interchange in practice.** The sector format and the
   single-sided block layout coincide (§4.5) and DISKID identifies a Macintosh
   disk [4], but whether any shipped Lisa software mounts Macintosh 400 KB
   volumes (rather than merely reading raw sectors) is not established by the
   sources here.
8. **The command-save ring's exact layout.** "64 byte ring buffer of 8 byte
   IOB's" [4] with the boot ROM's read-back offsets [3] does not fully pin the
   ring's base, index arithmetic or wrap behavior.
9. **DSKDIAG's port bit** — manual PB7 [1] versus software bit 6 [3] [6]
   (§5).
10. **The I/O buffer's documented lower bound** — manual $C501 versus the
    derived $C3E9/$C401 (§2.7).
11. **Sony-zone rotational speeds.** The manual gives rpm per zone only for
    the Twiggy format (Figure 6-11); no source states the Sony zones' rpm, so
    the correspondence between the PWM speed codes and physical speed is
    known only through the tach windows.
12. **The 6504A's interrupt configuration.** The firmware carries an
    "unexpected IRQ, NMI or BRK" error code [4], but the 6504A variant's
    available interrupt pins on this board are not documented in the evidence
    set; what the firmware actually guards against is inferred from the error
    name alone (*inferred — unverified*).

## References

1. Apple Computer, *Apple Lisa Computer: Hardware Manual*, April 1983 — §2.5.1 "Floppy Disk Control" (pp. 2-11 – 2-15, Figures 2-6 – 2-9), §6.2 "Floppy-Disk Controller" (pp. 6-2a – 6-24, Figures 6-1 – 6-5), §6.3 "Floppy-Disk Controller Operation" (pp. 6-31b – 6-45b, Figures 6-6a – 6-12), Chapter 9 "Floppy-Disk Drives" (pp. 9-1 – 9-4b, Figures 9-1 – 9-3).
2. Apple Computer (M. Baumwell), *Apple Lisa Computer: Hardware Manual 1983 — Errata*, May 1985 ("Macintosh XL Hardware Information", 16 May 1985) — I/O-space address correction; boot-ROM version display "H/88".
3. Apple Computer, *Apple Lisa Boot ROM source code, revision 2.48 ("H", RM248)* — officially released Lisa source archive; 68000 assembly modules RM248.B (boot-from-floppy path: `TWGBOOT`, `TWGRD`, `CMDCHK`, `CHKFIN`, `CLRFDIR`, `EJCTDSK`, burn-in), RM248.E (equates), and companion modules (self-test, machine-type detection).
4. Apple Computer, *Apple Lisa I/O ROM source code, version 88 — "Sony Driver for Lisa"* (6504 coprocessor firmware, December 1983) — officially released Lisa source archive; 6504 assembly modules `VAR`, `TABLES`, `CMD`, `LOOP`, `Interface`, `SEEK`, `RECAL`, `NPWM`, `NEWRWADDR`, `NREAD16`, `WRITE16`, `READ`, `WRITE`, `FORMAT`, `CLRTRK`, `CRECKSUM`, `VFYCKSUM`, `DENIBBLE`, `PRENIB`, `FR3TO1`, `WAITROM`, `HIMEM`, `SY`.
5. Apple Computer, *Apple Lisa I/O ROM 88 assembled listing*, December 1983 — contemporary assembly listing of the shipped coprocessor ROM; cited for byte-level verification of [4] (e.g. the `RWTS5` interrupt-status write, `LDA #$40 / ORA IST`).
6. Apple Computer, *Apple Lisa Office System source code* — officially released Lisa source archive; OS unit `SONY` (SOURCE-SONY.TEXT, R. Castro 1983, and SOURCE-SONYASM.TEXT, D. Offen / R. Castro) and the Sony boot loader-loader LDRLDR (source-ldmicro.TEXT).
