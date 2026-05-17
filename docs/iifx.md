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

The firmware fronts two distinct functions through the same mailbox:

- **Floppy**. The host posts XmtMsg requests describing the floppy
  operation (seek, read sector, write sector, etc.). The firmware
  programs the SWIM1, runs the data-transfer state machine using its
  internal DMA channel, and posts the result as a slot completion
  (Int0) or as an unsolicited disk-change notification on an RcvMsg
  slot (Int1).

- **ADB**. The firmware bit-bangs the ADB single-wire bus via its
  on-die GPIO. Talk and listen transactions are issued as XmtMsg
  requests; service-request and key/mouse events arrive as RcvMsg
  notifications on Int1.

Bit interactions specific to the SWIM IOP:

- `iopInBypassMode`: always reads 0 (firmware never enables bypass).
- `iopBypassIntReq`: not wired; reads 0.
- `iopSCCWrReq`: not wired; reads 1.

---