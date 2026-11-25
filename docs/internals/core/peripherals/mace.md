# MACE Ethernet core

The **MACE** is AMD's Am79C940 Ethernet controller as Apple's Curio ASIC
embeds it. One core serves every machine that carries one — the AV Quadras
(840AV/660AV), the TNT Power Macs (7500/8500/9500 and their kin) and, by the
same silicon, the PDM boards — in
[src/core/peripherals/mace.c](../../../../src/core/peripherals/mace.c) /
[mace.h](../../../../src/core/peripherals/mace.h). What differs between the
machines is around the core: the base address, the DMA engine that feeds it,
the interrupt line, and what is on the cable. Each family supplies those
through a small glue file:

| Family | Glue | DMA | Interrupt | Cable |
|---|---|---|---|---|
| AV | [enet.c](../../../../src/machines/av/enet.c) ([enet.md](../../machines/av/enet.md)) | PSC channels 2 (transmit) and 1 (receive, chain mode) | PSC level 3, bit 0 | the AppleTalk network's Ethernet port (EtherTalk) |
| TNT | [tnt_mace.c](../../../../src/machines/tnt/tnt_mace.c) | Grand Central DBDMA channels 2 and 3 | Grand Central source 14 | none: frames complete and vanish, nothing arrives |
| PDM | not wired | (AMIC) | — | — |

The PDM's MACE window is deliberately an empty, zero-reading block in
`amic.c` (MkLinux's Ethernet probe depends on it); wiring the core there
needs the AMIC's Ethernet DMA first. The hardware page is
[mace.md](../../../reference/machines/av/mace.md).

## Scope

- **The register file**: thirty-two byte registers, register *n* at
  offset *n* × `$10` on Curio, with the data manual's reset values
  (p. 74). The family decodes the stride and calls
  `mace_read/peek/write(m, reg, …)`.
  - CHIPID reads `$0940`, the "B0" Curio.
  - IR is read-to-clear; INTR (`mace_set_irq`) is `IR & ~IMR`, one = masked.
  - MPC, RNTPC and RCVCC are read-to-clear and wrap (MPC raising MPCO).
  - BIUCC.SWRST resets the core and reads back clear; FIFOCC.XMTFWU /
    RCVFWU reset their FIFO and clear themselves (data manual p. 66);
    UTR.RTRD sticks until reset.
  - IAC: PHYADDR / LOGADDR restart the PADR / LADRF pointers (LS byte
    first) and clear after 6 / 8 accesses; ADDRCHG clears ENRCV and
    completes at once (nothing is ever in flight).
- **Transmit** (`mace_tx_write` + `mace_tx_eof`, the FIFO-direct path a DMA
  engine drives; XMTFIFO writes for programmed I/O). At EOF the frame is
  padded to 60 bytes (APADXMT) and sent, if ENXMT is set — otherwise it is
  held until MACCC turns it on. Completion is immediate and good: XMTFS =
  XMTSV, XMTRC = 0, XMTINT.
- **Curio's one-deep transmit status.** The embedded core buffers one
  status, not the discrete part's two (the PDM driver's `MAX_STAT_PEND 1`,
  "MACE can only buffer 1 status (bug)"). `mace_tx_ready` (TDTREQ in PR) is
  false while a status is unread, a frame ended meanwhile is held, and
  reading XMTFS — the drivers read XMTRC first — sends it and calls the
  `tx_ready` hook so the DMA engine can move the next one.
- **Receive.** A frame from the cable (`mace_wire_rx`) or the loopback
  passes ENRCV, the loopback state and the address filter — PADR, broadcast
  (unless DRCVBC), multicast by the LADRF hash, everything in promiscuous
  mode — and is queued with its FCS appended (CRC-32, as on the wire) or,
  with ASTRPRCV, pad and FCS stripped. Eight frames fit; more are missed
  (MPC). `mace_rx_take` hands the head frame and its four RFS bytes to a
  DMA engine and raises RCVINT; RCVFIFO/RCVFS reads do the same for
  programmed I/O.
- **The LADRF hash** is the top six bits of the complemented, reflected
  CRC-32 of the destination address — the data manual's "high order 6 bits
  of the FCS", the drivers' `DoCRC`/`mace_crc`, and bmac.c's hash. The unit
  suite checks it against a bit-serial reference.
- **Loopback** (UTR bits 2:1). INTLPB and MENDECLPB return the frame
  through the receiver and keep the cable out of it both ways; EXTLPB also
  puts it on the cable. With RCVFCSE clear the looped frame carries the
  transmitter's FCS — the AV driver's 79-byte self-test frame comes back 83
  bytes long.
- **No timing.** A frame completes when the DMA engine ends it and arrives
  when the cable offers it; the AV glue adds the wire time.

## Hooks (`mace_ops_t`)

| Hook | Called | AV | TNT |
|---|---|---|---|
| `wire_tx` | a frame goes on the cable (not in internal loopback) | the AppleTalk network's `atalk_conn_ether_input` | unbound |
| `rx_ready` | a frame waits in the receive FIFO | schedule the ENetRd pump | unbound (the receive port offers nothing) |
| `tx_ready` | the transmitter can take another frame | schedule the ENetWr pump | unbound (DBDMA polls s5 = XMTSV) |

## Checkpoint

`MACE_CP_FIELDS` in mace.c is the one list for both directions
(checkpoint.h, "Field lists"): every register, the address RAM and its
pointers, the counters, the last RFS and its read index, the frame in the
transmit FIFO, the receive queue and the INTR level. `MACE_CP(OP, cp, m)`
makes the core an entry of a board's own list (TNT's `TNT_IO_CP_FIELDS`);
the AV glue saves it as its machine part. Hooks are rebound on the fresh
core; `mace_restore` re-drives INTR.

## Testing

`tests/unit/suites/mace/` drives the core as Apple's AV driver does: the
reset values and chip ID, the self-clearing bits, the IAC protocol, the
one-deep transmit status with a held second frame, ENXMT holding a frame,
the open-time self-test frame in all three loopback modes, the address
filter and LADRF hash, ASTRPRCV, FIFO overflow into MPC, programmed-I/O
receive, and a checkpoint round trip. The AV wiring is covered end to end
by `av-ethertalk-afp`; the register map by `av-mace-regs`.
