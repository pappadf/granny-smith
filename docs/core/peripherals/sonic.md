# DP83932 SONIC Ethernet Controller

The **National Semiconductor DP83932 SONIC** is the Quadra generation's
NIC, implemented machine-independently in
[src/core/peripherals/sonic.c](../../../src/core/peripherals/sonic.c) /
[sonic.h](../../../src/core/peripherals/sonic.h). Ground truth is the
DP83932B datasheet (§3/§4) plus **Apple's own ROM self-tests**
(`OS/StartMgr/UnivTestEnv/SONIC_*.c`) — the unit suite in
`tests/unit/suites/sonic/` mirrors those tests over mock guest memory.

## v1 scope

- Full 16-bit register file with the quirks the self-tests pin — e.g.
  TPS reads back inverted, TFS reads back width-shifted, register
  bit-march behaviour, IMR/ISR interrupt gating.
- **CAM load/readback through real descriptor DMA.**
- **MAC / ENDEC / transceiver loopback** through the real RRA/RDA/TDA
  linked-list buffer management.
- **No wire:** non-loopback transmissions complete successfully into the
  void and nothing is ever received. Bridging SONIC to a network is
  separate, future work.

## Bus mastering

SONIC DMAs descriptors and packet data to/from **guest-physical** memory
(no IOMMU on this family — the CPU MMU is not in the path). Machines
install memory hooks that resolve through the bus-side physical resolver
(`mmu_read_physical_*` / `mmu_write_physical_*`); unit tests install mock
hooks over a flat buffer.

## Machine wiring (Quadras)

- Register window in the I/O island at `$A000`: the 16-bit register
  value rides the **low half of each 4-byte slot**; byte writes commit on
  byte 3 (the substrate latches the high byte — that latch checkpoints
  with the machine).
- INT (active-high from the chip) wires active-low to VIA2 PA0 through
  the family /SLOTIRQ aggregate. The A/UX level-3 interrupt remap is not
  modeled.
- The **Apple MAC PROM** at island `$8000` carries the Apple
  presentation: each byte is the bit-reversed MAC address byte, and the
  XOR of all 8 bytes is `$FF` (`SonicEnet.a` `@GetAddr`/`NormAddr`).

## Power-on values

Beyond the datasheet's usual zeros, two registers come up non-zero at a
hardware reset and the model sets both: `TCR` gets `NCRS | BCM` (§4.3.4)
and **`EOBC` gets `$02F8`** (§4.3.9). EOBC is the word count below which
the receiver stops treating what is left of the RBA as usable; at zero the
"last buffer in the RBA" test never fires, so a driver that relies on the
power-on value would fill past the end of the buffer area instead of
switching.

## Debts

- The System's `.enet` driver / EtherTalk end-to-end is unexercised (the
  test images carry no Network software).
- The SONIC watchdog timer is static (ST/STP latch only, no TC rollover
  interrupt).

`sonic_checkpoint` serializes the plain-data prefix, up to the first
pointer, as every other device in the tree does — this was the last
whole-struct checkpoint write anywhere, and it put three host addresses
(the IRQ callback, its context and the memory port's function pointers)
into every save file. `sonic_set_irq_callback` re-drives an asserted INT
level on rebind.

The byte-write latch that the machine wiring note above describes belongs
to the **board**, not the chip: Apple's register window puts the 16-bit
value in the low half of a 4-byte slot, so a byte write must be
reassembled before it reaches the DP83932, which has no byte-write latch of
its own. `struct sonic` carried a `byte2_latch` field that nothing ever
read; it is gone. A second board wiring SONIC with different spacing would
need its own latch, which is exactly why it cannot live in the chip.
