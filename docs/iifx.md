# Macintosh IIfx

## Introduction

The Macintosh IIfx (1990) is Apple's high-end 68030 machine: a 40 MHz
68030 + 68882 FPU paired with a cluster of custom support chips that set
it apart from the earlier Mac II family. Two of those chips are **I/O
Processors** (IOPs) that off-load serial and floppy/ADB I/O from the
68030; the SCSI core is wrapped by an Apple **SCSI DMA** chip that adds a
bus-master DMA engine to the stock NCR 5380; interrupts are arbitrated by
the **OSS** chip and memory/ROM decoding by the **FMC**; and an *optional*
RAM Parity Unit (RPU) may decode at `$50F1E000`.

---

## I/O Processors (IOPs)

The IIfx off-loads serial and floppy/ADB I/O onto two I/O Processors. This
chapter covers what an IOP is (§1), the host-side register and shared-RAM
interface plus the mailbox message protocol common to both (§§2–14), and
then each IOP in turn: the SCC IOP (§15) and the SWIM IOP with its
SonyIOP-floppy (§§16–17) and ADB (§18) sub-protocols. §19 documents the RPU
probe that POST interleaves with IOP bring-up.

### 1. What an IOP is

Each IOP is a self-contained microcontroller (Apple part 343S1021)
sitting between the 68030 and one front-side peripheral. Internally it
contains a 65C02 core and 32 KB of dual-ported shared RAM. The 68030
exchanges data with the IOP exclusively through that shared RAM and a
handful of memory-mapped control registers — the 65C02 itself is
invisible to host software and to this document. The IOP's firmware is
not built into the chip; it is a discrete `'iopc'` resource downloaded
from the boot ROM into the shared RAM on every cold boot.

Apple software supports up to 8 IOPs per system (`MaxIopNum equ 7).
The IIfx ships with exactly two:

| Apple ID                | Front-side device                              | OSS source bit                           |
| ----------------------- | ---------------------------------------------- | ---------------------------------------- |
| `SccIopNum  equ 0`      | Zilog Z8530 SCC — dual-channel serial          | `OSSIntIOPSCC  equ 7` (bit 7)            |
| `SwimIopNum equ 1`      | SWIM1 floppy controller + ADB single-wire bus  | `OSSIntIOPSWIM equ 6` (bit 6)            |

The Apple Sound Chip (ASC) is **not** routed through an IOP on the IIfx
— it has its own OSS source and is reached directly by the 68030.

---

### 2. Host address map

The IOPs are mapped into the IIfx's I/O space:

| Range                            | IOP        | Notes                                                                                                                                       |
| -------------------------------- | ---------- | ------------------------------------------------------------------------------------------------------------------------------------------- |
| `$50F04000 — $50F05FFF`           | SCC IOP    | $2000-byte aperture; only the first few bus-byte offsets are used; remainder of the window mirrors those (see §3).                          |
| `$50F12000 — $50F13FFF`           | SWIM IOP   | Same layout as SCC IOP, different front-side device wiring.                                                                                  |

Every byte in the I/O space is also mirrored at the same offset within
each subsequent 256 KB block, per the IIfx's normal I/O-space aliasing.
This document quotes the canonical addresses; mirrors are transparent.

Each IOP's `hint` interrupt output is wired to a dedicated OSS source
(see §11). The OSS arbitrates priorities across all interrupt sources
and drives the 68030's autovector input.

---

### 3. The PIC register window

The IOP's host-visible interface is an 8-bit register file laid out
across one byte lane of the 32-bit bus. Each logical register therefore
appears at every-other-byte offsets within the $2000-byte aperture, with
two byte addresses aliased onto the same register (so `$50F12008` and
`$50F12009` both hit `iopRamData`).

Bus-byte offsets within the aperture:

| Apple name        | Bus offset | R/W   | Purpose                                                                                          |
| ----------------- | ---------- | ----- | ------------------------------------------------------------------------------------------------ |
| `iopRamAddrH`     | `+$0000`   | R/W   | High byte of the 16-bit shared-RAM address pointer.                                              |
| `iopRamAddr`      | `+$0001`   | R/W   | The same pointer as a word (high byte aliased to `$0001`, low byte at `$0002`).                  |
| `iopRamAddrL`     | `+$0002`   | R/W   | Low byte of the shared-RAM address pointer.                                                      |
| `iopStatCtl`      | `+$0004`   | R/W   | Status on read, command bits on write. See §4.                                                   |
| `iopRamData`      | `+$0008`   | R/W   | Read/write the shared-RAM byte at `iopRamAddr`; pointer auto-increments if `iopIncEnable` is set.|
| Peripheral passthrough | `+$0020..+$003F` | R/W | **Bypass mode only**. 16 byte-wide slots that pass directly through to the front-side device. See §12. |

The address decoder inside the chip uses the upper bits of the offset:

- Bit 5 set → peripheral passthrough.
- Bit 3 set → `iopRamData`.
- Bit 2 set → `iopStatCtl`.
- Bit 1 set → `iopRamAddrL`.
- Otherwise → `iopRamAddrH`.

`IOPMgr` never touches these offsets directly. It records absolute
addresses of `iopRamAddr`, `iopRamData`, and `iopStatCtl` for each
installed IOP in the per-IOP `IOPInfo` block, using the field names
`IopAddrRegPtr`, `IopDataRegPtr`, and `IopCtlRegPtr`.

---

### 4. `iopStatCtl`

The single most important register. The same byte is both read and
written, but with different semantics per bit.:

```
iopInBypassMode  equ 0   ; IOP is in BYPASS mode
iopIncEnable     equ 1   ; enable iopRamAddr auto-increment
iopRun           equ 2   ; 0 -> reset IOP, 1 -> run IOP
iopGenInterrupt  equ 3   ; interrupt (kick) the IOP
iopInt0Active    equ 4   ; IOP→host interrupt source 0
iopInt1Active    equ 5   ; IOP→host interrupt source 1
iopBypassIntReq  equ 6   ; peripheral /INT in bypass mode
iopSCCWrReq      equ 7   ; SCC WREQ live signal (0 = active)
```

#### 4.1 Per-bit behaviour

| Bit | Apple name          | Read meaning                                                                                  | Write meaning                                                                                                          |
| --- | ------------------- | --------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------- |
| 0   | `iopInBypassMode`   | 1 iff the IOP is in bypass mode (firmware mirrors its own bypass bit here). 0 otherwise.       | Ignored.                                                                                                              |
| 1   | `iopIncEnable`      | Address auto-increment is enabled for `iopRamData`.                                            | Stored. Controls whether reads/writes to `iopRamData` post-increment `iopRamAddr`.                                     |
| 2   | `iopRun`            | 1 = 65C02 is running, 0 = held in reset.                                                       | Stored. **Edge-detected**: the reset line is re-driven only on a 0↔1 transition of this bit.                            |
| 3   | `iopGenInterrupt`   | Always reads 0 (bit not stored).                                                               | Writing 1 generates a host-kick interrupt inside the IOP. **Not stored**; re-fires on every write of 1.                  |
| 4   | `iopInt0Active`     | 1 if the IOP has raised host-interrupt source 0.                                               | **Write-1-to-clear**. When both `iopInt0Active` and `iopInt1Active` fall to 0, the IOP's `hint` line to OSS deasserts. |
| 5   | `iopInt1Active`     | 1 if the IOP has raised host-interrupt source 1.                                               | **Write-1-to-clear**.                                                                                                  |
| 6   | `iopBypassIntReq`   | Live peripheral `/INT` request. Read-only. Active only in bypass mode; otherwise reads 0.       | Read-only.                                                                                                            |
| 7   | `iopSCCWrReq`       | Live SCC `WREQ` / device `/REQ` signal. Active-low (`0 = active`). Forced 1 outside bypass.    | Read-only.                                                                                                            |

Important details:

- Bits 0, 6, and 7 are status signals owned by the chip; the host
  cannot influence them by writing.
- Bit 3 is fire-and-forget. A write of `iopGenInterrupt` triggers a
  single edge inside the chip; the bit never reads back as 1.
- Bits 4 and 5 are W1C. To leave them untouched, write 0 to those
  positions. To clear, write 1. The host can never set them — only the
  firmware can.

#### 4.2 Composed command bytes

The canonical command bytes the host writes to `iopStatCtl` includes `iopRun`/`iopIncEnable`
plus the appropriate W1C bits so that the write does not accidentally clear unrelated interrupt-active state:

| Command          | Value | Function                                                                                                |
| ---------------- | ----- | ------------------------------------------------------------------------------------------------------- |
| `setIopIncEnable`| `$06` | Enable address-pointer auto-increment, keep IOP running.                                                |
| `clrIopIncEnable`| `$04` | Disable auto-increment, keep IOP running.                                                               |
| `clrIopInt0`     | `$16` | Acknowledge `iopInt0Active`, keep IOP running with auto-increment.                                       |
| `clrIopInt1`     | `$26` | Acknowledge `iopInt1Active`, keep IOP running with auto-increment.                                       |
| `setIopGenInt`   | `$0E` | Kick the IOP (host-side host-kick interrupt). Bit 3 is edge-only; this byte may be written repeatedly.   |
| `resetIopRun`    | `$32` | Hold IOP in reset, clear any pending host-interrupt-active bits, enable auto-increment.                  |
| `setIopRun`      | `$36` | Release IOP from reset, clear any pending host-interrupt-active bits, enable auto-increment.            |

These exact constants are the only command bytes that should ever be written to `iopStatCtl`.

---

### 5. Shared RAM access via `iopRamAddr` / `iopRamData`

The host reads and writes the IOP's 32 KB shared RAM through the
register window. The 16-bit shared-RAM pointer is `iopRamAddr` (with
its high and low halves at `iopRamAddrH`/`iopRamAddrL`). The byte at
that address is read or written through `iopRamData`.

When `iopIncEnable` is set in `iopStatCtl`, every access to
`iopRamData` post-increments `iopRamAddr`. This is the auto-increment
mode IOPMgr uses for all bulk transfers; without it the host would have
to set `iopRamAddr` before every single byte.

Apple-canonical IOP has 32 KB shared RAM

The pointer is a 16-bit register, so it can address `$0000-$FFFF`. The
firmware-side 65C02 sees its `$8000-$FFFF` range as a mirror of
`$0000-$7FFF` plus a small MMIO window — but the host's view through
`iopRamData` is shared RAM only, indexed by the full 16 bits.

---

### 6. Shared-RAM layout (Apple's firmware convention)

Apple uses a fixed shared-RAM layout common to every `'iopc'` firmware
image. The host-visible parts are:

| Range                    | Apple symbol         | Role                                                                                                |
| ------------------------ | -------------------- | --------------------------------------------------------------------------------------------------- |
| `$0000 — $01FF`          | (private)            | 65C02 zero-page and stack; opaque to the host.                                                       |
| `$0200`                  | `IOPXmtMsgBase`      | `XmtMsg[0].state` byte. Seeded by the firmware to the **active slot count** (image-specific — see below). |
| `$0201 — $0207`          | (`XmtMsg[1..7].state`) | Per-slot state for the seven transmit slots (host → IOP messages). Encodings from §7.              |
| `$021F`                  | `PatchReqAddr`       | Soft-restart request byte. See §14.                                                                  |
| `$0220 — $023F`          | `XmtMsg[1].msg`      | 32-byte payload of transmit slot 1.                                                                  |
| `$0240 — $025F`          | `XmtMsg[2].msg`      | 32-byte payload of transmit slot 2.                                                                  |
| …                        | …                    | …                                                                                                   |
| `$02E0 — $02FF`          | `XmtMsg[7].msg`      | 32-byte payload of transmit slot 7.                                                                  |
| `$0300`                  | `IOPRcvMsgBase`      | `RcvMsg[0].state` byte. Same seeding convention as `XmtMsg[0]`.                                       |
| `$0301 — $0307`          | (`RcvMsg[1..7].state`) | Per-slot state for the seven receive slots (IOP → host messages).                                    |
| `$031F`                  | `IOPAliveAddr`       | Firmware-alive heartbeat. Seeded `$FF` by the host before reset is released; firmware then maintains it. See §8. |
| `$0320 — $0339`          | `RcvMsg[1].msg`      | 32-byte payload of receive slot 1. (Apple stride is 32; only the first 10 bytes are usually used.)   |
| `$0340 — $035F`          | `RcvMsg[2].msg`      | 32-byte payload of receive slot 2.                                                                   |
| …                        | …                    | …                                                                                                   |
| `$03E0 — $03FF`          | `RcvMsg[7].msg`      | 32-byte payload of receive slot 7.                                                                   |
| `$0400 — $7FFF`          | (firmware)           | 65C02 code, lookup tables, working buffers. Layout depends on `'iopc'` image; not host-visible.       |

**Active slot count is image-specific.** The slot-count seed at `$0200` / `$0300` is written exactly once per cold boot, inside the per-image init routine:

| Image     | `XmtMsg[0]` / `RcvMsg[0]` seed | Active slots                                       | Source                                                |
| --------- | ------------------------------ | -------------------------------------------------- | ----------------------------------------------------- |
| SWIM IOP  | `$03`                          | 1 (BlockCopy, RcvMsg only), 2 (Sony), 3 (ADB)      | `SWIMIOP.aii` `InitDrivers`, writes `RCVMsgMax`/`XMTMsgMax` |
| SCC IOP   | `$07`                          | 1 (kernel commands, XmtMsg only)                   | `SCCIOP.aii` `Reset`, writes `RxMsgCnt`/`TxMsgCnt`     |

Slots above the seeded count *exist* in shared RAM but the firmware will halt (BRK loop) if a host ever kicks them on the SWIM IOP. The SCC IOP's higher slots are reserved for host-downloaded serial-driver code — port A on slots 2-4, port B on slots 5-7 (see §15.3).

```
MaxIopMsgNum    equ 7         ; transmit / receive slots are numbered 1..7
MaxIopMsgLen    equ 32        ; up to 32 bytes per slot
```

So each direction has seven 32-byte mailbox slots, with state bytes
packed in the eight-byte block immediately following the base symbol
(`$0200-$0207` for XmtMsg, `$0300-$0307` for RcvMsg).

#### 6.1 Computing slot addresses

For a slot number `N` (1..7) and a base symbol `Base` (`$0200` or
`$0300`):

- State byte:  `Base + N`
- Payload base: `Base | (N << 5)`  (= `Base + N * 32`)

For example, `XmtMsg[3].state = $0203` and `XmtMsg[3].msg = $0260`.

---

### 7. Message states

```
MsgIdle      equ 0     ; slot is unused
NewMsgSent   equ 1     ; sender posted; awaiting processing
MsgReceived  equ 2     ; processor claimed the slot; in flight
MsgCompleted equ 3     ; reply available; receiver should drain it
```

The state of a slot is owned by whoever is currently "handling" it: a
slot in `NewMsgSent` belongs to its handler until that handler advances
it to `MsgReceived` or `MsgCompleted`. The host and the IOP firmware
never write the same state byte concurrently.

---

### 8. Alive handshake

The IOP manager polls `IOPAliveAddr` (`$031F`) for non-zero after releasing
the IOP from reset, with a timeout loop. To get the very first iteration
to pass, the host pre-seeds `$031F = $FF` while the IOP is still held in
reset (see §9 step 4) — that byte is therefore "alive" as soon as the
manager looks, regardless of whether the firmware has begun executing.

After reset is released, the firmware's main loop overwrites the byte
on every iteration with an image-specific sentinel:

| Image     | Runtime heartbeat value | Written from                                                        |
| --------- | ----------------------- | ------------------------------------------------------------------- |
| SWIM IOP  | `'X'` (`$58`)           | `IdleLoop` in `IOPKernel.aii` — once per idle-task scan.            |
| SCC IOP   | `$FF`                   | `Ev_Wait` task scheduler in `SCCIOP.aii` — once per task switch.    |

So the host's actual liveness check is "`$031F` is non-zero **and** is
being refreshed". An IOP that has wedged (BRK landing in
`Unknown_Int` / `IdleStackError` / `IdleRegError`) will leave the byte
frozen at whatever it last wrote — most likely the heartbeat value
itself — so a single read cannot distinguish "alive" from "wedged".
A real driver re-reads after a delay; an emulator should refresh the
byte on every guest fetch from the appropriate firmware path so that
guests using polling-with-timeout detect liveness correctly.

---

### 9. Code load protocol

The IOP firmware ships as a `'iopc'` resource in the IIfx boot ROM,
indexed by IOP number (`SccIopNum` = 0, `SwimIopNum` = 1). On install,
the IOP manager:

1. **Hold the IOP in reset.**
   Write `resetIopRun` ($32) to `iopStatCtl`. This also turns on
   `iopIncEnable`.

2. **Zero-fill, complement, verify all 32 KB of shared RAM.**
   For each address `$0000..$7FFF`, write `$FF` (via
   `iopRamAddr` + `iopRamData`). Then complement (write back `$00` after
   reading each byte and verifying it read as `$FF`). Then read back and
   verify `$00`. Any mismatch is a hardware fault.

3. **Download the `'iopc'` resource.**
   Each segment is a stream of `(size_byte, load_addr_word, data...)`
   triples, terminated by `size_byte = 0`. For each segment the loader
   sets `iopRamAddr` to the segment's load address and writes the data
   bytes via `iopRamData` (auto-increment on). After the data is
   written, the loader reads it back and verifies.

4. **Pre-seed the alive byte.**
   Write `$FF` to `IOPAliveAddr` (`$031F`). This satisfies the
   `@AliveWaitLoop` check above on the very first iteration before the
   firmware has had a chance to run.

5. **Release the IOP from reset.**
   Write `setIopRun` ($36) to `iopStatCtl`. The 65C02 fetches its reset
   vector (mirrored from the top of shared RAM) and begins executing.

6. **Wait for alive.**
   Poll `IOPAliveAddr` until it reads `$FF` (or the firmware's
   replacement heartbeat byte).

After step 6 the IOP is ready to accept mailbox traffic.

---

### 10. Mailbox protocol — Transmit (host → IOP)

The host sends a request via `_IOPMsgRequest` with `irRequestKind =
irSendXmtMessage`. The end-to-end flow:

1. **Caller builds an `IOPRequestInfo`**:

   ```
   irQLink       ds.l 1    ; queue link (used by IOPMgr internally)
   irQType       ds.w 1    ; queue-element type
   irIOPNumber   ds.b 1    ; SccIopNum or SwimIopNum
   irRequestKind ds.b 1    ; irSendXmtMessage = 0
   irMsgNumber   ds.b 1    ; 1..7
   irMessageLen  ds.b 1    ; 0..32
   irReplyLen    ds.b 1    ; 0..32
   irReqActive   ds.b 1    ; written $FF by the trap, $00 by IOPInterrupt on completion
   irMessagePtr  ds.l 1    ; host-side buffer with the outgoing bytes
   irReplyPtr    ds.l 1    ; host-side buffer to receive the reply
   irHandler     ds.l 1    ; completion callback (or 0)
   ```

   `irRequestKind` values:

   ```
   irSendXmtMessage   equ 0    ; send an XmtMsg, read reply when done
   irSendRcvReply     equ 1    ; reply to a pending RcvMsg
   irWaitRcvMessage   equ 2    ; install a waiter for an RcvMsg
   irRemoveRcvWaiter  equ 3    ; cancel a waiter
   ```

2. **Caller invokes `_IOPMsgRequest`** (`A0 = IOPRequestInfo`).
   IOPMgr's trap implementation:

   1. Marks `irReqActive = $FF`.
   2. Enqueues the request on the per-slot transmit queue
      (`XmtMsgQHdr`).
   3. If this request is now at the head of the queue (queue had been
      empty), sends immediately. Otherwise leaves it for
      `IOPInterrupt` to dequeue when the current head completes.

3. **Immediate-send path** (`@SendAndInterrupt`):

   1. Save the current `iopRamAddr` (so the trap leaves it as it
      found it).
   2. Set `iopRamAddr = XmtMsg[N].msg` (= `$0200 | (N << 5)`).
   3. Copy `irMessageLen` bytes from `irMessagePtr` to `iopRamData`
      (auto-increment).
   4. Set `iopRamAddr = XmtMsg[N].state` (= `$0200 | N`).
   5. Write `iopRamData = NewMsgSent` (= `$01`).
   6. Restore the saved `iopRamAddr`.
   7. Write `iopStatCtl = setIopGenInt` (= `$0E`) — kicks the IOP.

4. **IOP firmware processes the request.**
   The firmware's host-kick handler walks `XmtMsg[1..7].state` looking
   for `NewMsgSent`, runs the appropriate per-slot handler, writes the
   reply bytes back into the same `XmtMsg[N].msg` buffer, sets
   `XmtMsg[N].state = MsgCompleted`, and asserts `iopInt0Active` (via
   its on-die host-interrupt-request register, which feeds directly to
   `iopStatCtl` bit 4 from the host's point of view). The IOP's `hint`
   line to OSS goes active.

5. **OSS raises the corresponding 68030 IRQ level.**
   Source 6 (SWIM IOP) or source 7 (SCC IOP); the priority Apple
   programs is in `OSSMskPSwm` / `OSSMskPScc` (default 1 and 4
   respectively on IIfx).

6. **Host `IOPInterrupt` handler runs.**
   It reads `iopStatCtl`, finds `iopInt0Active`, walks
   `XmtMsg[1..7].state` for `MsgCompleted`, copies `irReplyLen` bytes
   from `XmtMsg[N].msg` to the caller's `irReplyPtr`, sets
   `XmtMsg[N].state = MsgIdle`, sets `irReqActive = 0`, advances the
   queue (issuing the next queued request to the IOP), and writes
   `iopStatCtl = clrIopInt0` (`$16`) to acknowledge.
   If `irHandler` is non-null, the handler is called.

7. **Caller observes completion** either by polling `irReqActive` or
   by being called from `irHandler`.

The key invariants:

- `XmtMsg[N].state` transitions: `MsgIdle` → `NewMsgSent` (set by host)
  → `MsgReceived` (transiently, optional) → `MsgCompleted` (set by IOP)
  → `MsgIdle` (set by host).
- `iopInt0Active` is **always** the wake-up signal for completion of a
  host-initiated request. The firmware's internal `MsgCompletedINT`
  equate is bound to `INTHST0`, and every "I finished consuming an
  XmtMsg, here is your reply" code path raises exactly this bit.
  Always acknowledged with `clrIopInt0`.
- The host always restores `iopRamAddr` before kicking the IOP, so
  concurrent host code that's mid-transfer doesn't see surprises.

---

### 11. Mailbox protocol — Receive (IOP → host)

The opposite direction is more involved because the host must
"subscribe" to a receive slot before the IOP fills it. Used for
unsolicited notifications: ADB key events, mouse movement, disk-change
events, etc.

1. **Host subscribes.** Calls `_IOPMsgRequest` with `irRequestKind =
   irWaitRcvMessage`, supplying the slot number, reply buffer, and
   completion handler. IOPMgr records the waiter in
   `IOPMsgEntry.RcvMsgInfoPtr` for that slot; no IOP traffic occurs.

2. **IOP firmware decides to send.** It writes its payload into
   `RcvMsg[N].msg`, sets `RcvMsg[N].state = NewMsgSent`, and asserts
   `iopInt1Active`. The firmware's `NewMsgSentINT` equate is bound to
   `INTHST1`, and **every** firmware-initiated RcvMsg post raises
   exactly this bit. The "Int0 = reply, Int1 = unsolicited" framing is
   not a heuristic — it's the firmware's hard rule, with one nuance on
   the ADB slot (see §18.4).

3. **OSS raises the 68030 IRQ.**

4. **Host `IOPInterrupt` handler runs.** It walks
   `RcvMsg[1..7].state` for `NewMsgSent`, copies `irReplyLen` bytes
   from `RcvMsg[N].msg` to the registered `irReplyPtr`, advances the
   state to `MsgReceived`, runs the registered `irHandler`, and writes
   `iopStatCtl = clrIopInt1` (`$26`) to acknowledge.

5. **Host eventually sends a reply.** Calls `_IOPMsgRequest` with
   `irRequestKind = irSendRcvReply` for the same slot. IOPMgr copies the
   reply payload into `RcvMsg[N].msg`, sets `RcvMsg[N].state =
   MsgCompleted`, and writes `iopStatCtl = setIopGenInt` to kick the
   IOP. The firmware's host-kick handler sees the `MsgCompleted` state,
   knows the host has consumed the original message, processes any
   reply data, and clears the state back to `MsgIdle` (ready for the
   next notification on this slot).

`RcvMsg[N].state` transitions: `MsgIdle` → `NewMsgSent` (IOP) →
`MsgReceived` (host) → `MsgCompleted` (host, via `irSendRcvReply`) →
`MsgIdle` (IOP).

---

### 12. OS Manager API

Three OS traps are exposed by IOPMgr:

| Trap                | Trap number | Parameter block      | Function                                                                                |
| ------------------- | ----------- | -------------------- | --------------------------------------------------------------------------------------- |
| `_IOPMsgRequest`    | `$A087`     | `IOPRequestInfo`     | Send/receive a message, or install/cancel a receive waiter. See §10 and §11.            |
| `_IOPMoveData`      | `$A088`     | `IOPMoveInfo`        | Bulk move/compare/patch bytes between host RAM and IOP shared RAM.                       |
| `_IOPInfoAccess`    | `$A086`     | `IOPAccessInfo`      | Install / look up / remove an IOP (registers the firmware and per-slot dispatch tables). |

`IOPMoveInfo`  supports four copy directions via `imCopyKind`:

```
imIopToHost   equ 0    ; IOP RAM → host RAM
imHostToIop   equ 1    ; host RAM → IOP RAM
imCompare     equ 2    ; compare IOP RAM with host RAM
imPatchIop    equ 3    ; atomic patch of IOP RAM (no length)
```

All three traps internally use `iopRamAddr`/`iopRamData` for the data
movement and `iopStatCtl` for the kick/ack handshake. Clients of
`IOPMgr` never touch the PIC register window directly.

The "kernel" mailbox channel on slot 1 of each IOP is reserved by
`IOPMgr` for its own use, but its **direction and protocol differ per
image** — slot 1 is not a single uniform channel:

- **SWIM IOP slot 1** is firmware-initiated (`RcvMsg[1]` on the
  host's side). The firmware uses it to ask the host to perform a
  block copy between IOP shared RAM and main RAM — this is how all
  bulk floppy data actually crosses the host/IOP boundary. The payload
  is a `MoveReqInfo`-shaped BlockCopy descriptor (see §16.1).
- **SCC IOP slot 1** is host-initiated (`XmtMsg[1]` on the host's
  side). The host posts one of six fixed kernel commands (AllocDvr,
  DeAllocDvr, InitDvr, ByPass, Versn, SCCCntl) and the firmware writes
  its reply back into the same payload buffer (see §15.1).

Slot 1 in the opposite direction is `Unimplemented` (BRK loop) on both
images and must not be kicked.

---

### 13. Interrupt path to the 68030 (OSS)

Each IOP's `hint` line goes to a dedicated bit of the OSS pending
register, with a separate per-source priority byte. The relevant
equates:

| OSS source bit | Apple name        | IIfx wiring                            |
| -------------- | ----------------- | -------------------------------------- |
| 6              | `OSSIntIOPSWIM`   | SWIM IOP `hint`                        |
| 7              | `OSSIntIOPSCC`    | SCC IOP `hint`                         |

OSS arbitration: on every change to the pending bitmap or any
per-source priority byte, the OSS scans all currently-pending sources,
finds the maximum priority among them, and drives that autovector level
on the 68030. Lower-priority pending sources don't fire until the
higher-priority ones clear.

The per-source priority registers are named according to which source
they govern:

- `OSSMskPScc` — priority byte for `OSSIntIOPSCC` (SCC IOP).
- `OSSMskPSwm` — priority byte for `OSSIntIOPSWIM` (SWIM IOP).

A priority byte of 0 disables that source entirely (it can become
pending but will never drive the 68030's autovector). The IIfx ROM
programs `OSSMskPSwm = 1` and `OSSMskPScc = 4` early in boot.

The IOP `hint` line stays asserted as long as either `iopInt0Active`
or `iopInt1Active` is set in `iopStatCtl`. Writing `clrIopInt0` /
`clrIopInt1` to `iopStatCtl` is what eventually deasserts `hint` and
clears the OSS pending bit.

---

### 14. Soft-patch via `PatchReqAddr`

`$021F` in shared RAM is reserved for a "host wants to patch IOP
memory" handshake. It is **not** a soft restart — internal state is not
reinitialised. The handshake quiesces the IOP into a known idle state
so the host can rewrite firmware bytes through `iopRamAddr` /
`iopRamData` without racing the executing code.

The byte at `$021F` is itself the handshake state, encoded with the
same `Msg*` values from §7:

1. **Host requests patch.** Writes `NewMsgSent` (`$01`) to `$021F`
   via `iopRamAddr` + `iopRamData`. (Any non-zero value triggers the
   transition — `NewMsgSent` is the canonical one.)

2. **Firmware enters PatchWait.** On the next iteration of `IdleLoop`
   (SWIM IOP) the firmware reads `$021F`, sees non-zero, and jumps to
   the fixed-address trampoline `PatchWait` just below the interrupt
   vectors. PatchWait disables interrupts (`SEI`), then writes
   `MessageCompleted` (`$03`) to `$021F`.

3. **Host observes "ready to patch".** Poll `$021F` until it reads
   `$03`. At this point the IOP is parked in a 3-instruction loop with
   interrupts disabled, holding all registers, and no other code is
   running — the host can freely rewrite any byte in shared RAM.

4. **Host patches and releases.** After applying its edits, the host
   writes `MsgIdle` (`$00`) to `$021F`.

5. **Firmware resumes.** PatchWait's inner loop notices `$00`, jumps
   to `PatchDone` which re-enables interrupts and resumes `IdleLoop`.

`_IOPMoveData` with `imPatchIop` is the host-side primitive that
performs steps 1, 3, 4, 5 around a caller-supplied byte-range patch.
This is purely a synchronisation handshake — no state is dropped, no
callbacks are re-registered, no free lists are rebuilt. To rebuild
state, the host has to pulse `iopRun` and re-run the full §9 install
sequence.

---

### 15. SCC IOP (`SccIopNum = 0`)

| Attribute            | Value                                                          |
| -------------------- | -------------------------------------------------------------- |
| Host base address    | `$50F04000`                                                    |
| OSS source           | 7 (`OSSIntIOPSCC`)                                              |
| Front-side device    | Zilog Z8530 SCC (dual-channel UART; modem on A, printer on B)   |
| Bypass mode          | **Supported.** Default at boot.                                  |
| Front-side window    | `$50F04020-$50F0403F` (active in bypass mode)                    |
| Peripheral IRQ wired | Yes — Z8530 `/INT` → `iopBypassIntReq`.                          |

The SCC IOP boots into **bypass mode** so that `IOPMgr` can drive the
Z8530 directly during early initialisation. The host accesses the SCC
chip through the `$50F04020-$50F0403F` passthrough window; the
Z8530's `A/B`-`D/C` encoding decodes from offset bits 0 and 1.

Critical architectural point that the existing literature gets wrong:
**the SCC IOP firmware image as shipped contains no per-channel SCC
driver code.** The resident firmware in `iop-scc.bin` is a minimal
kernel that exposes only the six commands tabulated in §15.1 below.
Per-channel byte-stream I/O, async event delivery, RX/TX interrupt
handling — all of that lives in **downloadable driver code** that the
host writes into the IOP's `$2D55..$56A9` (Driver A) and
`$56AA..$7FEE` (Driver B) regions, then activates with the
`AllocDvr` / `InitDvr` kernel commands. The downloaded-driver message
protocol is documented in §15.3. The resident firmware's
default Z8530 ISR vector table points entirely at a `BRK` stub
(`UnKnown_SCCInt`), so enabling any Z8530 interrupt source without
first downloading a driver will hard-stop the IOP.

For the IIfx specifically, Mac OS in PRAM-default bypass mode never
downloads a driver — every serial byte goes through the
`$50F04020-3F` passthrough window. "Enhanced mode" requires the OS
to load A/UX-era driver code; an emulator that only needs to boot
Mac OS doesn't have to model the AllocDvr/InitDvr path beyond
returning sensible error replies.

The low-memory variable `SCCIOPFlag` (`$0BFE`) tracks whether the OS
currently prefers bypass mode or firmware-driven mode. `IOPMgr`
exposes `SCCIOPBypass` and `SCCIOPHwInit` entry points for the OS to
toggle between the two.

Bit interactions specific to the SCC IOP:

- `iopInBypassMode` (read-only on host side): 1 while the firmware is
  in bypass mode. 0 while the firmware drives the SCC autonomously.
- `iopBypassIntReq`: live Z8530 `/INT`. Visible only in bypass mode;
  outside bypass it always reads 0.
- `iopSCCWrReq`: live Z8530 `WREQ`. Active-low; reads 0 when the SCC
  asserts request. Forced 1 outside bypass.

#### 15.1 SCC IOP slot-1 kernel-command protocol

Slot 1 is the *only* mailbox slot the resident firmware uses, and only
in the host→IOP direction (i.e. `XmtMsg[1]` on the host's side). The
host writes a request byte at `$0220`, optional parameters at `$0221+`,
sets `XmtMsg[1].state = NewMsgSent`, and pulses `iopGenInterrupt`.
The firmware processes, **writes its reply back into the same
`$0220..$023F` buffer**, sets `XmtMsg[1].state = MsgCompleted`, and
raises `iopInt0Active`. There is no separate reply slot. The host
acknowledges with `clrIopInt0`.

The six accepted command codes (from `SCCIOPEqu.aii`):

| Code  | Apple name    | Function                                       |
| ----- | ------------- | ---------------------------------------------- |
| `$01` | `AllocDvr`    | Claim driver A or B on behalf of a client.      |
| `$02` | `DeAllocDvr`  | Release a previously-claimed driver.            |
| `$03` | `InitDvr`     | Run the downloaded driver's init routine.       |
| `$04` | `ByPass`      | Toggle bypass mode on or off (with ownership).   |
| `$05` | `Versn`       | Return a pointer to a Pascal version string.     |
| `$06` | `SCCCntl`     | Set per-channel SCC clock source (bypass only).  |

In **bypass mode** (the boot default and the IIfx steady state), only
`ByPass` (`$04`) and `SCCCntl` (`$06`) are accepted. Every other code
returns an `InByPass` (`$FC`) reply.

All replies share the convention: byte `+$00` is the result code
(`$00 = NoErr`, otherwise a two's-complement negative error code).
Negative results in two's-complement byte form:

| Reply value | Apple name   | Meaning                                          |
| ----------- | ------------ | ------------------------------------------------ |
| `$00`       | `NoErr`      | Success.                                          |
| `$FF`       | `Error`      | Generic / parameter out of range.                  |
| `$FE`       | `UnKnwnMsg`  | Command code outside `$01..$06`.                  |
| `$FD`       | `DvrInUse`   | Driver already allocated by another client.       |
| `$FC`       | `InByPass`   | Operation not legal while in bypass mode.         |
| `$FB`       | `NotAlloc`   | Driver not allocated yet.                         |
| `$FA`       | `BadID`      | Client ID mismatch on bypass-off.                 |

##### 15.1.1 `AllocDvr` (`$01`)

Request:

| Offset | Size | Name        | Notes                                             |
| ------ | ---- | ----------- | ------------------------------------------------- |
| `+$00` | byte | command     | `$01`.                                             |
| `+$01` | byte | `Driver`    | `$00` = driver A (modem channel), `$01` = driver B (printer channel). |
| `+$02` | byte | `ClientID`  | Host-chosen non-zero tag. Used to verify ownership on `DeAllocDvr` and `ByPass`-off. |

Reply (overwrites the same buffer):

| Offset | Size | Name        | Notes                                             |
| ------ | ---- | ----------- | ------------------------------------------------- |
| `+$00` | byte | result      | `NoErr` on success, `DvrInUse` if already held, `Error` if `Driver > 1`. |
| `+$01` | byte | `OwnerID`   | On `DvrInUse`: the `ClientID` of the current owner. Otherwise `$00`. |
| `+$02` | byte | `InitAddrHi`| High byte of the driver's init entry-point — for the resident firmware, `$2D` (driver A) or `$56` (driver B). |
| `+$03` | byte | `InitAddrLo`| Low byte — `$54` or `$A9` respectively (the pre-init RAM addresses are `$2D55-1` / `$56AA-1`, conforming to an RTS-based jump convention). |

##### 15.1.2 `DeAllocDvr` (`$02`)

Request: `+$00 = $02`, `+$01 = Driver`.

Reply: `+$00 = NoErr` or `Error` (driver out of range). Side effect:
performs an indirect JSR through `Close_Vec[Driver]` (preset to
`$2D58` / `$56AD`), giving the downloaded driver a chance to clean
up before the slot is released.

##### 15.1.3 `InitDvr` (`$03`)

Request: `+$00 = $03`, `+$01 = Driver`.

Reply: `+$00 = NoErr`, `NotAlloc`, or `Error`.

This command synchronously runs the downloaded driver's init routine
(at `Init_Vec[Driver]`, returned by `AllocDvr`) and blocks until that
init signals `InitFin` ($02). If the downloaded code never signals
back, no `MsgCompleted` is ever posted for this command — the host's
timeout is the only escape. (The `$031F` alive byte continues to be
refreshed by the kernel scheduler, so an emulator that watches only
the alive byte cannot distinguish "init wedged" from "init running".)

##### 15.1.4 `ByPass` (`$04`)

Request:

| Offset | Size | Name       | Notes                                                   |
| ------ | ---- | ---------- | ------------------------------------------------------- |
| `+$00` | byte | command    | `$04`.                                                  |
| `+$01` | byte | `On_Off`   | `$00` = leave bypass, non-zero = enter bypass.           |
| `+$02` | byte | `ClientID` | Non-zero tag. Must match the entering client when leaving bypass. |

Reply:

| Offset | Size | Name         | Notes                                                              |
| ------ | ---- | ------------ | ------------------------------------------------------------------ |
| `+$00` | byte | result       | `NoErr`, `DvrInUse` (a driver is still allocated), `InByPass`, or `BadID`. |
| `+$01` | byte | `ClientAID`  | On `DvrInUse`: driver A's current owner. On `InByPass`/`BadID`: the bypass holder. Else `$00`. |
| `+$02` | byte | `ClientBID`  | On `DvrInUse`: driver B's current owner. Else `$00`.                |

Enter-bypass side effects: masks the IOP-internal Z8530 IRQ, resets
both Z8530 channels (WR9 = ChanA_Reset | ChanB_Reset), reprograms a
canonical idle config (WR4 = X16 clock + 2 stop bits = `$4C`, WR3 = Rx
8-bit = `$C0`, etc.), and sets `SCC_Cntl ($F030) |= ByPass_Bit`.

Leave-bypass side effects: clears `ByPass_Bit`, unmasks SCC IRQ at
the IOP-internal level, and runs the same Z8530 reinit. The Z8530
is then in a clean state for the downloaded driver to take over —
but **no driver is auto-attached**; the host must `AllocDvr` +
`InitDvr` to wire one up before useful traffic flows.

##### 15.1.5 `Versn` (`$05`)

Request: `+$00 = $05`, `+$01 = DriverID` (`$00 = DvrA`, `$01 = DvrB`,
`$02 = Kern`).

Reply: `+$00 = NoErr` or `Error`; `+$01` = high byte of a 16-bit IOP
RAM pointer to a Pascal-style version string; `+$02` = low byte. The
resident kernel registers exactly one string at boot:

```
"Enhanced Communications Controller"
"Version 1.0A0 © Apple Computer Inc. 1988"
```

(Each segment is a separate Pascal-counted string in IOP RAM; the
pointer returned by Versn points at the start of the kernel's
registered block.)

Note the asymmetry vs. `AllocDvr`: `AllocDvr` returns the address as
(hi, lo) at `+$02 / +$03`, but `Versn` returns (hi, lo) at
`+$01 / +$02`. This asymmetry is in the firmware as-written.

##### 15.1.6 `SCCCntl` (`$06`)

Request: `+$00 = $06`, `+$01 = Driver`, `+$02 = GPI` (0 = use 3.6864 MHz
RTxC pin; non-zero = use GPIA as the RTxC clock source for that channel).

Reply: `+$00 = NoErr` or `Error` (driver out of range, or not in
bypass mode). Side effect: toggles either the `RTXCA_GPIA` (`%00011000`)
or `RTXCB_GPIA` (`%01100000`) bits of `SCC_Cntl` ($F030).

#### 15.2 SCC IOP — interrupts the host actually sees

`iopInt0Active` is **the only** host IRQ the resident firmware ever
raises, and only as the completion signal of a slot-1 kernel
command. `iopInt1Active` is never set by the resident firmware; if
the host sees Int1 from an SCC IOP, downloaded driver code must be
responsible.

Bypass-mode SCC byte-level interrupts (RX-char-available, TX-buffer-
empty, framing error, break received) reach the host directly via
`iopBypassIntReq` as the OSS source 7 interrupt — they do **not**
route through the mailbox or through `iopIntNActive`. The Z8530's
own interrupt vector is the only thing the host's SCC driver
inspects.

#### 15.3 Enhanced mode — the downloadable serial drivers

The resident kernel (§15.1) only allocates, initialises, and bypasses;
the actual byte-stream serial I/O lives in two **downloadable** drivers —
one per port — shipped as `'SERD'` resources (id 60 = modem / port A,
id 61 = printer / port B; the first two bytes are the driver length, the
rest is the IOP image). The host claims a port with `AllocDvr`, downloads
the `'SERD'` body into the region `AllocDvr` reported (driver A from
`$2D55`, driver B from `$56AA`), and runs its init with `InitDvr`. From
then on the driver presents the Inside-Macintosh serial-driver interface
(`.AIn`/`.AOut`/`.BIn`/`.BOut`) and off-loads the SCC from the 68030.

Granny Smith does **not** model this path — the IIfx boots Mac OS in
PRAM-default bypass and never downloads a `'SERD'`. It is documented here
for completeness, and because **A/UX** uses enhanced mode (see PT 18,
below). Source: *Serial IOP Driver ERS*.

Each active driver owns **six** mailbox slots — three host→IOP and three
IOP→host — at fixed slot numbers: **port A = slots 2, 3, 4; port B =
slots 5, 6, 7** (slot 1 stays the resident kernel-command channel of
§15.1). Within a driver the three logical boxes map:

| Driver box | Host→IOP (`XmtMsg`)        | IOP→host (`RcvMsg`)      |
| ---------- | -------------------------- | ------------------------ |
| 1          | Read                       | Event (status-change)    |
| 2          | Write                      | —                        |
| 3          | Open / Control / Close     | —                        |

So for port A: `XmtMsg[2]` = Read, `XmtMsg[3]` = Write, `XmtMsg[4]` =
Open/Control/Close, `RcvMsg[2]` = Event. Port B uses 5/6/7 identically.
Every payload puts the IOP-written `Result code` at `+$00`.

- **Open** (box 3, opcode `$01`): in — serial config word (`+$04`, the
  *Inside Macintosh* vol II format) and the IOP-specific baud divisor
  (`+$06`, host-converted from the config word's divisor); out — the IOP
  addresses + lengths of the **write buffer** (`+$08`/`+$0A`) and the
  **statistics buffer** (`+$0C`). The host fills the write buffer and
  reads status straight out of those IOP-RAM buffers, not via messages.
- **Read** (box 1): completes when ≥1 byte is in the IOP's RX ring buffer
  (or on an abortable error); the reply hands back the ring-buffer
  address/extent and byte count, and the *next* Read tells the IOP how
  many bytes the host consumed. Data is moved by the host reading the IOP
  ring buffer directly.
- **Write** (box 2): host copies data into the Open-reported write buffer,
  then sends a Write with the length (`+$04`); it completes when the last
  byte clears the SCC transmitter.
- **Control** (box 3, opcode `$00` + a sub-opcode): the full Macintosh
  serial control set — Serial Reset (`$08`), Serial Handshake (`$0A`),
  Clear/Set Break (`$0B`/`$0C`), Set Baud (`$0D`), Extended Handshake
  (`$0E`), Control Options (`$10`), Assert/Clear DTR (`$11`/`$12`),
  PE-substitution (`$13`/`$14`), Set/Clear XOff (`$15`/`$16`),
  (Unconditional) Send XOn/XOff (`$17`–`$1A`), Reset SCC (`$1B`).
- **Close** (box 3, opcode `$02`).
- **Event** (IOP→host box 1, raised via Int1): posted on an SCC status
  change the host armed via the Serial Handshake control call; carries
  SCC RR0 at interrupt time (`+$04`) and the changed RR0 bits (`+$05`).

Status (the six-byte Inside-Mac status block) is **not** a message: the
driver maintains it in the statistics buffer whose address Open returned;
the host reads it directly, with a one-byte valid/ack handshake at the
head of that buffer.

**A/UX caveat (PT 18).** Apple developer tech note *PT 18 — IOP-Based
Serial Differences under A/UX* documents serial behaviour differences and
bugs when A/UX drives the IOP serial path; an A/UX-accurate serial model
must account for it. Granny Smith does not model IOP serial yet, so this
is informational.

---

### 16. SWIM IOP (`SwimIopNum = 1`)

| Attribute            | Value                                                          |
| -------------------- | -------------------------------------------------------------- |
| Host base address    | `$50F12000`                                                    |
| OSS source           | 6 (`OSSIntIOPSWIM`)                                            |
| Front-side device    | SWIM1 floppy controller                                          |
| Secondary device     | ADB single-wire bus (bit-banged by firmware GPIO)                |
| Bypass mode          | **Not supported.** Firmware-driven only.                        |
| Front-side window    | Not exposed to the host. SWIM is reached only via mailbox.       |
| Peripheral IRQ wired | No.                                                              |

The SWIM IOP runs in firmware-driven mode at all times — there is no
host-direct path to the SWIM1 chip. Floppy I/O and ADB transactions
are entirely mailbox-driven.

The firmware fronts three distinct functions through the mailbox,
each on its own dedicated slot:

| Slot | Channel             | Direction                                | Protocol               |
| ---- | ------------------- | ---------------------------------------- | ---------------------- |
| 1    | BlockCopy / patch   | `RcvMsg` only (IOP → host)               | `MoveReqInfo` (§16.1)  |
| 2    | `.Sony` (SonyIOP)   | XmtMsg (host → IOP), RcvMsg (IOP → host) | `SwimIopMsg` (§17)     |
| 3    | ADB Mgr (ADBMsg)    | XmtMsg (host → IOP), RcvMsg (IOP → host) | `ADBMsg` (§18)         |

Slot 1 of `XmtMsg` (host → IOP) is `Unimplemented` — the firmware
will BRK-halt if the host kicks it. Slots 4-7 of both directions are
also `Unimplemented`. So the SWIM IOP advertises 3 active slots
(`XmtMsg[0]` and `RcvMsg[0]` seeded to `$03` — see §6).

Bit interactions specific to the SWIM IOP:

- `iopInBypassMode`: always reads 0 (firmware never enables bypass).
- `iopBypassIntReq`: not wired; reads 0.
- `iopSCCWrReq`: not wired; reads 1.

#### 16.1 Slot 1 — BlockCopy (`MoveReqInfo`)

This slot is the *only* way bulk floppy data crosses the host/IOP
boundary. The IOP has 32 KB of shared RAM but a single floppy I/O
request can move many KB to or from main RAM; rather than streaming
the data through `iopRamData`, the firmware buffers it in IOP RAM
and asks the host to do the bus-master move. The host's response
is a single `_IOPMoveData` invocation, so the per-byte cost is
amortised across a whole sector batch.

The protocol is the standard "RcvMsg" flow from §11 with this
specific payload:

| Offset  | Size  | Name             | Notes                                                                                      |
| ------- | ----- | ---------------- | ------------------------------------------------------------------------------------------ |
| `+$00`  | byte  | `bcReqCmd`       | `$00 = bcIOPtoHOST`, `$01 = bcHOSTtoIOP`, `$02 = bcCompare`.                                |
| `+$01`  | byte  | (padding)        | Unused.                                                                                    |
| `+$02`  | word  | `bcReqByteCount` | Big-endian; total transfer in bytes (typically a power-of-two multiple of 256).             |
| `+$04`  | long  | `bcReqHostAddr`  | Big-endian 32-bit physical host RAM address (source for `bcHOSTtoIOP`, destination for `bcIOPtoHOST`). |
| `+$08`  | word  | `bcReqIopAddr`   | Big-endian 16-bit IOP shared-RAM address.                                                  |
| `+$0A`  | byte  | `bcReqCompRel`   | Output field, used only when `bcReqCmd = bcCompare`: host writes a non-zero "unequal" or zero "equal" result before completing the reply. |

State-machine: firmware fills the payload, sets
`RcvMsg[1].state = NewMsgSent`, raises Int1. Host's `IOPInterrupt`
walks RcvMsg states, finds slot 1, performs the requested copy
(or compare), writes the `bcReqCompRel` byte back for `bcCompare`,
sets `RcvMsg[1].state = MsgCompleted`, kicks the IOP via
`iopGenInterrupt`. The firmware reads back `bcReqCompRel` (if
relevant), clears `RcvMsg[1].state` to `MsgIdle`, and resumes.

The SWIM driver uses double-buffered BCPBs — two for data, two for
tags (4 total at boot) — so while the host is moving track N-1 out
of IOP RAM, the firmware can already be reading track N into a
different IOP-RAM region. This is why `ReadReq` / `WriteReq` can
sustain near-floppy-bandwidth without stalling on host-side ack
latency.

`ReadVerifyReq` reuses this slot with `bcCompare`: the firmware reads
from disk into IOP RAM, then issues a BlockCopy with
`bcReqCmd = bcCompare`. The host reads the host-side buffer, compares
against IOP RAM, and reports equal/unequal via `bcReqCompRel`. A
non-zero compare result becomes `dataVerErr` (-68) in the host's
Sony-slot reply.

---

### 17. SWIM IOP — slot 2 SonyIOP protocol

Slot 2 carries the `.Sony` driver's IOP-mediated floppy protocol. The
on-the-wire layout of the payload is the `SwimIopMsg record` from the
System ROM. Both directions share the layout — XmtMsg payloads carry a
`ReqKind` request code, RcvMsg payloads carry an `EvtKind` event code in
the same byte position.

#### 17.1 Payload layout (`SwimIopMsg`)

Common header (always present at the start of both XmtMsg and RcvMsg
payloads):

| Offset | Size | Name          | Notes                                                     |
| ------ | ---- | ------------- | --------------------------------------------------------- |
| `+$00` | byte | `ReqKind`     | Request code (host → IOP) or event code (IOP → host).      |
| `+$01` | byte | `DriveNumber` | Logical drive index; see §17.4.                            |
| `+$02` | word | `ErrorCode`   | Signed Mac OS result code in replies (noErr / -50 / etc.). |

The bytes from `+$04` onward overlay one of several per-command payload
shapes (an `AdditionalParam` union in the original source):

**Initialize reply** (`xmtReqInitialize`):

| Offset       | Size     | Name                | Notes                                                          |
| ------------ | -------- | ------------------- | -------------------------------------------------------------- |
| `+$04`       | 28 bytes | `DriveKinds[0..27]` | One `DriveKind` per logical drive index. The firmware always writes the full 28-byte block; the IIfx ROM only consumes the first four (qDrive table is 4 wide). For physical drive indexing on the IIfx, see §17.4. |

**DriveStatus / ExtDriveStatus reply** (`xmtReqDriveStatus`). Offsets
inside the payload map one-for-one to the `Stat*` equates in
`SWIMDefs.aii` — those equates are payload-relative, so `StatTrackH =
$04` means `+$04` from the start of the payload at `$0340`:

| Offset      | Size    | Name              | Notes                                                                                     |
| ----------- | ------- | ----------------- | ----------------------------------------------------------------------------------------- |
| `+$04`      | word    | `Track`           | Current head track.                                                                       |
| `+$06`      | byte    | `WriteProtected`  | bit 7 set → write-protected.                                                              |
| `+$07`      | byte    | `DiskInPlace`     | 0 = empty, 1/2 = disk present.                                                            |
| `+$08`      | byte    | `DriveInstalled`  | 0 = unknown, 1 = installed, `$FF` = no drive.                                              |
| `+$09`      | byte    | `Sides`           | bit 7 set → double-sided.                                                                 |
| `+$0A`      | byte    | `TwoSidedFormat`  | `$FF` = 2-sided disk, `$00` = 1-sided.                                                    |
| `+$0B`      | byte    | `NewInterface`    | `$FF` = 800K+ / SuperDrive.                                                               |
| `+$0C`      | word    | `DiskErrors`      | Running disk-error count.                                                                  |
| `+$0E`      | byte    | `DriveInfoB3`     | Drive-info control call byte 3 (high). `$00` for stock SuperDrives.                       |
| `+$0F`      | byte    | `DriveInfoB2`     | Drive-info byte 2. `$00` for stock SuperDrives.                                            |
| `+$10`      | byte    | `DriveAttr`       | Disk-drive attribute byte (filled from the firmware's per-drive `DriveAttributes` table). |
| `+$11`      | byte    | `DriveType`       | Disk-drive-kind byte — same encoding as `DriveKind` (`$04 = DSMFMHD`, etc.).                |
| `+$12`      | byte    | `MfmDrive`        | `$FF` = SuperDrive, else `$00`.                                                            |
| `+$13`      | byte    | `MfmDisk`         | `$FF` = MFM media.                                                                         |
| `+$14`      | byte    | `MfmFormat`       | `$FF` = 1440K, `$00` = 720K.                                                               |
| `+$15`      | byte    | `DiskController`  | `$FF` = SWIM, `$00` = IWM.                                                                 |
| `+$16`      | word    | `CurrentFormat`   | Big-endian bit mask of the current format.                                                |
| `+$18`      | word    | `FormatsAllowed`  | Big-endian bit mask of allowable formats.                                                 |
| `+$1A`      | long    | `DiskSize`        | Big-endian, fixed-media drive size in bytes (HD-20 only; `$00000000` for floppies).        |
| `+$1E`      | byte    | `IconFlags`       | bit 0 = caller wants media icon, bit 1 = drive icon.                                       |
| `+$1F`      | byte    | `Spare`           | Always `$00`.                                                                              |

**Data-transfer request** (`xmtReqRead` / `xmtReqWrite` /
`xmtReqReadVerify`):

| Offset | Size     | Name          | Notes                                       |
| ------ | -------- | ------------- | ------------------------------------------- |
| `+$04` | long     | `BufferAddr`  | Host RAM target/source physical address.    |
| `+$08` | long     | `BlockNumber` | Starting 512-byte block.                    |
| `+$0C` | long     | `BlockCount`  | Number of 512-byte blocks to transfer.      |
| `+$10` | 12 bytes | `MfsTagData`  | MFS tag bytes (read/write).                  |

`BufferAddr` is a host main-RAM physical address — the IOP never reads
or writes that address directly. Instead it stages the data in its
own shared RAM and uses the slot-1 BlockCopy protocol (§16.1) to ask
the host to perform the actual move. Double-buffering inside the
firmware overlaps disk I/O with host-side BlockCopy so that a
multi-track read sustains close to floppy bandwidth.

**Format request** (`xmtReqFormat`) uses a different `+$04` overlay:

| Offset | Size | Name              | Notes                                                  |
| ------ | ---- | ----------------- | ------------------------------------------------------ |
| `+$04` | word | `FormatKind`      | Disk-format selector (one of the `*format` constants). |
| `+$06` | byte | `HdrFmtKind`      | Per-sector header format byte (`$00` = firmware default). |
| `+$07` | byte | `FmtInterleave`   | Sector interleave (`$00` = firmware default).           |
| `+$08` | long | `FmtDataAddress`  | Host RAM address of format sector data.                |
| `+$0C` | long | `FmtTagAddress`   | Host RAM address of format tag data.                    |

**GetRawData request** (`xmtReqGetRawData`) uses yet another overlay:

| Offset | Size | Name               | Notes                                                  |
| ------ | ---- | ------------------ | ------------------------------------------------------ |
| `+$04` | long | `RawClockAddress`  | Host RAM destination for packed MFM clock bits.        |
| `+$08` | long | `RawDataAddress`   | Host RAM destination for the raw data bytes.            |
| `+$0C` | long | `RawByteCount`     | Number of raw bytes to read.                            |
| `+$10` | word | `RawSearchMode`    | `$00` immediate, `$01` after address mark, `$02` after data mark, `$03` after index. |
| `+$12` | word | `RawCylinder`      | Cylinder number.                                       |
| `+$14` | byte | `RawHead`          | Head number.                                           |
| `+$15` | byte | `RawSector`        | Sector number.                                         |

#### 17.2 xmtReq request codes (host → IOP, in `ReqKind`)

| Code  | Apple name              | Meaning                                                       |
| ----- | ----------------------- | ------------------------------------------------------------- |
| `$01` | `xmtReqInitialize`      | Returns `DriveKinds[0..27]` (28 bytes) in the reply.            |
| `$02` | `xmtReqShutdown`        | **Stub** — dispatches to `UnKnownSWIMReq`, returns `paramErr`. |
| `$03` | `xmtReqStartPolling`    | Begin autonomous drive-presence polling.                       |
| `$04` | `xmtReqStopPolling`     | Pause drive polling.                                           |
| `$05` | `xmtReqSetHfsTagAddr`   | Stash a host RAM address for MFS tag bytes.                    |
| `$06` | `xmtReqDriveStatus`     | Returns the DriveStatus block above in the reply.              |
| `$07` | `xmtReqEject`           | Eject media from the named drive.                              |
| `$08` | `xmtReqFormat`          | Format media (destructive).                                    |
| `$09` | `xmtReqFormatVerify`    | Verify formatting.                                             |
| `$0A` | `xmtReqWrite`           | Write `BlockCount` × 512-byte blocks from `BufferAddr`.        |
| `$0B` | `xmtReqRead`            | Read `BlockCount` × 512-byte blocks into `BufferAddr`.         |
| `$0C` | `xmtReqReadVerify`      | Read + compare via BlockCopy slot 1 / `bcCompare`. No host buffer touched directly. |
| `$0D` | `xmtReqCacheControl`    | Track-cache enable / disable.                                  |
| `$0E` | `xmtReqTagBufControl`   | Tag-buffer enable / disable.                                   |
| `$0F` | `xmtReqGetIcon`         | **Stub** — dispatches to `UnKnownSWIMReq`, returns `paramErr`.  |
| `$10` | `xmtReqDupInfo`         | Disk-duplicator info.                                          |
| `$11` | `xmtReqGetRawData`      | Copy-protection raw read. Uses a different `+$04..+$15` overlay (clock-bits / data buffer pointers, search mode, cylinder, head, sector). |

Codes `$12..$18` are slots in the dispatch table that all route to
`UnKnownSWIMReq` (returning `paramErr`); they exist so a future
firmware patch can drop new commands in without rebuilding the
dispatch table.

The IOP services every `ReqKind` it understands by writing the result
into the same XmtMsg payload, setting `ErrorCode`, advancing
`XmtMsg[2].state` to `MsgCompleted`, and raising `iopInt0Active`.

#### 17.3 rcvReq event codes (IOP → host, in `ReqKind`)

Posted asynchronously through `RcvMsg[2]` with `iopInt1Active`. The
host must subscribe to slot 2 with `irWaitRcvMessage` at boot.

| Code  | Apple name                 | Meaning                                              |
| ----- | -------------------------- | ---------------------------------------------------- |
| `$01` | `rcvReqDiskInserted`       | Disk just appeared in `DriveNumber`.                  |
| `$02` | `rcvReqDiskEjected`        | Disk just removed from `DriveNumber`.                 |
| `$03` | `rcvReqDiskStatusChanged`  | Drive status changed (e.g. write-protect tab moved).  |

These are emitted by an **autonomous** drive-presence loop the firmware
runs once the host sends `xmtReqStartPolling` (§17.2) — the SonyIOP
analogue of the ADB autopoll loop (§18.3). The firmware scans the drives
on its own and posts an event on `RcvMsg[2]` + Int1 only when one
changes; the host need not poll. Granny Smith models this with a
recurring scheduler event (`swim_drive_poll_tick`, ≈20 ms), gated so it
re-arms only after the host has drained the previous `RcvMsg[2]` event,
and quiesced by `xmtReqStopPolling`.

#### 17.4 DriveKinds and the qDrive numbering convention

The `DriveKinds[]` byte table returned by `xmtReqInitialize` is
**the** authoritative source of "which drive index is what." The
firmware writes all 28 bytes; the Sony driver on the IIfx only
consumes the first 4 (it uses each index `N` as the `qDrive` argument
when calling `_AddDrive`, which in turn becomes `ioVRefNum` for
`_MountVol`).

The firmware populates indices according to *physical* drive numbering
in `SWIMDefs.aii`:

- index 1 = `intDriveNumber` (internal floppy)
- index 2 = `extDriveNumber` (external floppy)
- index 5..8 = `FirstHD20DriveNumber..` (up to 4 HD-20 drives)
- all other indices = `noDriveKind` (`$00`)

DriveKind values:

| Value | Meaning                                       |
| ----- | --------------------------------------------- |
| `$00` | `noDriveKind` — no drive present at this index. |
| `$01` | unspecified.                                  |
| `$02` | `SSGCRDriveKind` — 400 KB GCR single-sided.    |
| `$03` | `DSGCRDriveKind` — 800 KB GCR double-sided.    |
| `$04` | `DSMFMHDDriveKind` — 1.44 MB SuperDrive.       |
| `$07` | `HD20DriveKind` — HD-20 fixed disk.            |

Because Mac OS rejects `ioVRefNum = 0` in `_MountVol` (returns
`paramErr` / -50), **`DriveKinds[0]` must be `$00` on the IIfx** —
logical drive 0 is reserved, and the firmware advertises the two
physical SuperDrives at indices 1 and 2. Granny Smith's SWIM IOP model
issues `DriveKinds = {$00, $04, $04, $00}` for that reason; advertising
the first physical drive at index 0 sends the boot block into a
`_MountVol`-loops-forever stall.

---

### 18. SWIM IOP — slot 3 ADBMsg protocol

Slot 3 carries the OS's `ADB Mgr ↔ IOP firmware` packet protocol. The
payload layout is `ADBMsg`:

| Offset | Size    | Name        | Notes                                              |
| ------ | ------- | ----------- | -------------------------------------------------- |
| `+$00` | byte    | `Flags`     | Bitfield; see §18.1.                                |
| `+$01` | byte    | `DataCount` | Length of valid `ADBData[]` bytes. Legal values are `{0, 2..8}` — 1 is never valid. |
| `+$02` | byte    | `ADBCmd`    | The ADB command byte (Talk / Listen / Reset / Flush). |
| `+$03` | 8 bytes | `ADBData`   | Up to 8 bytes of in/out payload.                    |

(The firmware allocates an 11th byte at `+$0B` as receive-overflow
margin, but this byte is firmware-internal and is not part of the
host-visible payload.)

#### 18.1 `Flags` bit semantics

| Bit | Name              | Meaning                                                                                              |
| --- | ----------------- | ---------------------------------------------------------------------------------------------------- |
| 7   | `ExplicitCmd`     | Host-initiated command. Firmware must issue `ADBCmd` verbatim on the bus. Firmware-internal label: same. |
| 6   | `PollEnable`      | Autopoll mode. Firmware picks the next enabled device and Talk-R0s it; `ADBCmd` in request is ignored. Firmware-internal label: `AutoPollEnable`. |
| 5   | `SetPollEnables`  | One-shot. `ADBData[0..1]` carries a fresh 2-byte big-endian DevMap to replace the firmware's poll mask; bit `N` of the 16-bit value (LSB at `ADBData[1]`, MSB at `ADBData[0]`) = "address `N` is enabled for autopoll". The firmware clears `SetPollEnables` after consuming it. |
| 2   | `SRQ`             | Service-request pending — *some* device asserted SRQ during the gap after the last command's stop bit. **No device address is encoded anywhere.** Firmware-internal label: `ServiceRequest`. |
| 1   | `NoReply`         | Set on the reply when the issued command did not produce a complete valid response. Covers four cases: (a) no device pulled the bus low within ≈260 µs of the command's stop bit, (b) reply started but stalled mid-byte, (c) reply was shorter than 2 bytes or longer than 8 bytes, (d) bus fight detected on either the host's command or its Listen-data phase. Firmware-internal label: `TimedOut`. |

Bits 0, 3, 4 are unused — the firmware never reads or writes them.

`ExplicitCmd` and `PollEnable` are mutually exclusive on a single
request; the OS uses them to disambiguate routing in its `IOPReqDone`
dispatch (one branch goes to `ExplicitRequestDone`, the other to
`ImplicitRequestDone`).

**Default state at cold boot:** `Flags = 0` in the firmware-internal
storage (BSS); `DisableFlags[]` is also BSS-zero, which the firmware
interprets as "all 16 addresses enabled" — i.e. **the default DevMap
is `$FFFF`**. Autopoll itself is **off** at boot — the firmware
performs no bus traffic until the host posts a request with
`PollEnable` (or `ExplicitCmd`) set. The host's first slot-3 message
can be either an Explicit command or an implicit `PollEnable` —
there is no required ordering or init handshake.

**Bus-Reset semantics.** The ADB spec encodes Bus Reset as a long
low-level on the bus, not as a clocked-out command byte. The
firmware triggers Bus Reset whenever the *low nibble* of `ADBCmd`
is `$0` — masked with `$0F` and compared to `ResetCmd = $00`. So
not just `$00` but also `$10`, `$20`, ... etc. produce a Bus Reset.
This matches the ADB spec but is worth noting because it differs
from how most Mac OS host code talks about "Reset".

#### 18.2 The two transport paths

**A. Explicit command** (`ExplicitCmd = 1`). Host fills `ADBCmd`
with a specific command byte and `ADBData` with up to 8 outgoing bytes
(for Listen). The firmware issues that exact command on the bus. The
reply preserves `ExplicitCmd` and echoes the same `ADBCmd`, with
`ADBData` carrying the Talk reply (or empty + `NoReply` if no device
answered). The host's `ADB Mgr` routes the completion through
`ExplicitRequestDone` and advances its command queue.

**B. Implicit autopoll** (`PollEnable = 1`, `ExplicitCmd = 0`).
Setting `PollEnable` puts the firmware into **autopoll mode**. The
`ADBCmd` field of the request is **meaningless** — whatever stale value
the buffer happened to hold — because the firmware picks the device
itself. This is a *mode*, not a one-shot request: once enabled the
firmware polls the bus autonomously (§18.3), and the device-selection
rules below are what it runs on *each* autonomous cycle:

1. Selects the device to poll **using its MRU (most-recently-used)
   chain, not numeric address order**. The chain is maintained
   per-device in IOP RAM (initialised to the identity 0→1→...→15→0 at
   boot, then permuted as devices reply). The next poll target is
   normally `AutoPollAddr` — the device that replied last cycle —
   which means in steady state the firmware **re-polls the same
   device** until either someone else replies (stealing the head of
   the chain) or SRQ kicks in.
2. When the previous reply asserted SRQ (bit 2), the firmware enters
   an SRQ-search phase: it walks `MRUList[AutoPollAddr]`, then
   `MRUList[MRUList[AutoPollAddr]]`, etc., **skipping addresses whose
   `DevMap` bit is clear**. After one full pass (16 hops) it stops
   skipping and polls whatever it lands on. The first non-disabled
   address found becomes the new `AutoPollAddr` and is Talk-R0'd.
3. Issues `Talk-Reg-0` (`(addr << 4) | $0C`) to that address.
4. Returns the response in `ADBData` and sets the reply's `ADBCmd`
   to the actual Talk command that was issued, **not** to the stale
   value from the request.

Step 4 matters: the host's `ADB Mgr` uses the reply's `ADBCmd` to
update its `pollCmd` / `pollAddr` globals, which in turn feed mouse /
keyboard event dispatch. A reply with the wrong `ADBCmd` either
misdelivers data (e.g. mouse bytes look like keyboard events) or
sends the OS into a stuck-state loop (e.g. retrying `Flush` on
address 2 forever).

When the OS sets `SetPollEnables` together with `PollEnable`, the
firmware also reads `ADBData[0..1]` as a big-endian 2-byte DevMap and
latches it before the autopoll scan. The reply clears
`SetPollEnables` (one-shot bit).

#### 18.3 Autonomous Auto/SRQ polling

This is the single most important ADB behaviour to model correctly, and
the one most easily mis-read from the host-side software flow alone.

Per the *IOP ADB Driver ERS* (§"How the Messages are Used"): **once the
host has enabled polling, the IOP polls the bus entirely on its own,
"without intervention from the 68030", and interrupts the host only when
a device actually has data — "one interrupt each time a device has data,
and requires no interrupts to direct the service request polling".** The
firmware free-runs the §18.2.B selection loop at roughly the documented
ADB autopoll rate (≈10 ms per cycle; it calibrates this with a software
fractional-time divider, the 65C02 having no hardware timer divider).
When a polled device returns data, the firmware posts it on `RcvMsg[3]`
with `Flags` = `PollEnable` set / `ExplicitCmd` clear / `NoReply` clear,
`ADBCmd` = the `Talk-R0` it issued, `DataCount` + `ADBData` = the
device's bytes, and raises **Int1**. A poll that finds no data produces
*no* message and *no* interrupt — it is silent.

There are therefore two host usage patterns, and a faithful model must
serve **both**:

- **Enable-once-then-wait (A/UX).** A/UX's ADB driver enables autopoll a
  single time — its last slot-3 message at the end of boot is a *pure*
  `PollEnable` reply (`Flags = $40`, no `ExplicitCmd`, no
  `SetPollEnables`) — and then goes completely silent, relying on the
  firmware to push input via Int1. A model that only acts when kicked by
  the host delivers no input at all under A/UX (this was a real Granny
  Smith bug: mouse and keyboard were dead under A/UX until the autonomous
  loop was added).
- **Re-post each cycle (Mac OS).** Mac OS's ADB Mgr re-issues an implicit
  autopoll request every cycle (its `ImplicitRequestDone` path resumes
  autopoll on each completion), so it keeps host→IOP traffic flowing.

How the host acknowledges an autopoll push is the second subtlety. A/UX's
receive handler is **bypass-style**: it reads `RcvMsg[3]` in its Int1 ISR
and acknowledges purely by **clearing Int1** (write-1-to-clear on
`iopStatCtl`) — it does *not* walk `RcvMsg[3].state` back through
`MsgReceived` / `MsgCompleted`. So the "host has consumed the last push,
the buffer is free again" signal is the **Int1 line going clear**, not the
message-state byte (which stays at `NewMsgSent` indefinitely under A/UX).

**Emulator model (`iop_swim.c`).** Granny Smith models this with a
free-running scheduler event (`swim_adb_autopoll_tick`, ≈11 ms) that is
armed when a slot-3 message arrives with `PollEnable` set and disarmed
when one arrives with it clear. Each tick — while no explicit transaction
is in flight and **no Int1 is still pending** (the bypass-style re-arm
gate above) — it `Talk-R0`s the enabled `DevMap` devices in round-robin
order through the ADB device model; on data it writes `RcvMsg[3]`
(`Flags = $40`, `ADBCmd = Talk-R0`, the bytes) and raises Int1. To avoid
racing a host that drives its *own* polling (Mac OS), each host-driven
slot-3 transaction sets a short grace window during which the autonomous
loop stands aside; it only takes over once the host has gone quiet (the
A/UX case). One mechanism serves both OSes: Mac OS keeps its host-driven
loop, A/UX gets autonomous delivery.

#### 18.4 Reply Int conventions

The firmware's own Int usage follows the §10/§11 split:

- When the host posts a command on `XmtMsg[3]` (`RCVMsg3` from the
  firmware's POV), the firmware's `HandleRCVMsg3` handler copies the
  payload, advances `XmtMsg[3].state` to `MsgCompleted`, and raises
  **Int0** (`MsgCompletedINT`). This is the standard ack for a
  host-initiated request.
- When the firmware has an ADB reply to deliver — whether for an earlier
  explicit command or for an autonomous autopoll cycle (§18.3) — it
  fills `RcvMsg[3]`, sets `RcvMsg[3].state = NewMsgSent`, and raises
  **Int1** (`NewMsgSentINT`).

Once the OS sets `fDBUseRcvMsg = 1` (after its first `irSendRcvReply` on
slot 3) it stops posting explicit commands via `XmtMsg[3]` and instead
puts the next request into the `RcvMsg[3]` payload buffer while
acknowledging the prior reply; the firmware's `HandleXMTMsg3` (RcvMsg-ack
path) picks it up and processes it identically. So from the host's
interrupt perspective slot 3 is effectively "all Int1" once initialised.

**Caveat for emulator authors:** do **not** assume the host always drives
the §11 receive handshake to completion. The §11 flow (host advances
`RcvMsg[N].state` to `MsgReceived` / `MsgCompleted` and kicks the IOP) is
what Mac OS does; A/UX's ADB receive path instead acks an autopoll push by
clearing Int1 and never touches the state byte (§18.3). An emulator that
gates "may I post the next autopoll message?" on `RcvMsg[3].state`
returning to `MsgIdle` delivers exactly one input event under A/UX and
then deadlocks — gate on the Int1-clear instead.

---

### 19. RPU (RAM Parity Unit) probe at `$50F1E000`

Not part of the IOP register window proper, but tightly coupled to
boot-time IOP setup because POST runs the RPU probe between the SCC
IOP heartbeat and the SWIM IOP heartbeat.

The IIfx supports an **optional** RAM Parity Unit chip decoded at
`$50F1E000-$50F1E01F` (the DecoderInfo's `RPUAddr` field is annotated
"RPU - (optional)" in Apple's source). On unequipped IIfxs — including
every shipping production unit — the BIU30's memory decoder is left
without a target for that range and **every access bus-errors**.

POST phase `$93` ($408430DC in IIfx-ROM) probes the chip with a
sequence of ~32 `BSET` / `BCLR` / `MOVE.L` cycles against the
`$50F1E000` window. Every one of those accesses is expected to
bus-error. The handler at the trap vector advances the probe state
machine on each bus-error and leaves `AddrMapFlags` bit 20
(`RPUExists`) **clear** if all 32 cycles trap. Later, the OS's
`gestaltParityAttr` handler reads `RPUAddr` from DecoderInfo and would
touch the chip — but only after first checking `AddrMapFlags` bit 20.

Consequence:

- If the emulated machine returns data for `$50F1E000..$50F1E01F`
  instead of bus-erroring, POST decides the RPU **is** installed,
  sets `RPUExists`, and later `gestaltParityAttr` finds the chip
  unresponsive. System 7's `ParityINIT` then posts the modal
  "Parity has been disabled" dialog and the boot stalls before the
  Finder draws.

Granny Smith's IIfx model bus-errors every byte / word / long
read and write in `$50F1E000..$50F1E01F` to model the absent-RPU
case. This is the only way to reach Finder without an out-of-band
software patch to `AddrMapFlags`.

---

## SCSI DMA

The IIfx pairs a stock NCR 5380 SCSI core (the same custom-marked die
the Mac II / IIx ship with) with an **Apple custom "SCSI DMA" chip**
that wraps it. The 5380 register set is unchanged from the Mac II;
the wrapper layers a programmable bus-master DMA engine on top, with
its own interrupt that feeds OSS source 9.


### 1. Address space

The SCSI subsystem occupies four contiguous windows in the canonical
IIfx I/O island:

| Range                            | Window           | What it is                                           |
| -------------------------------- | ---------------- | ---------------------------------------------------- |
| `$50F0_8000 – $50F0_9FFF`        | DMA wrapper      | Wrapper registers ($80–$1FF) **and** a 5380-register passthrough for the low 128 bytes ($00–$7F). |
| `$50F0_A000 – $50F0_BFFF`        | 5380 polled regs | Direct access to the eight 5380 registers, $10-spaced (see §3). |
| `$50F0_C000 – $50F0_CFFF`        | Pseudo-DMA read  | Every byte read from this window returns the next CDR byte from the chip — used by the CPU-driven pseudo-DMA loops in `FastRead`-class drivers on machines that don't use the wrapper, and as a status-poll address. |
| `$50F0_D000 – $50F0_DFFF`        | Pseudo-DMA write | Mirror of the read aperture for the host→target direction. Writes go to the chip's ODR with the pseudo-DMA flag set, so the byte enters the buffer when the 5380's DRQ is asserted. |

Every byte in the I/O island is mirrored at the same offset within
each subsequent 256 KB block per the IIfx's normal aliasing rules
— the canonical addresses above suffice for any software-visible
behaviour.

### 2. SCSI DMA wrapper register layout

Offsets within the `$50F0_8000` window, byte-addressable, but the
control/state registers are **big-endian 32-bit** values written and
read as longs. Apple's names match `HardwarePrivateEqu.a`:

| Offset (hex) | Apple name | Direction | Notes |
| ------------ | ---------- | --------- | ----- |
| `$000 – $07F` | (5380 passthrough) | R/W | Same byte layout as the polled aperture in §3 — i.e. reads/writes of offset `$N0` hit 5380 register `N`. |
| `$080` | `sDCTRL`   | R/W | Wrapper control + status (see §2.1). |
| `$0C0` | `sDCNT`    | R/W | DMA byte count (24-bit field in a 32-bit register). Decrements as bytes transfer; the driver reads back zero on success. |
| `$100` | `sDADDR`   | R/W | The DMA address. Per the chip ERS (§6.6.4) it holds the *current* address: the engine loads its internal counter from `$100` at DMA start and **auto-increments** as bytes move — it is not a static base (see §4). A **physical** address. |
| `$140` | `sDTIME`   | R/W | Watchdog timer reload value. The wrapper raises `iTIMEP` (and IRQ, if `INTREN` is set) when the timer fires before the transfer completes. |
| `$180` | `sTEST`    | R/W | Chip self-test register; written `0` during SCSI Mgr init to leave test mode. |

#### 2.1 `sDCTRL` bits

Per `HardwarePrivateEqu.a` lines 399–419, names with `i*` prefix
dropped for brevity:

| Bit | Mask     | Name       | Direction | Meaning |
| --- | -------- | ---------- | --------- | ------- |
| 0   | `$0001`  | `DMAEN`    | W         | Enable the wrapper's bus-master DMA engine. Cleared after a transfer completes. |
| 1   | `$0002`  | `INTREN`   | W         | Enable SCSI interrupts to OSS source 9. Stays set across transfers — SCSI Mgr init sets it once via `move.l #iINTREN, sDCTRL(a3)`. |
| 2   | `$0004`  | `TIMEEN`   | W         | Enable the watchdog timer. The driver sets this together with `DMAEN`. |
| 4   | `$0010`  | `RESET`    | W (self-clearing) | Soft-reset the wrapper. After this fires, `sDCTRL` is left with `INTREN` set and `sDCNT` is cleared. |
| 7   | `$0080`  | `TIMEP`    | R         | Timer interrupt pending — set when the watchdog expired before the transfer finished. The driver checks `iDMABERR + iTIMEP` after `@waitForIntr`. |
| 8   | `$0100`  | `DMABERR`  | R         | DMA bus error — set when a 68030 bus error fired during the transfer. Same `andi.w #iDMABERR+iTIMEP, d5` post-flight check. |
| 12  | `$1000`  | `ARBIDEN`  | W         | Enable wrapper-side arbitration. Some Mac OS code paths set this to delegate the arbitration phase to the wrapper rather than driving it by hand through the 5380's MR_ARBITRATE bit. |
| 13  | `$2000`  | `WONARB`   | R         | "Won arbitration" — readback indicator that the wrapper claims to have won. Required acknowledgement before the driver writes the 5380's ICR to start selection. |

The driver's standard "start a DMA read" word is `iTIMEEN +
iINTREN + iDMAEN = $0007`. Writing this kicks the watchdog,
arms the wrapper IRQ, and arms the DMA engine — but no bytes
transfer until `sIDMArx` (§3.1) is touched.

**Latched address vs internal counter.** There is one address register at
`$100`, but two pieces of state behind it: the value the CPU last wrote
(the `$100` latch) and the engine's **internal DMA address counter**, loaded
from the latch when DMA starts (the `MR.DMA` edge / a start-register write)
and then advanced one step per byte/word moved. Re-reading `$100` returns
the advanced counter, not the original base. A/UX depends on this directly
(§4, §5); the read path in Granny Smith returns the internal counter for
`$100` reads.

### 3. NCR 5380 register passthrough

The 5380 register file is exposed at both `$50F0_A000` (polled
aperture) and `$50F0_8000` (the wrapper's low 128 bytes). At both
windows, register selection uses address bits 4–6, so consecutive
registers are 16 bytes apart:

| Offset | Read name | Write name | Apple equ | NCR 5380 datasheet name |
| ------ | --------- | ---------- | --------- | ----------------------- |
| `$00`  | CDR (current data) | ODR (output data) | `sCDR`/`sODR` | Current SCSI Data / Output Data |
| `$10`  | ICR | ICR | `sICR` | Initiator Command Register |
| `$20`  | MR | MR | `sMR` | Mode Register |
| `$30`  | TCR | TCR | `sTCR` | Target Command Register |
| `$40`  | CSR | SER | `sCSR` / `sSER` | Current SCSI Bus Status / Select Enable |
| `$50`  | BSR | `sDMAtx` | `sBSR` / `sDMAtx` | Bus and Status / Start DMA Send |
| `$60`  | IDR | `sTDMArx` | `sIDR` / `sTDMArx` | Input Data / Start Target DMA Receive |
| `$70`  | RESET (reset parity/IRQ) | **`sIDMArx`** | `sRESET` / `sIDMArx` | Reset Parity / **Start Initiator DMA Receive** |

The two read/write names are different functions of the same
4-bit register select; the chip uses /CS, A0–A2, and R/W to
multiplex them. On the Mac bus those map to address bits 4–6
plus the R/W line.

#### 3.1 `sIDMArx` — Start Initiator DMA Receive

A write to register 7 (offset `$70` from either passthrough base)
tells the 5380 to begin DMA-mode handshaking for a data-in
(target → initiator) transfer. On a stock 5380 the chip then
drives `/DRQ` whenever a byte is in the IDR, and the host moves
that byte by reading the pseudo-DMA aperture; the byte count is
the host's responsibility.

On the IIfx the wrapper hooks this differently: when `sIDMArx`
is written **with `DCTRL.DMAEN` set**, the wrapper takes over
the byte-move loop in hardware, copying bytes from the 5380 into
RAM at `sDADDR`, decrementing `sDCNT`, and finally asserting
the wrapper IRQ (gated by `INTREN`) when the count reaches zero
or the watchdog fires. The CPU is free to run while the transfer
happens.

The driver's `@waitForIntr` loop in `FastReadOSS` polls
`OSSIntStat` bit 9 (`OSSIntSCSI`) for the wrapper IRQ. Once it
fires, it checks `sDCTRL` for `iDMABERR | iTIMEP` and `sDCNT`
for zero — anything non-zero signals an error and routes to the
`badAddrError` path.

### 4. Bus-master DMA engine

Beyond the pseudo-DMA aperture (§1) and the per-byte PIO path, the wrapper
adds a real **bus-master DMA engine** — the feature the plain 5380 in the
Mac II / SE/30 lacks. It is the path A/UX uses for all bulk SCSI I/O. (Mac
OS largely sticks to pseudo-DMA / iHSKEN; see §7 and §9.)

**Arming and triggering.** The driver programs `$100` (address) and `$0C0`
(byte count), arms the engine with `sDCTRL = iTIMEEN+iINTREN+iDMAEN`
(`$0007`), puts the 5380 into DMA mode (`MR.DMA`), and then *starts* the
transfer by touching the 5380 start-DMA register for the direction —
`sIDMArx` (`$070`, data-in) or `sDMAtx` (`$050`, data-out). The engine
runs only when that start register is touched **with `DMAEN` set**.

**Physical bus master.** Once running, the engine moves bytes between the
5380 and **main memory directly, on the physical bus** — not through the
68030 or its MMU. A/UX programs `$100` with a **physical** address it
obtained from `realvtop`, so the emulator's engine must use the physical
memory path (`mmu_*_physical`), not the CPU's translated path. (An early
model pumped through the CPU MMU and corrupted memory because the address
is already physical — note 94.)

**Current-address counter (chip ERS §6.6.4).** `$100` holds the *current*
DMA address: the engine loads its internal address counter from `$100` at
DMA start and **auto-increments** it as bytes move, while `$0C0` decrements
toward zero. The internal counter is distinct from the `$100` CPU latch
(§2) — reading `$100` back returns the advanced counter. This
*continuation* property is load-bearing for A/UX, which re-arms the engine
without always rewriting `$100` and relies on the counter to resume where
the previous segment stopped (§5).

**Completion / watchdog / bus error.** When `$0C0` reaches zero the engine
stops, clears `DMAEN`, and (if `INTREN`) raises OSS source 9; the driver's
`@waitForIntr` loop then checks `sDCTRL` for `iDMABERR | iTIMEP` and `$0C0`
for zero. The watchdog (`$140` / `TIMEEN`) reloads on DMA activity and sets
`TIMEP` + interrupts if the transfer stalls (hung target). A faulting
physical access mid-transfer sets `DMABERR` + interrupts instead of
completing.

### 5. Scatter-gather and the read/write re-arm

A/UX transfers that span more than one physical page are **scattered** —
the kernel buffer is a list of physically-discontiguous pages. A/UX does
**not** program a hardware scatter-gather descriptor list; instead it
**re-arms the bus-master engine once per segment** within a single SCSI
command (a fresh `$100` + `$0C0`, toggle `MR.DMA`, re-touch the start
register for each page) while the 5380 stays in the same DATA phase and the
target keeps the command open. Between segments the driver polls the 5380,
waiting for the target still to be REQ-ing, then arms the next page.

Modelling this faithfully took three chip-level rules — no peeking at the
kernel's CDB or page lists (note 114):

- **Read re-arm — keep /REQ asserted while the target has data.** A real
  SCSI target in DATA IN holds `/REQ` asserted as long as it has another
  byte to give, dropping it only when its data is exhausted (then it moves
  to STATUS). The model keeps `CSR_REQ` set while the inbound buffer is
  non-empty and clears it only when the buffer empties. Without this the
  driver's between-segment poll times out after segment 1 and the rest of a
  multi-page read is delivered as stale RAM.
- **Write re-arm — don't force-complete per segment.** Symmetrically, a
  mid-transfer segment EOP on a data-out command must **not** finish the
  command; the engine streams the whole `tl × block_size` and completes
  only when the accumulated buffer is full. With the phase left intact the
  driver re-arms each scattered *source* page itself.
- **Engine supersedes the iHSKEN primer.** A/UX's `scsiout` glue writes a
  `CLR.B` `$00` "primer" to the blind (iHSKEN) port before arming the
  engine. On the engine's **first** data-out byte the model discards
  whatever the primer left in the buffer, so the transfer isn't shifted one
  byte late. This fires once per command, only on the bus-master path —
  pure-iHSKEN writers (Mac OS) are untouched.

Before these rules existed the model carried a CDB-decoding "kernel-credit
shim" to paper over the missing per-segment re-arm; it was the source of
on-disk corruption and has since been **removed** (see §9).

### 6. Interrupt routing

The 5380 has two open-collector outputs, `/IRQ` and `/DRQ`. On the
Mac II / SE/30 / IIcx these wire into VIA2 (CB2 and CA2 respectively).
The IIfx breaks that pattern:

- The 5380's `/IRQ` and the SCSI DMA wrapper's transfer-complete
  signal are OR'd together into a single OSS interrupt source.
- That source is **OSS source 9** (`OSSMskScsi EQU $009` in
  `HardwarePrivateEqu.a`; `OSSIntSCSI EQU 9` is the bit number in
  `OSSIntStat`).
- The default IRQ level the boot ROM programs for source 9 is 2
  (`irq_level[9] = 2` in the post-init OSS state).
- There is **no Mac-II-style VIA2 IFR bit** for SCSI on the IIfx.
  Code that polls VIA2 to detect SCSI interrupts will see nothing.

The SCSI Mgr `FastReadOSS` path waits for the wrapper IRQ by
polling `OSSIntStat`:

```
@waitForIntr
        btst.b  #OSSIntSCSI-8, OSSIntStat(a1)
        beq.s   @waitForIntr
```

`OSSIntStat` is a 16-bit register at OSS offset `$202`; bit 9
lives in the upper byte, hence the `-8` in the bit number.

### 7. Boot vs runtime access patterns

The IIfx ROM and Mac OS access the SCSI subsystem through three
distinct paths, in this order during a cold boot:

1. **Boot ROM partition probe (PIO, 6-byte CDB)**. Before Mac OS
   even loads, the ROM reads block 0 of the disk to find the
   partition map, then iterates LBAs 1..N to enumerate the
   partition records. Each read is a 6-byte `READ(6)` with tl=1.
   These reads use the polled aperture (`$50F0_A000`) and the
   pseudo-DMA read aperture (`$50F0_C000`); the ROM doesn't touch
   the wrapper's DMA registers. The driver partition's first
   sectors are loaded the same way once located.

2. **SCSI Manager init (one-time wrapper handshake)**. Once the
   in-RAM SCSI driver takes over, it runs `SCSIMgrInit.a`'s
   `@DMAdone` branch:

   ```
       btst.b  #sSCSIDMAExists, G_Reserved0+3(a4)
       beq.s   @DMAdone
       movea.l SCSIBase, a3
       move.l  #iINTREN, sDCTRL(a3)   ; INTREN only, clears test mode
   ```

   This single write is what produces the `DCTRL = $0000_0002`
   pattern observable in a SCSI-write trace on the IIfx — Mac OS
   leaves `INTREN` armed for the lifetime of the boot and only
   adds `DMAEN+TIMEEN` for each bulk transfer.

3. **Bulk reads (true DMA)**. Once Mac OS reaches `FastReadOSS`
   (the OSS-aware variant of the SCSI Mgr's Fast Read helper),
   bulk transfers go through the wrapper:

   ```
       move.l  G_Reserved1(a4), sDTIME(a3)   ; watchdog reload
       move.l  #iTIMEEN+iINTREN+iDMAEN, sDCTRL(a3)
       move.l  pAddr(a0), sDADDR(a3)
       move.l  pCount(a0), sDCNT(a3)
       move.b  #iIEOP+iDMA, sMR(a3)          ; arm 5380 DMA mode
       move.b  zeroReg, sIDMArx(a3)          ; start the transfer
   @waitForIntr
       btst.b  #OSSIntSCSI-8, OSSIntStat(a1)
       beq.s   @waitForIntr
       ...
   ```

   Mac OS prefers 10-byte CDB (`READ_10`) here, so the volume
   manager / HFS layer can request transfers up to 65535 blocks
   in a single SCSI command.

### 8. Emulator-side requirements

To support the path above end-to-end, an IIfx emulator has to
expose:

- The polled aperture at `$50F0_A000` with the standard 5380
  semantics (selection, arbitration, command/data/status phases,
  ICR/MR/TCR/CSR/BSR/IDR/ODR).
- The pseudo-DMA read/write apertures at `$50F0_C000` / `$50F0_D000`
  that pull bytes through the 5380's data path one at a time. This
  is enough to run the legacy `FastRead` paths used by ROM-resident
  drivers and by every Mac OS version through 7.6.1 when the wrapper
  is bypassed.
- The DMA-wrapper register file at `$50F0_8080..$50F0_81FF` (DCTRL,
  DCNT, DADDR, DTIME, TEST), including the 5380 passthrough for
  offsets `$00..$7F` so the SCSI Mgr can address the chip through
  the wrapper window too.
- A wrapper-side bus-master DMA engine (§4) triggered by a write to
  `sIDMArx` (`$70`, data-in) or `sDMAtx` (`$50`, data-out) when
  `DCTRL.DMAEN` is set: it moves `sDCNT` bytes between the 5380 and RAM
  at the **physical** address in `$100`, **auto-increments** the internal
  address counter, leaves `sDCNT` zero and `DMAEN` cleared on success, and
  asserts OSS source 9 if `INTREN` is set. It must drive the **physical**
  memory bus (the address is already physical — no CPU MMU), reload the
  `$140` watchdog on activity (→ `TIMEP` on stall), and set `DMABERR` on a
  faulting access.
- The scatter-gather re-arm behaviour of §5 — keep `/REQ` asserted while
  the inbound buffer is non-empty (so a multi-segment read re-arms each
  scattered page), don't force-complete a data-out command on a
  mid-transfer segment EOP (so a multi-segment write re-arms each scattered
  source page), and let the engine's first data-out byte supersede the
  iHSKEN primer. Without these, A/UX delivers only the first page of a
  scattered transfer (read) or writes shifted/garbage data (write).
- Routing of the 5380's `/IRQ` line into OSS source 9 (not VIA2,
  which the IIfx doesn't even have a software-visible second copy
  of).

Apple Technical Note HW 09 famously says shipping Mac OS (6.0.5
through 7.6.1) "does not yet take advantage of" the wrapper's true
DMA mode — that is at best out of date. Mac OS 7's universal SCSI
Manager calls `FastReadOSS` on every machine that reports
`sSCSIDMAExists` (see `SCSIMgrHW.a@1075` for the symmetric
`FastWriteOSS` path), and a working IIfx emulator must service
`sIDMArx` writes accordingly.

A/UX drives the wrapper hardest: it uses the bus-master engine for **both**
directions and for **scattered** multi-page transfers — not via a hardware
descriptor list but by re-arming the engine per page (§5), leaning on the
auto-incrementing internal address counter (§4) to continue across
segments. The write direction (`sDMAtx`) is symmetric to the read
direction (`sIDMArx`) but, as §5 details, is **not** a trivial mirror: the
per-segment re-arm and the iHSKEN-primer interaction were where the real
modelling bugs lived. See §9 for the history.

### 9. Modelling history & pitfalls

The IIfx SCSI DMA path was the single hardest part of the A/UX bring-up;

- **Apple disabled bus-master SCSI DMA under Mac OS on the IIfx** (it was
  "trashing disks"; Tech Note HW 09). Mac OS therefore drives the chip in
  pseudo-DMA / iHSKEN mode, while **A/UX is the only shipping OS that uses
  the bus-master engine** — including for writes. The two OSes exercise
  *different* paths through the same chip, and both must keep working: a
  change made for A/UX (e.g. the iHSKEN-primer rule, §5) must not regress
  the Mac OS pseudo-DMA path.
- **The address is physical; pump the physical bus, not the CPU MMU**
  (note 94). A/UX hands the engine a `realvtop` physical address.
- **The "kernel-credit shim" was a dead end — now removed.** While the
  write path was mis-modelled, the model carried a workaround that decoded
  the SCSI CDB's LBA (which the real 343S0064-A engine cannot see) to
  "credit" overlapping re-issued reads/writes. It papered over the missing
  per-segment re-arm (§5) and *caused* the on-disk corruption it appeared
  to fix. Once the three §5 rules were in place the shim was verified inert
  (zero credits across a full boot) and **removed** (note 114). The general
  lesson: when a load-bearing workaround has to read guest state the
  hardware can't, the real bug is elsewhere — fix that and the workaround
  drops out cleanly.
- **Breakpoints perturb this path.** The SCSI FSM is cycle-timing
  sensitive; a breakpoint or logpoint forces single-step and shifts IRQ
  timing enough to move (or hide) the failure. Use no-breakpoint,
  event-gated diagnostics (`GS_*` env gates) for SCSI-DMA bugs, and a fresh
  disk-image copy per run — a buggy write persists corruption into the
  image.
- **Read and write are mirror-symmetric *bugs* too.** The read "deliver
  only segment 1" defect and the write "shift / wrong-source" defect were
  the same missing per-segment re-arm seen from both directions; fixing one
  without the other still corrupts.

---