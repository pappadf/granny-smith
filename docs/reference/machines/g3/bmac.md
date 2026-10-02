# BMAC — the Heathrow Ethernet cell

**Contents:**

1. [Overview](#1-overview)
2. [Register file](#2-register-file)
3. [Behaviour](#3-behaviour) — the serial EEPROM, transmit, receive, filtering, interrupts
4. [Programming model](#4-programming-model) — the init core every driver shares, and Open Firmware's package
5. [Quirks & errata](#5-quirks--errata)
6. [Open questions](#6-open-questions)

---

## 1. Overview

BMAC ("BigMac") is the 10BASE-T Ethernet MAC cell inside Heathrow at `+$11000`. Its device-tree
node is `/pci/mac-io/bmac@11000`, alias `enet`: `compatible "bmac"`, `cell-id $B1`,
`AAPL,interrupts $2A $20 $21`.

- **DMA.** DBDMA channel 2 transmits and channel 3 receives
  ([dbdma.md](../tnt/dbdma.md)).
- **Interrupts.** The cell is source `$2A`, the one bank-2 source; the two DMA channels complete
  on bank-2 bits `$20`/`$21` ([g3.md](g3.md) §4.4, §5.1).
- **Difference from MACE.** BMAC replaces the MACE of the TNT generation. Its 16-bit registers
  replace MACE's bytes. Its station address lives in a serial EEPROM, not a PROM aperture. Its
  receive DMA ends early at the end of each frame and appends a status trailer.
- **Transceiver.** It runs a 7-wire serial transceiver interface, and no MII PHY answers on the
  management port [1][2][3].

Cells with `CHIPID ≥ $C0` are Paddington's "BMAC+", with an MII PHY. That is a different cell and
is not this page's subject.

## 2. Register file

The registers are 16 bits wide on 16-byte centres and little-endian on the bus. Apple's driver
uses `OSReadSwapInt16`/`OSWriteSwapInt16`, Linux uses `in_le16`/`out_le16` [1][2]. Offsets are
from the cell base.

| Offset | Name | Use |
|---|---|---|
| `$000` | XIFC | `$0001` TxOutputEnable, `$0002` XIFLoopback, `$0004` MIILoopback, `$0010` SQE test |
| `$100` / `$120` | TXFIFOCSR / RXFIFOCSR | bit 0 enables the FIFO; the drivers write 0 then 1 |
| `$110` | TXTH | transmit threshold (Apple writes `$FF`, Linux and BSD 4) |
| `$160` | XCVRIF | `$0002` COLActiveLow, `$0004` SerialMode, `$0008` ClkBit (written together, `$000E`); **bit 8, read-only: link, 0 = up** |
| `$170` | CHIPID | low byte `$B1` on this board (Apple names `$B0` Heathrow, `$C0`/`$C4` Paddington) |
| `$180` | MIFCSR | MII bit-bang: `$1` clock, `$2` data out, `$4` drive enable, `$8` data in |
| `$190` | SROMCSR | EEPROM bit-bang: `$1` chip select, `$2` clock, `$4` data **from** the EEPROM, `$8` data **to** it |
| `$200` | STATUS | events, read-to-clear (§3.5) |
| `$210` | INTDISABLE | same bits; a set bit masks the event |
| `$420` | TXRST | write `$0001`; the bit reads 0 when the reset is done, and every driver polls it |
| `$430` | TXCFG | `$0001` TxMACEnable, `$0080` TxDisableFCS (clear: the cell appends the FCS) |
| `$4E0` | PAREG | read once at init, value unused |
| `$500`–`$530` | NCCNT NTCNT EXCNT LTCNT | collision counters, zeroed at init |
| `$540` | RSEED | backoff seed |
| `$620` | RXRST | written 0 |
| `$630` | RXCFG | `$0001` RxMACEnable, `$0040` promiscuous, `$0100` keep the FCS in the buffer, `$0200` RxRejectOwnPackets, `$0800` RxHashFilterEnable |
| `$660` / `$670` / `$680` | MADD2 / MADD1 / MADD0 | station address: MADD0 = bytes 0–1 (`b0 << 8 \| b1`), MADD1 = bytes 2–3, MADD2 = bytes 4–5 |
| `$690`–`$6C0`, `$6E0` | FRCNT LECNT AECNT FECNT, RXCV | receive counters, zeroed at init |
| `$700`–`$730` | HASH3 … HASH0 | 64-bit multicast hash table |

The names follow Apple's `BMacEnetRegisters.h` [1]. The registers between these (inter-packet gap,
slot time, preamble, frame-size limits, state-machine debug) are written by no driver.

## 3. Behaviour

### 3.1 The serial EEPROM

The EEPROM is a 64 × 16-bit Microwire part on `SROMCSR`. The station address is words 10–12,
three words at Apple's `kSROMStartOffset`. Each byte comes out **bit-reversed**: byte `2i` is
`bitrev8(word & $FF)` and byte `2i+1` is `bitrev8(word >> 8)` [1][2].

The read protocol, as Apple's `BMacEnetHW.cpp` clocks it [1]:

- **Start.** Deselect (`SROMCSR = 0`), then shift in start bit 1 and opcode `10`, then six address
  bits, most significant first. Each bit is presented on `$8` with chip select and sampled on the
  rising clock.
- **Data.** Raise the clock and read `$4`, sixteen times: data bit 15 first.

Open Firmware 2.4 reads the same words, with its package's `eread-*` words and `cbit-flip`, to
publish `local-mac-address` [4]. Apple's Mac OS X driver reads only the EEPROM; Linux and the BSDs
read only the property [1][2][3]. A model must therefore keep the two in agreement, and it does
when the firmware itself copies one into the other.

### 3.2 Transmit

- **Frame assembly.** A frame is the bytes of one or more `OUTPUT_MORE` descriptors and a closing
  `OUTPUT_LAST`.
- **Pad and FCS.** The cell pads short frames: OpenBSD hands it 60-byte frames, while Apple hands
  it raw lengths. With TxDisableFCS clear, the cell appends the FCS.
- **Completion.** The drivers test the `ACTIVE` bit in the last descriptor's status [1][2].
- **The S5 wait.** Apple's and Linux's `OUTPUT_LAST` carries `WAIT_IF_FALSE` against `WaitSelect
  $00200020`, so it waits on DBDMA status line S5. Apple follows it with a `LOAD_QUAD` of
  `STATUS`, whose side effect is to clear the register.

### 3.3 Receive

Each frame ends its `INPUT_*` descriptor early.

- **What the descriptor records.** The residual is the unused remainder of `reqCount`. The status
  word written back carries S6. The BSD driver documents `$8440`/`$9440` (RUN | ACTIVE | S6,
  optionally WAKE) from real hardware [3].
- **Apple's slot.** Apple programs two descriptors per slot: `INPUT_MORE` with `BRANCH_IF_TRUE`
  and `INT_IF_TRUE` on S6 (`BranchSelect = InterruptSelect = $00400040`), then `INPUT_LAST`. A
  frame that fits the first segment skips the second [1].

The buffer holds:

```
destination(6) source(6) type(2) payload  FCS(4)  trailer(2)
```

- **The FCS** is present because every driver sets RXCFG `$0100`. Linux and BSD subtract 4 from
  the DMA count [2][3].
- **The trailer** is big-endian. Bit 15 is abort and bits 13–0 are the frame length. Apple reads
  it with `kRxAbortBit`/`kRxLengthMask` [1].
- **The length arithmetic.** Linux computes `RX_BUFLEN − residual − 2`, and Open Firmware's
  `poll?` computes `reqCount − resCount − 4 − 2` [2][4].

### 3.4 Address filtering

The receiver must be enabled. A frame is then accepted when any of these holds:

- the receiver is promiscuous;
- the destination is our MADD address;
- the destination is broadcast;
- the destination is multicast, the hash filter is enabled, and its hash bit is set.

The hash is the CRC-32 (`$04C11DB7`) of the destination, each byte fed least significant bit first.
Its low six bits, bit-reversed, select one of 64 bits [1][2].

With RxRejectOwnPackets set, a frame whose source is our address is dropped. This matters because
Apple and OpenBSD send a frame to themselves right after init [1][3].

### 3.5 Interrupts

- **STATUS bits.** Bit 0 is FrameReceived, bit 5 is RxOverFlow and bit 8 is FrameSent. Bits 9–15
  are the transmit errors and counter wraps, and bits 1–7 the receive counter wraps [1][2].
- **The cell interrupt** is `STATUS & ~INTDISABLE`. Apple masks with `$FCFF` (FrameSent and
  TxUnderrun), Linux with `$FEFE`, OpenBSD with `$FEFF` [1][2][3].
- **The DMA interrupts** are ordinary DBDMA channel completions.

### 3.6 Feature control

These are Heathrow FCR bits [1][2]:

- **Bits 29–30** enable the transceiver and the cell (Apple's `kEnetEnabledBits $60000000`).
- **Bit 31** is the cell reset (`kResetEnetCell`).

The drivers set the enable, pulse the reset with 10–50 ms waits, then program the cell.

## 4. Programming model

### 4.1 The init core

Apple's `_initChip`, Linux's `bmac_init_registers` and OpenBSD's `bmac_init` run the same sequence
[1][2][3]:

1. `TXRST = 1`, then poll until it reads 0.
2. `RXRST = 0`.
3. `XCVRIF = $000E`: serial transceiver mode (Apple only when no PHY answered).
4. Write `RSEED`.
5. Set `XIFC |= TxOutputEnable`.
6. Read `PAREG`.
7. Zero the counters.
8. Write `TXTH`.
9. Write both FIFO CSRs 0 then 1.
10. Read `STATUS`.
11. Write the hash table.
12. Write `MADD0`–`MADD2`.
13. Set `RXCFG = $0B00`, plus `$0040` for promiscuous mode.
14. Write `INTDISABLE`.
15. Start receive DMA, then set the enable bits in `RXCFG` and `TXCFG`.

Apple probes for an MII PHY first. It bit-bangs management frames on `MIFCSR`, and with nothing
driving data-in every address fails, which leaves it on the no-PHY path [1].

### 4.2 Open Firmware 2.4's package

The `bmac` package is built from the following words [4]:

- **Initialization.** `init-bmac` stops both channels, then runs `init-enet-txrx` (the register
  sequence above), `setupdma` and `init-bmac-gossamer`. It enables the receiver for 20 ms and
  disables it again.
- **Receive.** `setrxdma` points channel 3 at one `INPUT_LAST` (`$3000`) on the caller's buffer.
  `poll?` reads the descriptor's resCount: non-zero means a frame arrived.
- **Transmit.** `xmt1 ( adr len -- )` waits for channel 2 to go idle and starts one `OUTPUT_LAST`
  (`$1000`). It then polls `STATUS` (as a little-endian longword) until FrameSent is set, and
  finally waits for the channel to go idle again. Each wait times out after 2 s with
  "BMAC-xmt timeout".
- **Open.** `open` runs the TFTP boot support's BOOTP exchange. The BOOTP request is a 342-byte
  broadcast from UDP port 1234 to 67.

## 5. Quirks & errata

- **No PHY.** Nothing drives `MIFCSR` data-in, so it reads 0. Every driver's Heathrow path assumes
  this: Linux and the BSDs configure a PHY only on BMAC+, and Apple's probe fails at its first
  "MII not floating" check [1][2][3]. A model that answers on the management bus sends Apple's
  driver down a PHY path this board does not have.
- **The status read clears.** Reading `STATUS` for any reason, including a DBDMA `LOAD_QUAD`,
  consumes its events.
- **Two hash register orders.** Linux and Apple's init put hash bits 0–15 in `HASH3`; Apple's
  multicast update path writes them to `HASH0` [1][2]. A false positive only costs the driver a
  software filter pass.

## 6. Open questions

1. **Does the trailer's length include the FCS?** Only Apple's driver reads the field, and its
   upper bound of 1518 suggests it does.
2. **What S5 means on the transmit channel.** "Transmitter done" is the reading of Apple's
   wait-then-`LOAD_QUAD`; the Paddington streaming variant drops the wait.
3. **What `init-bmac-gossamer` does.** Open Firmware's `see` cannot decompile it on this ROM.
4. **The address source in Mac OS 8/9's Apple Enet driver:** the EEPROM or the Name Registry.

## References

1. Apple Computer, *AppleBMacEthernet* driver source (Darwin open source):
   - `BMacEnetRegisters.h`: register offsets and bits, `kEnetEnabledBits`, `kResetEnetCell`,
     `kRxAbortBit`, `kRxLengthMask`;
   - `BMacEnetHW.cpp`: the EEPROM protocol;
   - `BMacEnetPrivate.cpp`: `_resetChip`, `_initChip`, the transmit and receive rings,
     `getHardwareAddress`;
   - `BMacEnet.cpp` and `BMacEnetMII.cpp`.
2. Linux, `drivers/net/ethernet/apple/bmac.c` and `bmac.h`, and
   `arch/powerpc/platforms/powermac/feature.c` (`heathrow_bmac_enable`).
3. NetBSD and OpenBSD, `sys/arch/macppc/dev/if_bm.c` and `if_bmreg.h`.
4. Open Firmware 2.4 in the beige G3 Rev C ROM (`$78F57389`). The package was observed at the
   firmware prompt, on the model: its `.properties`, its `words`, and the decompiled `xmt1`,
   `poll?`, `setdmadesc`, `init-bmac` and `init-enet-txrx`.
