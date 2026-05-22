# Macintosh IIfx — I/O Processors (IOPs)

A reference for the abstraction level at which Granny Smith models the two
I/O Processors used in the IIfx. Every register, command byte, shared-RAM
offset, and state encoding here is host-visible: it is what the IIfx
ROM and Mac OS software actually read and write. We do not model the
internal 65C02 microcontroller or its on-die register file — those are
firmware-internal and not exercised through any external interface.

---

## 1. What an IOP is

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

## 2. Host address map

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

## 3. The PIC register window

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

## 4. `iopStatCtl`

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

### 4.1 Per-bit behaviour

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

### 4.2 Composed command bytes

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

## 5. Shared RAM access via `iopRamAddr` / `iopRamData`

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

## 6. Shared-RAM layout (Apple's firmware convention)

Apple uses a fixed shared-RAM layout common to every `'iopc'` firmware
image. The host-visible parts are:

| Range                    | Apple symbol         | Role                                                                                                |
| ------------------------ | -------------------- | --------------------------------------------------------------------------------------------------- |
| `$0000 — $01FF`          | (private)            | 65C02 zero-page and stack; opaque to the host.                                                       |
| `$0200`                  | `IOPXmtMsgBase`      | `XmtMsg[0].state` byte. Seeded by the firmware to `MaxIopMsgNum` (`$07`) as the slot count.          |
| `$0201 — $0207`          | (`XmtMsg[1..7].state`) | Per-slot state for the seven transmit slots (host → IOP messages). Encodings from §7.              |
| `$021F`                  | `PatchReqAddr`       | Soft-restart request byte. See §14.                                                                  |
| `$0220 — $023F`          | `XmtMsg[1].msg`      | 32-byte payload of transmit slot 1.                                                                  |
| `$0240 — $025F`          | `XmtMsg[2].msg`      | 32-byte payload of transmit slot 2.                                                                  |
| …                        | …                    | …                                                                                                   |
| `$02E0 — $02FF`          | `XmtMsg[7].msg`      | 32-byte payload of transmit slot 7.                                                                  |
| `$0300`                  | `IOPRcvMsgBase`      | `RcvMsg[0].state` byte. Same seeding convention as `XmtMsg[0]`.                                       |
| `$0301 — $0307`          | (`RcvMsg[1..7].state`) | Per-slot state for the seven receive slots (IOP → host messages).                                    |
| `$031F`                  | `IOPAliveAddr`       | Firmware-alive heartbeat. Written `$FF` on entry; host polls. See §8.                                |
| `$0320 — $0339`          | `RcvMsg[1].msg`      | 32-byte payload of receive slot 1. (Apple stride is 32; only the first 10 bytes are usually used.)   |
| `$0340 — $035F`          | `RcvMsg[2].msg`      | 32-byte payload of receive slot 2.                                                                   |
| …                        | …                    | …                                                                                                   |
| `$03E0 — $03FF`          | `RcvMsg[7].msg`      | 32-byte payload of receive slot 7.                                                                   |
| `$0400 — $7FFF`          | (firmware)           | 65C02 code, lookup tables, working buffers. Layout depends on `'iopc'` image; not host-visible.       |

```
MaxIopMsgNum    equ 7         ; transmit / receive slots are numbered 1..7
MaxIopMsgLen    equ 32        ; up to 32 bytes per slot
```

So each direction has seven 32-byte mailbox slots, with state bytes
packed in the eight-byte block immediately following the base symbol
(`$0200-$0207` for XmtMsg, `$0300-$0307` for RcvMsg).

### 6.1 Computing slot addresses

For a slot number `N` (1..7) and a base symbol `Base` (`$0200` or
`$0300`):

- State byte:  `Base + N`
- Payload base: `Base | (N << 5)`  (= `Base + N * 32`)

For example, `XmtMsg[3].state = $0203` and `XmtMsg[3].msg = $0260`.

---

## 7. Message states

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

## 8. Alive handshake

The IOP manager polls `IOPAliveAddr` (`$031F`) for `$FF` after releasing the
IOP from reset, with a timeout loop.

The firmware writes `$FF` to `IOPAliveAddr` very early in its reset
entry. After the boot-time handshake completes, the firmware writes
this byte (or a sentinel sub-byte, depending on the image) on every
main-loop iteration so the host can monitor the IOP's health later.

---

## 9. Code load protocol

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

## 10. Mailbox protocol — Transmit (host → IOP)

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
- `iopInt0Active` is the wake-up signal for the host. Always
  acknowledged with `clrIopInt0`.
- The host always restores `iopRamAddr` before kicking the IOP, so
  concurrent host code that's mid-transfer doesn't see surprises.

---

## 11. Mailbox protocol — Receive (IOP → host)

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
   `iopInt1Active`. (The convention is Int0 for "I finished your
   request, here is the reply", Int1 for "spontaneous message, please
   handle it".)

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

## 12. OS Manager API

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

The kernel-message channel (slot 1 of each IOP) is reserved by
`IOPMgr` for its own use and carries a `MoveReqInfo` message
 — a per-IOP "kernel" command channel used during
boot for things like "scan drives" and "install ADB driver".

---

## 13. Interrupt path to the 68030 (OSS)

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

## 14. Soft restart via `PatchReqAddr`

`$021F` in shared RAM is reserved for a "soft restart" request:

1. Host writes a non-zero byte to `$021F` via `iopRamAddr` +
   `iopRamData`.
2. The firmware's main loop polls this byte. On the next iteration,
   non-zero is detected and the firmware jumps to its internal restart
   trampoline near the top of code memory.
3. The trampoline reinitialises internal state (free lists, callback
   vectors), then waits for the host to clear `$021F` back to zero.
4. Host clears `$021F` once it's done patching whatever it intended
   to patch (typical use is via `_IOPMoveData` with `imPatchIop`).
5. The trampoline returns to the main loop.

This avoids a full code reload, which would otherwise require pulsing
`iopRun` and re-running the full `IOPMgr` install sequence (§9). The
caller does this through `_IOPMoveData` with `imPatchIop`, which
handles the handshake.

---

## 15. SCC IOP (`SccIopNum = 0`)

| Attribute            | Value                                                          |
| -------------------- | -------------------------------------------------------------- |
| Host base address    | `$50F04000`                                                    |
| OSS source           | 7 (`OSSIntIOPSCC`)                                              |
| Front-side device    | Zilog Z8530 SCC (dual-channel UART; modem on A, printer on B)   |
| Bypass mode          | **Supported.** Default at boot.                                  |
| Front-side window    | `$50F04020-$50F0403F` (active in bypass mode)                    |
| Peripheral IRQ wired | Yes — Z8530 `/INT` → `iopBypassIntReq`.                          |

The SCC IOP boots into **bypass mode** so that `IOPMgr` can drive the
Z8530 directly during early initialisation, before the firmware-side
driver is up. The host accesses the SCC chip through the
`$50F04020-$50F0403F` passthrough window; the Z8530's `A/B`-`D/C`
encoding decodes from offset bits 0 and 1.

Once `IOPMgr` posts an "enhanced mode" XmtMsg, the firmware takes
ownership: it programs the Z8530, enables its own DMA, and routes
per-channel send/receive events to the host as Int0 mailbox
completions.

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

---

## 16. SWIM IOP (`SwimIopNum = 1`)

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

The firmware fronts two distinct functions through the same mailbox,
each on its own dedicated slot:

| Slot | Channel             | Direction(s)                     | Protocol           |
| ---- | ------------------- | -------------------------------- | ------------------ |
| 1    | IOPMgr kernel       | XmtMsg + RcvMsg                  | `MoveReqInfo` (§12)|
| 2    | `.Sony` (SonyIOP)   | XmtMsg (host → IOP), RcvMsg (IOP → host) | `SwimIopMsg` (§17) |
| 3    | ADB Mgr (ADBMsg)    | XmtMsg (host → IOP), RcvMsg (IOP → host) | `ADBMsg` (§18) |

Bit interactions specific to the SWIM IOP:

- `iopInBypassMode`: always reads 0 (firmware never enables bypass).
- `iopBypassIntReq`: not wired; reads 0.
- `iopSCCWrReq`: not wired; reads 1.

---

## 17. SWIM IOP — slot 2 SonyIOP protocol

Slot 2 carries the `.Sony` driver's IOP-mediated floppy protocol. The
on-the-wire layout of the payload is the `SwimIopMsg record` from the
System ROM. Both directions share the layout — XmtMsg payloads carry a
`ReqKind` request code, RcvMsg payloads carry an `EvtKind` event code in
the same byte position.

### 17.1 Payload layout (`SwimIopMsg`)

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

| Offset | Size    | Name          | Notes                                          |
| ------ | ------- | ------------- | ---------------------------------------------- |
| `+$04` | 4 bytes | `DriveKinds[0..3]` | One `DriveKind` per logical drive index. |

**DriveStatus / ExtDriveStatus reply** (`xmtReqDriveStatus`):

| Offset | Size | Name              | Notes                                              |
| ------ | ---- | ----------------- | -------------------------------------------------- |
| `+$04` | word | `Track`           | Current head track.                                |
| `+$06` | byte | `WriteProtected`  | bit 7 set → write-protected.                       |
| `+$07` | byte | `DiskInPlace`     | 0 = empty, 1/2 = disk present.                     |
| `+$08` | byte | `DriveInstalled`  | 0 = unknown, 1 = installed, `$FF` = no drive.       |
| `+$09` | byte | `Sides`           | bit 7 set → double-sided.                          |
| `+$0A` | byte | `TwoSidedFormat`  | `$FF` = 2-sided disk, `$00` = 1-sided.              |
| `+$0B` | byte | `NewInterface`    | `$FF` = 800K+ / SuperDrive.                         |
| `+$0C` | word | `DiskErrors`      | Running disk-error count.                          |
| `+$0E` | byte | `MfmDrive`        | `$FF` = SuperDrive.                                 |
| `+$0F` | byte | `MfmDisk`         | `$FF` = MFM disk in drive.                          |
| `+$10` | byte | `MfmFormat`       | `$FF` = 1440K, `$00` = 720K.                        |
| `+$11` | byte | `DiskController`  | `$FF` = SWIM, `$00` = IWM.                          |
| `+$12` | word | `CurrentFormat`   | Current-format bit mask.                           |
| `+$14` | word | `FormatsAllowed`  | Allowable-format bit mask.                         |

**Data-transfer request** (`xmtReqRead` / `xmtReqWrite` /
`xmtReqReadVerify` / `xmtReqFormat`):

| Offset | Size     | Name          | Notes                                       |
| ------ | -------- | ------------- | ------------------------------------------- |
| `+$04` | long     | `BufferAddr`  | Host RAM target/source physical address.    |
| `+$08` | long     | `BlockNumber` | Starting 512-byte block.                    |
| `+$0C` | long     | `BlockCount`  | Number of 512-byte blocks to transfer.      |
| `+$10` | 12 bytes | `MfsTagData`  | MFS tag bytes (read/write).                  |

### 17.2 xmtReq request codes (host → IOP, in `ReqKind`)

| Code  | Apple name              | Meaning                                                       |
| ----- | ----------------------- | ------------------------------------------------------------- |
| `$01` | `xmtReqInitialize`      | Returns `DriveKinds[0..3]` in the reply.                       |
| `$02` | `xmtReqShutdown`        | Stop driver activity.                                          |
| `$03` | `xmtReqStartPolling`    | Begin autonomous drive-presence polling.                       |
| `$04` | `xmtReqStopPolling`     | Pause drive polling.                                           |
| `$05` | `xmtReqSetHfsTagAddr`   | Stash a host RAM address for MFS tag bytes.                    |
| `$06` | `xmtReqDriveStatus`     | Returns the DriveStatus block above in the reply.              |
| `$07` | `xmtReqEject`           | Eject media from the named drive.                              |
| `$08` | `xmtReqFormat`          | Format media (destructive).                                    |
| `$09` | `xmtReqFormatVerify`    | Verify formatting.                                             |
| `$0A` | `xmtReqWrite`           | Write `BlockCount` × 512-byte blocks from `BufferAddr`.        |
| `$0B` | `xmtReqRead`            | Read `BlockCount` × 512-byte blocks into `BufferAddr`.         |
| `$0C` | `xmtReqReadVerify`      | Read + compare; no host buffer touched.                        |
| `$0D` | `xmtReqCacheControl`    | Track-cache enable / disable.                                  |
| `$0E` | `xmtReqTagBufControl`   | Tag-buffer enable / disable.                                   |
| `$0F` | `xmtReqGetIcon`         | Fetch media/drive icon resource.                                |
| `$10` | `xmtReqDupInfo`         | Disk-duplicator info.                                          |
| `$11` | `xmtReqGetRawData`      | Copy-protection raw read.                                      |

The IOP services every `ReqKind` it understands by writing the result
into the same XmtMsg payload, setting `ErrorCode`, advancing
`XmtMsg[2].state` to `MsgCompleted`, and raising `iopInt0Active`.

### 17.3 rcvReq event codes (IOP → host, in `ReqKind`)

Posted asynchronously through `RcvMsg[2]` with `iopInt1Active`. The
host must subscribe to slot 2 with `irWaitRcvMessage` at boot.

| Code  | Apple name                 | Meaning                                              |
| ----- | -------------------------- | ---------------------------------------------------- |
| `$01` | `rcvReqDiskInserted`       | Disk just appeared in `DriveNumber`.                  |
| `$02` | `rcvReqDiskEjected`        | Disk just removed from `DriveNumber`.                 |
| `$03` | `rcvReqDiskStatusChanged`  | Drive status changed (e.g. write-protect tab moved).  |

### 17.4 DriveKinds and the qDrive numbering convention

The `DriveKinds[0..3]` byte table returned by `xmtReqInitialize` is
**the** authoritative source of "which drive index is what." The Sony
driver uses each index `N` as the `qDrive` argument when calling
`_AddDrive`, which in turn becomes `ioVRefNum` for `_MountVol`.

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

## 18. SWIM IOP — slot 3 ADBMsg protocol

Slot 3 carries the OS's `ADB Mgr ↔ IOP firmware` packet protocol. The
payload layout is `ADBMsg`:

| Offset | Size    | Name        | Notes                                              |
| ------ | ------- | ----------- | -------------------------------------------------- |
| `+$00` | byte    | `Flags`     | Bitfield; see §18.1.                                |
| `+$01` | byte    | `DataCount` | Length of valid `ADBData[]` bytes (0..8).           |
| `+$02` | byte    | `ADBCmd`    | The ADB command byte (Talk / Listen / Reset / Flush). |
| `+$03` | 8 bytes | `ADBData`   | Up to 8 bytes of in/out payload.                    |

### 18.1 `Flags` bit semantics

| Bit | Name              | Meaning                                                                                              |
| --- | ----------------- | ---------------------------------------------------------------------------------------------------- |
| 7   | `ExplicitCmd`     | Host-initiated command. Firmware must issue `ADBCmd` verbatim on the bus.                              |
| 6   | `PollEnable`      | Autopoll mode. Firmware picks the next enabled device and Talk-R0s it; `ADBCmd` in request is ignored. |
| 5   | `SetPollEnables`  | `ADBData[0..1]` carries a fresh 2-byte DevMap bitmap to replace the firmware's current poll mask.    |
| 2   | `SRQ`             | Service-request pending — at least one device asserted SRQ during the last bus cycle.                |
| 1   | `NoReply`         | Set on the reply when no device answered the issued command.                                          |

`ExplicitCmd` and `PollEnable` are mutually exclusive on a single
request; the OS uses them to disambiguate routing in its `IOPReqDone`
dispatch (one branch goes to `ExplicitRequestDone`, the other to
`ImplicitRequestDone`).

### 18.2 The two transport paths

**A. Explicit command** (`ExplicitCmd = 1`). Host fills `ADBCmd`
with a specific command byte and `ADBData` with up to 8 outgoing bytes
(for Listen). The firmware issues that exact command on the bus. The
reply preserves `ExplicitCmd` and echoes the same `ADBCmd`, with
`ADBData` carrying the Talk reply (or empty + `NoReply` if no device
answered). The host's `ADB Mgr` routes the completion through
`ExplicitRequestDone` and advances its command queue.

**B. Implicit autopoll** (`PollEnable = 1`, `ExplicitCmd = 0`). The
OS posts an autopoll request when `fDBExpActive = 1` in ADBVars. The
`ADBCmd` field of the request is **meaningless** — whatever stale
value the buffer happened to hold — because the firmware itself picks
the next device. The firmware:

1. Walks its `DevMap` (16-bit bitmap, bit `N` = address `N` enabled),
   starting from the address after the previous autopoll cycle.
2. Issues `Talk-Reg-0` (`$XC` where `X = addr`) to the first enabled
   address with pending data.
3. Returns the response in `ADBData` and sets the reply's `ADBCmd`
   to the actual Talk command that was issued, **not** to the stale
   value from the request.

That last point matters: the host's `ADB Mgr` uses the reply's
`ADBCmd` to update its `pollCmd` / `pollAddr` globals, which in turn
feed mouse / keyboard event dispatch. A reply with the wrong `ADBCmd`
either misdelivers data (e.g. mouse bytes look like keyboard events)
or sends the OS into a stuck-state loop (e.g. retrying `Flush` on
address 2 forever).

When the OS sets `SetPollEnables` together with `PollEnable`, the
firmware also reads `ADBData[0..1]` as a big-endian 2-byte DevMap and
latches it before the autopoll scan. The reply clears
`SetPollEnables` (one-shot bit).

### 18.3 Reply Int conventions

Both explicit and autopoll completions land on `RcvMsg[3]` with
`iopInt1Active` raised — **not** `iopInt0Active`. (The IOP firmware
does post explicit replies on `XmtMsg[3]` completion initially, but
once the OS has issued its first `irSendRcvReply` on slot 3 and set
`fDBUseRcvMsg = 1`, all subsequent slot-3 traffic — including replies
to host-posted explicit commands — flows over `RcvMsg[3]` via Int1.)
This means the simple "Int0 = reply, Int1 = unsolicited" framing from
§11 does not strictly apply to slot 3; the channel is steady-state
Int1 once initialised.

---

## 19. RPU (RAM Parity Unit) probe at `$50F1E000`

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

## 20. SCSI subsystem (NCR 5380 + SCSI DMA wrapper)

The IIfx pairs a stock NCR 5380 SCSI core (the same custom-marked die
the Mac II / IIx ship with) with an **Apple custom "SCSI DMA" chip**
that wraps it. The 5380 register set is unchanged from the Mac II;
the wrapper layers a programmable bus-master DMA engine on top, with
its own interrupt that feeds OSS source 9.

Primary sources for this chapter: Apple's *Guide to the Macintosh
Family Hardware* 2nd ed. §"SCSI DMA in the Macintosh IIfx" and
§"Interrupts" (which says SCSI IRQ/DRQ in the IIfx is "enabled or
disabled by setting bits in the Interrupt Enable register inside …
OSS in the Macintosh IIfx"); the leaked SuperMario / System 7.1
source tree (`SuperMarioProj.1994-02-09`), specifically
`Internal/Asm/HardwarePrivateEqu.a` for register offsets and bit
definitions and `OS/SCSIMgr/SCSIMgrHW.a` for the driver-side
protocol; the SWIM Design Documents collection (`SWIMDesignDocs/04 -
IIfx scsi chip`); and Apple Technical Note HW 09 "Macintosh IIfx:
The Inside Story" (April 1990, Rich Collyer).

### 20.1 Address space

The SCSI subsystem occupies four contiguous windows in the canonical
IIfx I/O island:

| Range                            | Window           | What it is                                           |
| -------------------------------- | ---------------- | ---------------------------------------------------- |
| `$50F0_8000 – $50F0_9FFF`        | DMA wrapper      | Wrapper registers ($80–$1FF) **and** a 5380-register passthrough for the low 128 bytes ($00–$7F). |
| `$50F0_A000 – $50F0_BFFF`        | 5380 polled regs | Direct access to the eight 5380 registers, $10-spaced (see §20.3). |
| `$50F0_C000 – $50F0_CFFF`        | Pseudo-DMA read  | Every byte read from this window returns the next CDR byte from the chip — used by the CPU-driven pseudo-DMA loops in `FastRead`-class drivers on machines that don't use the wrapper, and as a status-poll address by some MAME-era reverse engineering. |
| `$50F0_D000 – $50F0_DFFF`        | Pseudo-DMA write | Mirror of the read aperture for the host→target direction. Writes go to the chip's ODR with the pseudo-DMA flag set, so the byte enters the buffer when the 5380's DRQ is asserted. |

Every byte in the I/O island is mirrored at the same offset within
each subsequent 256 KB block per the IIfx's normal aliasing rules
— the canonical addresses above suffice for any software-visible
behaviour.

### 20.2 SCSI DMA wrapper register layout

Offsets within the `$50F0_8000` window, byte-addressable, but the
control/state registers are **big-endian 32-bit** values written and
read as longs. Apple's names match `HardwarePrivateEqu.a`:

| Offset (hex) | Apple name | Direction | Notes |
| ------------ | ---------- | --------- | ----- |
| `$000 – $07F` | (5380 passthrough) | R/W | Same byte layout as the polled aperture in §20.3 — i.e. reads/writes of offset `$N0` hit 5380 register `N`. |
| `$080` | `sDCTRL`   | R/W | Wrapper control + status (see §20.2.1). |
| `$0C0` | `sDCNT`    | R/W | DMA byte count (24-bit field in a 32-bit register). Decrements as bytes transfer; the driver reads back zero on success. |
| `$100` | `sDADDR`   | R/W | Physical destination address for read DMA / source address for write DMA. |
| `$140` | `sDTIME`   | R/W | Watchdog timer reload value. The wrapper raises `iTIMEP` (and IRQ, if `INTREN` is set) when the timer fires before the transfer completes. |
| `$180` | `sTEST`    | R/W | Chip self-test register; written `0` during SCSI Mgr init to leave test mode. |

#### 20.2.1 `sDCTRL` bits

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
transfer until `sIDMArx` (§20.3.1) is touched.

### 20.3 NCR 5380 register passthrough

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

#### 20.3.1 `sIDMArx` — Start Initiator DMA Receive

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

### 20.4 Interrupt routing

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

### 20.5 Boot vs runtime access patterns

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

### 20.6 Emulator-side requirements

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
- A wrapper-side burst-transfer engine triggered by a write to
  `sIDMArx` (passthrough offset `$70`) when `DCTRL.DMAEN` is set:
  it must pull `sDCNT` bytes from the 5380's CDR into RAM at
  `sDADDR`, leave `sDCNT` at zero and `sDCTRL.DMAEN` cleared on
  success, and assert OSS source 9 if `INTREN` is set.
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

A/UX 2.0+ additionally uses the wrapper for **scatter-gather** DMA
(linked descriptor lists) and for write-direction transfers via
`sDMAtx`. This document covers only the read-direction path — the
write path mirrors it (write to `sDMAtx` instead of `sIDMArx`,
pull bytes from RAM into the 5380's ODR, raise the same OSS
source 9 IRQ on completion).

---