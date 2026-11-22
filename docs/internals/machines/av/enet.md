# AV built-in Ethernet

The 840AV's and 660AV's Ethernet: the shared MACE core
([mace.md](../../core/peripherals/mace.md)) at `$50F1C000`, fed by the PSC's
two Ethernet DMA channels, with Apple's station-address PROM at `$50F08000`
and the AAUI port cabled to the emulator's AppleTalk network. Implementation:
[src/machines/av/enet.c](../../../../src/machines/av/enet.c) /
[enet.h](../../../../src/machines/av/enet.h). Hardware reference:
[mace.md](../../../reference/machines/av/mace.md) and
[psc.md](../../../reference/machines/av/psc.md) §4.6.

## Wiring

- **Registers.** Island `$1C000`–`$1C1FF`, register *n* at *n* × `$10`.
- **INTR** drives PSC level 3, bit 0 (`MaceInterrupt`).
- **Transmit, PSC channel 2 (ENetWr).** The driver's `PrimeXmitRS` arms
  one frame per set (Addr the packet, Cnt its length, SENSE|IE|ENABLED)
  and keeps both sets armed. The pump moves the active set's whole count
  into the MACE's transmit FIFO and ends it with EOF; the PSC's terminal
  count interrupts at level 4 (`PSCInterrupt`), the MACE's XMTINT at level
  3. Because Curio's MACE holds one transmit status, the pump moves a set
  only while `mace_tx_ready`, and the core's `tx_ready` hook — XMTFS read by
  `MaceInterrupt` — wakes it for the other set.
- **Receive, PSC channel 1 (ENetRd), chain mode.** Each frame the MACE
  receives is deposited into the next 2048-byte buffer of the active set's
  chain (`av_psc_dma_chain_in`): the four RFS bytes, each on both halves of
  a word, in the first eight bytes; the frame, FCS included (RCVFC = `$00`),
  from offset 16. See [psc.md](psc.md) for the channel semantics.
- **The address PROM.** Eight bytes at `$x1` of each 16-byte group, the
  station address bit-reversed and read backwards from `$51`, XOR of all
  eight `$FF`. The address is 02:00:00:09:07:02 (locally administered).
  The checksum is checked by `.ENET`'s open before anything else; the old
  register stub's PROM failed it, so `.ENET` never reached the MACE.
- **The cable.** `wire_tx` hands every frame the MACE puts on the wire to
  the AppleTalk network (`atalk_conn_ether_input`), and the network's
  frames for this machine come back through `mace_wire_rx` — the port is
  attached with `atalk_conn_attach_ether` when the part is built and
  detached when it is deleted. The network speaks EtherTalk Phase 2 there
  ([appletalk.md](../../../reference/protocols/appletalk.md), Part I-B;
  `appletalk.h`, "EtherTalk").

## Timing

Coarse, and there only so that nothing completes inside the register write
that started it (`PrimeXmitRS` toggles its next-set offset after the arming
write; a synchronous completion would interrupt between the two). A
transmit set moves 20 µs after the touch that made it ready; received
frames reach memory one per wire time (800 ns a byte at 10 Mb/s plus the
9.6 µs gap). The pumps are two scheduler events (`enet.tx`, `enet.rx`),
registered at construction and armed only while there is work: by the
PSC's Ethernet touch hook (any write to channel 1's or 2's registers) and
by the core's `rx_ready`/`tx_ready` hooks.

## Reset and checkpoint

The board reset (`av_bus_reset`) resets the core (its RESET pin) and drops
the pending pump events. The checkpoint part `mace` is the core's field
list; the pumps are scheduler events and travel in the scheduler's queue.
The AppleTalk side keeps nothing of the Ethernet across a checkpoint
beyond what every connection keeps: to the restored Mac the server has
restarted, and the host relearns the guest's station from its next frame.

## Testing

- `tests/integration/av-ethertalk-afp` — 840AV and 660AV, System 7.1:
  Network control panel → EtherTalk Built In (which opens `.ENET`, whose
  three loopback self-tests must pass), Chooser → AppleShare lists the
  emulated server over Ethernet; on the 840AV, Guest login, the volume
  mounted and a 410 KB file copied off it.
- `tests/integration/av-mace-regs` — the register map.
- Unit: `tests/unit/suites/mace` (the core), `tests/unit/suites/psc` (the
  channels), `tests/unit/suites/atalk_link` (EtherTalk).
