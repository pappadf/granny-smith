# Lisa parallel hard disk (ProFile / Widget) — implementation notes

Status: **device implemented + verified** (Step 7). The read/write/device-info
handshake is done (`src/core/peripherals/lisa_profile.{c,h}`), wired on VIA2 in
`src/machines/lisa.c`, and verified end-to-end through Apple's own boot-ROM driver
(see "Implemented & verified" below). COPS input and the Lisa-2 machine-type
detection are now done too; what remains for the *OS install* test is getting
LisaOS past **filesystem init** (error 10707) — see "Remaining" below. This doc
captures the protocol (from the rev-H boot ROM `RM248.B.TEXT` and the OS driver
`SOURCE-PROFILEASM`) and the implementation.

## Implemented & verified

- **`lisa_profile.{c,h}`** — behavioural ProFile controller. State machine driven
  only by the real VIA2 pins: CMD/ (PB4) edges sequence the phases; the device
  presents the state byte on the no-handshake port-A register and clocks
  command/status/data bytes on the handshaked register; BSY (PB1 + CA1) follows.
  Commands: read (0), write (1/2). On-wire block = **532 bytes (20-byte tag + 512
  data)**; a read streams 4 status bytes + the block, a write streams the block
  then (after a handshake) 4 status bytes. Block **$FFFFFF** returns the
  controller's synthesized **device-info block** (name "PROFILE", drive type 0,
  9728 blocks = 5 MB, 532 bytes/block) — the OS reads drive type at offset 14 and
  the 24-bit block count at offsets 18..20 (`PROF_INIT`). Backed by an in-RAM
  image (`nblocks × 532`), optionally persisted to a host file.
- **`via.c` port-A hooks** — `via_set_porta_hooks()` adds a read/write callback on
  the ORA/IRA (handshaked, CA2/PSTRB-strobed) and ORA-no-handshake registers, the
  one infrastructure gap. NULL on every other VIA, so no other machine changes.
- **`lisa.c` wiring** — VIA2 port-B output → CMD//DRW (computed from the true line
  levels, so an undriven open-collector CMD/ reads high, not a spurious assert);
  port-A hooks → the device; BSY → PB1 + CA1; OCD/ (PB0) driven low when a disk is
  attached; VIA1 PB7 (CRES/) falling edge → controller reset. A `profile` object
  (`profile.attach [path] [writable]` / `profile.detach` / `profile.present`)
  attaches an image (blank in-memory if no path).
- **Verification.** `tests/unit/suites/lisa_profile` drives the device API
  directly (detection, device-info read, write→read round-trip, decline/abort).
  `tests/integration/lisa-profile` boots the rev-H ROM, attaches a blank ProFile,
  and calls Apple's own **`PROREAD`** ($FE1F70) to read block $FFFFFF over the real
  VIA handshake — `PROREAD` returns D0=0 and the buffer holds "PROFILE"/9728/532.
  This exercises the ROM driver → VIA hooks → device → BSY/OCD path with no
  synthetic shortcuts. xl-boot (MacWorks→Finder) still passes — no regression.

## Remaining (for the OS install test)

- **COPS mouse + keyboard injection** — *done* (machine input hook → COPS;
  `keyboard.press` / `mouse.move` / `mouse.click`). Bytes flow end-to-end.
- **Machine-type detection** — *done* (the big one). LisaOS would not boot because
  the disk-controller ROM id (`$FCC031`) was zero → read as a Lisa 1 → the OS
  installed the **Twiggy** floppy driver on our **Sony** drive and startup
  stranded (the "scheduler hang"). `machine_lisa` now sets `$FCC031 = $A0`
  (`iob_sony`, Lisa 2/5) so the OS uses the Sony driver. See `docs/lisa.md` §16.2.
- **Three boot fixes landed (sessions 8–9) — boot now runs the full OS.** The old
  "scheduler hang" was **not** the machine type alone; it was masked by two CPU/IO
  bugs. (1) **FDC completion is now interrupt-driven (IPL 1) + deferred** — the OS
  Sony driver blocks on the completion interrupt, which our FDC previously only
  exposed as the pollable PB4 level *and* raised synchronously (racing the block).
  (2) **`STOP` now halts the CPU** — the OS scheduler's idle `Pause` (`STOP #$2000`)
  was a no-op that spun forever. (3) **The vertical-retrace status bit is now
  cycle-accurate + active-low** (VTIRDIS-cleared) — VIDTST passes and the OS clock
  runs real-time. With these, LOS 3.1 **loads the entire OS, passes the ROM video
  self-test, runs the OS scheduler healthily, and reads to the volume catalog
  (lba 28).** See `docs/lisa.md` §7.1/§7.4/§13 and `docs/lisa_fdc.md`.
- **Current blocker — FS-mount.** After the catalog read (lba 28) the OS
  **FS-reader process never runs to issue the next read** (the LisaEm reference
  reads lba 29 → writes lba 28 → continues to mount the whole disk; ours stops).
  Narrowed via a LisaEm go-byte command-trace oracle: the lba-29 read is issued by
  an OS process at seg96 `$c08fb4`, never reached in ours. **Ruled out:** the
  `$00C05F` drive-status bits (our drive-1 `$0C` is correct; LisaEm's internal
  `$C0` resets our early boot — see `docs/lisa.md` §13.3). This is the original
  error-10707 (`FS_Mount` / `nodiskpres`) issue, now reached in a *healthy* running
  system. **Next:** find why the FS-reader process is never scheduled (compare
  scheduler queues / PCB `blk_state` vs the LisaEm `$cc5a46`-woken-per-read trace;
  verify our segment-MMU slot assignment matches LisaEm's `$c0xxxx` before trusting
  those PCs). The auto-boot PM seed in `lisa_fdc_init` is a temporary debug aid
  (revert before commit; auto-boot needs a real PM `bootvol`, PM at `$FCC181`).
- Once it boots: drive the install → write the OS to the ProFile → reboot from it.
  The device write path is implemented and unit-tested but not yet OS-exercised.

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
