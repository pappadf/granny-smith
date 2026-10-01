# BMAC Ethernet cell

**BMAC** ("BigMac") is the 10BASE-T Ethernet MAC inside Heathrow, the beige
Power Macintosh G3's I/O controller. It is implemented machine-independently
in [src/core/peripherals/bmac.c](../../../../src/core/peripherals/bmac.c) /
[bmac.h](../../../../src/core/peripherals/bmac.h), on the shared DBDMA engine;
the Gossamer family wires it in
[gossamer_bmac.c](../../../../src/machines/gossamer/gossamer_bmac.c). The
hardware page is [bmac.md](../../../reference/machines/g3/bmac.md).

## Scope

- The 16-bit register file on 16-byte centres, `$000`–`$770`. Everything
  is a read/write latch except the registers below.
  - `CHIPID` reads `$B1`.
  - `STATUS` clears when it is read.
  - `TXRST` reads back 0.
  - `MIFCSR` bit 3 reads 0: no PHY drives MDIO.
  - `XCVRIF` bit 8 is the link, 0 = up.
- **The serial EEPROM** behind `SROMCSR`, a 64 × 16 Microwire part.
  - The station address is in words 10–12, each byte bit-reversed.
  - The READ command and sequential reads are modelled; other opcodes are
    ignored.
  - Open Firmware reads it to build `local-mac-address`, and so does
    Apple's Mac OS X driver.
- **Transmit.** Channel 2's `OUTPUT_MORE`/`OUTPUT_LAST` pieces are
  assembled into one frame.
  - At `OUTPUT_LAST` the frame is padded to 60 bytes, `FrameSent` is set,
    and the frame either goes to the wire or loops back.
  - S5, which the drivers' `WAIT_IF_FALSE` tests, is held true: transmission
    is synchronous.
- **Receive.** Channel 3's `INPUT_*` descriptor ends at the end of the
  frame.
  - The descriptor's residual is the unused tail and its status carries S6
    (`$8440` for a Linux slot).
  - The buffer holds the frame, the FCS (when `RXCFG` `$0100` is set) and a
    big-endian 2-byte trailer giving the length including the FCS.
  - Apple's `BRANCH_IF_TRUE(S6)` skips the unused second segment of a slot.
- **The address filter:** our address, broadcast, the multicast hash,
  promiscuous mode, and `RxRejectOwnPackets`.
- **Loopback.** `XIFC` bit 1 or 2 sends a transmitted frame back through
  the filter.
- **No wire.** Any other transmitted frame goes nowhere; `machine.bmac`
  records it as `last_tx`. Nothing arrives except what
  `machine.bmac.receive(hex)` offers. Bridging to a host network is
  separate, future work, as it is for SONIC ([sonic.md](sonic.md)).

## The DBDMA frame hooks

DBDMA's device port moves a byte stream. Ethernet needs the frame edges,
so two optional hooks were added to `dbdma_port_t`; no other port sets
them.

| Hook | Called | BMAC's use |
|---|---|---|
| `out_last` | When an `OUTPUT_LAST` has delivered its last byte. It runs before the branch decision and the result write-back. | Sends the assembled frame. |
| `in_end` | When `in` comes back short, and again when an `INPUT_*` fills exactly. It answers whether the input stopped at a frame end, and consumes that boundary. | Ends the descriptor at the end of the frame. |

`in` returns 0 while a boundary is pending. A frame that ends on a
descriptor's last byte therefore ends that descriptor only, and the next
one waits for the next frame.

## Machine wiring (Gossamer)

**The bus edge.** Heathrow's `+$11000` window, registers little-endian:

- A halfword cycle is the byte-swapped register (`lhbrx`/`sthbrx`).
- A longword cycle carries the register in the low half of the
  little-endian word, as Open Firmware's `rl@-flip` reads it.
- A byte cycle reads one byte lane. DBDMA's `LOAD_QUAD` of `STATUS` after
  a transmit arrives this way, and reading `STATUS` clears it.

**Interrupts.** The cell is source `$2A` (bank 2), a level that is
`STATUS & ~INTDISABLE`. Its DMA channels complete on bank-2 sources 32/33.

**FCR.** A rising edge on bit 31 resets the cell: the registers return to
their power-on values and the EEPROM keeps its contents. Bits 29–30 (the
enable) are logged, not gated.

**Station address.** `00:05:02:47:53:01`: Apple's OUI with a fixed tail,
the same on every run.

**`machine.bmac`.**

- Attributes: `mac`, `link` (read/write), `tx_frames`, `rx_frames`,
  `last_tx`.
- Method: `receive(hex)`.

## Tests

- `tests/unit/suites/bmac`:
  - the EEPROM read, edge for edge, as Apple's driver clocks it;
  - the register semantics;
  - Apple's transmit slot with its S5 wait;
  - Linux's and Apple's receive slots;
  - an exact-fill boundary;
  - the filter, loopback with the self-addressed warm-up frame,
    overflow, and a checkpoint round trip.
- `tests/unit/suites/dbdma` `test_frame_boundaries`: the two hooks on the
  engine alone.
- `tests/integration/gossamer-bmac` runs Open Firmware 2.4's own BMAC
  package:
  - the EEPROM address as `local-mac-address`;
  - receive with `setrxdma`/`poll?`, and transmit with `xmt1`;
  - loopback set from Forth;
  - the BOOTP request `boot enet` broadcasts.

## Debts

- **No host network.**
- **Mac OS 9 networking is unexercised.** No row opens Apple Enet or Open
  Transport over Ethernet.
- **Not modelled:** the collision and error counters (they never move),
  the MII management frames (no PHY), and the hash filter's exact register
  order. Either of the two orders the drivers write admits a frame.
