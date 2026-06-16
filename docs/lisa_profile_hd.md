# Lisa parallel hard disk (ProFile / Widget) — implementation plan

Status: **not yet implemented** (Step 7). This doc captures the protocol (extracted
from the rev-H boot ROM source, `RM248.B.TEXT`) and a concrete plan, so the device
can be built quickly. Goal: a Lisa 2 with a 10 MB ProFile so the Pascal Workshop
3.0 can install a Lisa OS onto it, with an `xl`-style integration test.

## Media reality (important)

- **Lisa Office System (1984)** — `local/lisa/LisaOS/Lisa Office System (1984)(Apple)(Disk {1..5} of 5).image`
  is **DiskCopy-4.2 with tags** (419284 B = 84 header + 409600 data + 9600 tags;
  block-0 tag FILEID `$AAAA`) and **boots** on `model=lisa` + rev-H ROM (disk 1: 323
  reads to track 58, runs loaded OS code at `$2Exxxx`). **This is the install OS to
  target** — the real Office System.
- **Pascal Workshop 3.0** (`local/lisa/images/Apple/Lisa/workshop_3.0/688-00xx*.dc42`,
  tagged DC42) also boots — a fallback install OS.
- Tagless trap: the *other* "Lisa Off Sys 30-31" images (inside
  `Apple Lisa Software - Disk Images_vol_3.zip`) are **raw 409600 = 800×512, no tag
  section** → they hit the boot-failure recovery menu. The Lisa OS *requires* tags
  (real FS data, not synthesisable). Always use a 419284-byte DC42 image, not a
  409600-byte raw one.
- Build the rev-H ROM by interleaving `firmware/341-0175-H.BIN` (even) +
  `341-0176-H.BIN` (odd) → version `$0248` at offset `$3FFC`.

## Detection is trivial — it's the OCD line

`CHKPROFILE` → `PROINIT` → the entire "attached?" test is one bit:
`BTST #OCD,IRB2(A0)` (VIA2 **PB0**). OCD=0 ⇒ connected ⇒ attached. Driving
`via_input(via2, 1, /*PB0*/0, false)` (and BSY idle, below) makes the ROM list the
ProFile as a boot device and try to boot block 0 from it. So *detection* is a
two-line stub; the real work is the read/write **handshake + block I/O**.

## VIA2 pin map (docs/lisa.md §14)

| Line | Signal | Dir | Polarity / notes |
| --- | --- | --- | --- |
| VIA2 PA0–7 | data | bidir | byte; `IRA2` read / `ORA2` write |
| VIA2 CA2 | PSTRB/ | out | **pulse mode** (PROINIT sets PCR2 CA2 pulse/+edge); pulses once per port-A access — this is the per-byte strobe |
| VIA2 PB1 / CA1 | BSY | in | **active-low**: 0 = busy, 1 = not-busy |
| VIA2 PB0 | OCD | in | 1 = disconnected, **0 = connected** |
| VIA2 PB3 | DRW | out | 1 = read, 0 = write |
| VIA2 PB4 | CMD/ | out | **0 = command asserted**, 1 = deasserted |
| VIA2 CB2 | PARITY/ | in | latched |
| VIA1 PB7 | CRES/ | out | controller reset (active low) |
| VIA1 PB5 | DIAGPAR | out | diagnostic parity |

## Protocol (from RM248.B)

**Handshake / command-completion (`FINDD2`)** — assumes A0 = VIA2:
1. `ANDI #$EF,ORB2` → CMD/ = 0 (assert); `DDRA2 = 0` (port A input).
2. `WFBSY`: wait BSY → 0 (busy). **Device drives BSY low to acknowledge CMD/.**
3. `GETRSP`: read `PORTA2` (no handshake) → compare to **D2 = expected state byte**.
   The device presents this on port-A input. (For the read path the expected
   sync byte is `$01` — see `STAT01` in `STRTRD`.)
4. Reply: `SENDRSP` — `DDRA2=$FF`, `MOVE.B D3,PORTA2` (`$55` = OK, `$00` = error),
   `ORI #$10,ORB2` (toggle CMD/).
5. `WFNBSY`: wait BSY → 1 (not busy). **Device raises BSY when done.**

**Read (`STRTRD`/`READIT`)**:
- Build `CMDBUFR` = 6 bytes: `cmd`($00 read), `block`(24-bit BE), `retry`, `threshold`.
- `STAT01`: sync (`FINDD2` with expected `$01`) and send the 6-byte command (each
  byte via `SENDRSP`-style port-A write while CMD/ asserted).
- Status phase, then `READIT`: `MOVE.B IRA2(A0),(A1)+` per byte (×N). Each `IRA2`
  read pulses CA2/PSTRB — the device must present the **next byte** on each read.

**Write** is the mirror (host drives port A out per byte). **Reset**: pulse VIA1
PB7 (CRES/) low (`DOCRES`). **Block size**: 512 data + tag (532–536 on the wire);
back the image with 512+tag per block (BLU-style) since the Lisa FS is tag-bearing.

## The one infrastructure gap

`via.c`'s `ORA_IRA` (reg 1) read has **no device callback** — so the ProFile can't
advance to the next byte on each data-read. Add a read hook (e.g. a
`via_porta_read_fn` invoked from the `ORA_IRA` case, or a CA2-pulse callback) so the
device supplies bytes during the read data-phase. The command/response/write phases
already work through the existing `output_cb` (fires on port-A/B writes).

## Implementation steps

1. `via.c`: add a port-A-read (IRA) hook; wire it on the Lisa VIA2.
2. `lisa_profile.{c,h}`: behavioural device — state machine (IDLE → CMD → status →
   data-read / data-write), backed by an `image_t` (10 MB = ~9728 × (512+tag)).
   Drive BSY (PB1/CA1) and port-A input; consume the 6-byte command; service block
   read/write. Detection = OCD already-connected.
3. `lisa.c`: wire VIA2 PB0/PB1/CA1/CA2 + the read hook to the device; add it to the
   `floppy`-style object surface or a `hd` object so a test/UI can attach an image.
   Create a blank 10 MB ProFile image (storage.hd_create-style).
4. Bring-up milestones: (a) ROM lists the ProFile in "STARTUP FROM…"; (b) ROM reads
   block 0; (c) Workshop boots from floppy and *sees* the ProFile.
5. **COPS keyboard injection** (separate, also needed): the Workshop install is
   keyboard-driven; today the COPS only does autonomous polling. Add host→COPS key
   events (present scancodes on VIA1 port A + pulse CA1, like the mouse path).
6. `tests/integration/xl-install` (or `lisa-install`): boot the Workshop floppy,
   attach a blank 10 MB ProFile, drive the install (keyboard + disk swaps), then
   verify the HD is bootable / `screen.match` the installed desktop.

## References

- Protocol routines in `local/lisa/AppleLisa - Boot ROM Source/Lisa Boot ROM RM248.B.TEXT`:
  `PROINIT`, `FINDD2`, `WFBSY`/`WFNBSY`, `SENDRSP`, `READIT`, `STRTRD`/`STAT01`, `DOCRES`.
- docs/lisa.md §14 (parallel HD), §10 (VIA2).
