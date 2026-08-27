# The Symbios 53C8xx SCRIPTS engine

`src/core/peripherals/pci/cards/sym53c8xx.h` — shared state and register map
`src/core/peripherals/pci/cards/sym53c825.c` — the chip (PCI identity, registers, BARs)
`src/core/peripherals/pci/cards/scripts53c8xx.c` — the instruction engine

The Symbios Logic 53C825A is the fast/wide SCSI controller the Apple Network
Server carries two of, in place of the Macintosh boards' MESH. It is the
first SCSI controller in this repository that is not register-driven.

Every other one — the 5380, the 53C94/96, MESH — works the same way: a
driver writes a command byte and polls a status register. A 53C8xx does not.
It **fetches and executes a small instruction set out of host memory**. A
register-level model that does not run those instructions moves exactly zero
bytes, which is why the engine is the largest single item in the Network
Server work and the whole critical path to booting AIX.

Reference: Symbios Logic, *PCI-SCSI I/O Processors Programming Guide*, v2.1;
LSI Logic, *LSI53C825A/825AE PCI to SCSI I/O Processor Technical Manual*,
v3.1 (2001), chapters 4–6.

## Why two translation units

`scripts53c8xx.c` holds the engine and the chip lifecycle; `sym53c825.c`
holds the PCI face and the parts of the register file that are not plain
store-and-readback. The split follows DBDMA's: the engine reaches host
memory through three narrow functions (`sym53c8xx_read_block`,
`sym53c8xx_write_block`, `sym53c8xx_read32`), so it can be driven against a
mock bus with no PCI, no machine and no SCSI devices underneath it — the
only affordable way to cover five instruction classes and their negative
cases.

## The instruction set

Five classes, dispatched on the top two or three bits of the first dword.
Every instruction is two dwords; Memory Move is the one exception at three.

| Bits 31:30 | Class | Notes |
|---|---|---|
| `00` | **Block Move** | the instruction the part exists for |
| `01`, opcode ≤ 4 | **I/O** | Select, Wait Disconnect, Wait Reselect, Set, Clear |
| `01`, opcode ≥ 5 | **Read/Write** | the register ALU, against SFBR |
| `10` | **Transfer Control** | Jump, Call, Return, Interrupt |
| `110` | **Memory Move** | three dwords: count, source, destination |
| `111` | **Load and Store** | up to four bytes, register ↔ memory |

**Block Move** is where the phase discipline lives. In initiator mode the
instruction names the SCSI phase it expects; if the target is presenting a
different one the instruction does **not** execute, DSP is rewound to point
*at* it, and a PHASE MISMATCH (`SIST0` bit 7) is raised. That comparison is
the single most load-bearing behaviour in the set: a driver drives a whole
transaction by moving one phase at a time and branching on the mismatch.

Table-indirect addressing (bit 28) fetches both the byte count and the
buffer address from a structure at `DSA + a 24-bit signed offset`, which is
what lets SCRIPTS execute an operating system's own I/O data structures
directly.

## The interrupt discipline

Three registers, each with a different law, and a driver spins on all of
them:

* **`ISTAT` is a SUMMARY.** Its `DIP` and `SIP` bits are live views of "does
  `DSTAT` hold anything" and "does `SIST0`/`SIST1` hold anything" — never
  stored state. It is the only register a driver may touch while SCRIPTS
  are running, which is exactly why polled drivers read it.
* **`DSTAT` / `SIST0` / `SIST1` are CAUSES, and they are READ TO CLEAR.**
  Leave a bit standing and the driver re-interrupts forever; clear it a
  moment too early and the cause is lost. `DSTAT`'s `DFE` bit is the
  exception — a live "DMA FIFO empty" condition, not a latched cause, so it
  survives the read.
* **`IRQ/` follows (causes AND enables).** A *masked* fatal condition still
  halts SCRIPTS and still sets its status bit: "the SCRIPTS still stop … but
  the IRQ/ pin is not asserted." Masking an interrupt on this part does not
  mean ignoring the event.
* **`ISTAT`'s `INTF` drives the pin too, and nothing gates it.**
  Interrupt-on-the-fly exists to tell a driver a command finished *without*
  stopping SCRIPTS, so it has no enable bit and it is **write-one-to-clear**
  rather than read-to-clear — a driver that stored back the ISTAT byte it
  had just read would otherwise re-arm the interrupt it was dismissing. A
  model that latches `INTF` without raising `IRQ/` leaves the driver waiting
  forever on an interrupt a perfectly healthy script already sent.

And the rule the DBDMA work paid for once already: **status must change
synchronously with the control write.** A driver writes DSP (or `DCNTL`'s
START DMA bit) and immediately polls; if the engine's effect on
`ISTAT`/`DSTAT` lands later, the driver's first `while (running)` loop spins
forever with nothing to diagnose.

## How a command actually ends — the non-obvious part

Nothing in the instruction set says this, and it is not what you would
guess. The Network Server ROM's own driver loop is:

```forth
begin
  istat 2 and while/if                       \ a SCSI-type cause
    sist@ to sist
    sist 400 and if … true eexit then        \ selection time-out
    sist 4 and if  dcmd@ 98 <>  eexit  then  \ UNEXPECTED DISCONNECT
    …
  istat 1 and while/if  dstat@ to dstat  then \ a DMA-type cause
again
```

There is **no exit on the SCRIPTS `INT` instruction at all.** Every command
ends on **UNEXPECTED DISCONNECT** (`SIST0` bit 2), and success versus failure
is decided by *where* it landed: `dcmd@ 98 <>` is false — no error — only
when the last opcode fetched was `$98`, the Transfer Control `INT` that ends
the script.

So the disconnect must be reported **after** the script has run to its
`INT`, never at the moment the bus goes free. Report it early and `DCMD`
still holds the Clear-ACK opcode, the driver calls the command failed, and
it resets the bus and retries forever. `disconnect_deferred()` exists for
exactly this: the bus is released when the script clears ACK on the last
MESSAGE IN byte, and the cause is raised once the engine halts.

## Endianness — a pin, and this board does not assert it

The chip's `BIG_LIT/` pin selects byte ordering, and Apple's Network Server
developer note describes what the big-endian setting does, which reads like
a statement about the board. The ROM says otherwise, three times over:

* its register accessors **flip**. `see dsp!` at the Open Firmware prompt
  gives `: dsp!  regs >dsp rl!-flip ;` — a byte-reversed longword store,
  which is what a big-endian host needs in order to write a *little-endian*
  chip register;
* its byte offsets are **not repositioned**. In big-endian mode the chip
  moves a byte register's address to `N ^ 3`; the ROM writes `SCNTL1` at
  `+$01`, `SCNTL3` at `+$03`, `SCID` at `+$04`, `CTEST3` at `+$1B` and
  `STIME0` at `+$48` — every one the natural register number;
* its SCRIPTS are stored **byte-reversed in memory**. The buffer the driver
  hands DSP holds `00000240 00000000 06000002 …`, which read little-endian
  is `40020000 00000000 02000006 …` — a Select of target 2 followed by a
  six-byte Command block move.

So `big_endian` is **false** for this board, and it stays a construction
parameter rather than a constant because it is a wiring fact: the
`SYM53C825AJ` variant is little-endian *only* (its `BIG_LIT` pin is replaced
by a JTAG signal), and a socketed 53C8xx on another board may differ.

Two consequences:

* **Instruction fetch** assembles a dword in the strapped order.
* **The register file** is byte-addressed with a register's low byte at its
  low offset, while the processor bus is big-endian and delivers a 32-bit
  store MSB first. So a wide access decomposes in *bus* order and reassembles
  in *chip* order — which is precisely the swap the ROM's `-flip` words are
  compensating for, seen from the other side.

**Data payloads need none of this.** "The first byte in from the SCSI bus
goes to address 0" in both modes, so a Block Move's data is a straight byte
copy either way.

## GPIO0 is a presence strap on this board

`GPIO[3:0]` are input pins at power-up with an internal pull-down, so an
unwired part reads `GPREG` as zero — and on the Network Server that is
fatal. Open Firmware's own word is:

```forth
: check-disabled  …  regs >gpreg xb@ 1 and 0=
  if  "disabled" encode-string "status" property  then … ;
```

`GPIO0` low means "this fast/wide channel is not fitted": the node gets
`status "disabled"`, and every later `open` of it fails with `Can't open
SCSI host adapter`. The board pulls `GPIO0` high, so `gpio_strap` does too.

## What the model does not do

* **Reselection.** There are no competing initiators and no disconnecting
  targets on the shared bus model, so a reselection never arrives and
  `Wait Reselect` parks forever unless the driver rings its doorbell.

  Parking is the behaviour, not a shortcut around it. With `SIGP` clear
  the part waits; with `SIGP` set it takes the instruction's alternate
  address and clears the bit. So the engine stops with DSP pointing *at*
  the instruction and raises nothing, and a write of `SIGP` re-executes
  it. Taking the alternate address unconditionally looks like the
  forgiving choice and is the opposite: a driver's idle script is a short
  ring that ends in `Wait Reselect` and jumps back to its own start, so
  the engine runs that ring at host speed until the watchdog stops it and
  the driver waiting on the interrupt never gets one. That is exactly how
  the Network Server's own AIX bootstrap hung.
* **Target mode.** The part supports it; nothing this repository emulates
  uses it. A target-mode I/O instruction reports an illegal instruction
  rather than pretending.
* **Transfer rates.** Synchronous and wide negotiation are *answered* —
  a fast/wide channel is expected to negotiate and a chip that always
  rejected would be lying about the part — but the emulated bus has no
  timing, so what is modelled is the agreement, not the rate it implies.
* **The DMA FIFO.** `DFE` reads as permanently empty. Nothing observes the
  FIFO except error recovery, which this model never enters.

## Runaway protection

One `sym53c8xx_start()` runs until the script stops itself. `SYM825_INSN_BUDGET`
(200 000 instructions) is far above any real SCRIPT — Open Firmware's probe
and AIX's driver both run a few dozen per connection — and hitting it raises
the chip's own watchdog cause (`DSTAT` `WTD`) and halts the engine, rather
than taking the emulator down with the guest.
