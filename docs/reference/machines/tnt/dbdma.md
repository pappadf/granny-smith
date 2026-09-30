# DBDMA — the descriptor-based DMA engine

**Contents:**

1. [Overview](#1-overview) — what the architecture is, which machines carry it, division of labor between engine and device, the eleven Grand Central channels, clocking and bus arbitration
2. [Register file](#2-register-file) — the per-channel register block: ChannelControl, ChannelStatus, CommandPtrLo, the three condition-select registers, the optional extended region, reset state
3. [Behaviour](#3-behaviour) — the command descriptor and its command words, the eight commands, the channel state machine, status write-back, conditional branch/interrupt/wait, error handling, interrupts, endianness and coherency
4. [Programming model](#4-programming-model) — device-tree enumeration, boot-time reset, the canonical start/stop/reset sequences, descriptor publication, ring building, per-device driving, the firmware boot beep
5. [Quirks & errata](#5-quirks--errata)
6. [Open questions](#6-open-questions)

---

## 1. Overview

### 1.1 What the part is

**DBDMA** — descriptor-based direct memory access — is Apple's single DMA architecture for the PCI Power Macintosh. The 7500/8500 Developer Note's glossary defines it as "a DMA technique using DMA descriptor lists that are read from memory by the IC performing the DMA transfers" [2] p. 33; the same note's architecture chapter states the design goal in one sentence: the Grand Central IC "provides DBDMA support for all I/O transfers, including transfers through its internal I/O controllers as well as transfers through the Curio IC for other I/O devices" [3] p. 11. Where every earlier Macintosh generation gave each I/O device a bespoke DMA ring or buffer-pair engine (the AMIC of the 6100/7100/8100 being the immediate predecessor — see [amic.md](../pdm/amic.md)), DBDMA is one engine, programmed one way, for every device: SCSI, Ethernet, serial, floppy and audio all present the identical channel interface to software, differing only in what their device cells do with the bytes.

The architecture is a published, standardized thing, not a per-machine accident. Apple documents it in *Designing PCI Cards and Drivers for Power Macintosh Computers*, in the chapter "Descriptor-Based DMA" [4], and the 1996 Common Hardware Reference Platform I/O Device Reference adopts it wholesale as the DMA architecture of the Apple legacy devices a CHRP system must implement: "DBDMA is a programming model used for MESH SCSI and ESCC today and potentially other devices in the future" [1] §12.2 p. 147. That standard's Chapter 15 is the most complete public register- and command-level description of the engine [1] pp. 171–194, and this page follows its terminology.

The architecture's own summary of itself, from that standard [1] §15.2 p. 171:

- **Fixed-size commands.** All command entries are 16 bytes long.
- **Fewest processor interrupts.** No more than one interrupt is needed per I/O operation.
- **Initiator-resident data structures.** All command and status list components are expected to be located in initiator-resident system memory.
- **Simple command structure.** Descriptors are organized in a simple linear array; linking is accomplished through an optional branch action, either embedded in a data transfer command or through a NOP command that specifies a branch.
- **Logically distinct channels.** Different DMA channels serve logically distinct data-transfer paths (full-duplex transmit and receive, for example), so driver software never assigns channels dynamically.
- **Conditional waits.** DMA commands can be conditionally suspended on the channel's internal status bits, which a device can drive, so a channel can pause until a flow-control conflict is resolved.
- **Embedded branches.** The INPUT, OUTPUT and NOP commands carry a conditional branch capability, to minimize command latency in channel programs that loop.

### 1.2 Machines that carry it

| Machine | Apple codename | DMA engine | Channels | Notes |
|---|---|---|---|---|
| Power Macintosh 7200 | Catalyst | Grand Central (Apple 343S1125) | 10 populated | no MESH cell, so channel 10 is unused; the 53C94 is the machine's only SCSI controller [6] |
| Power Macintosh 7500 | TNT | Grand Central | 11 | plus two channels in the Plan B IC for video input [2] p. 21 |
| Power Macintosh 8500 | TNT | Grand Central | 11 | plus Plan B's two video-input channels [2] p. 21 |
| Power Macintosh 9500 | TNT | Grand Central | 11 | [3] p. 11 |

Grand Central is "a custom IC that provides core I/O services" [2] p. 33 hanging off the first Bandit PCI bridge, decoding a 128 KB window at the base of that bridge's PCI I/O space; its eleven channel register blocks occupy the block at offset +$8000 within that window (§2.1). The Open Firmware device tree publishes the chip as the node `gc` with `device_type "dbdma"` and `model "AAPL,343S1125"` [6]. On the 7500 and 8500 a second, smaller DBDMA engine lives in the **Plan B** IC (Apple 343S1138, tree node `planb`, `device_type "video-in"`): "The Plan B IC provides two DBDMA channels for the 7196 DESC IC: a DBDMA write channel and a DBDMA read channel. The DBDMA write channel takes data from the pixel FIFO buffer in the 7196 IC, attaches an appropriate DMA address, and performs a PCI write operation. The DBDMA read channel reads the 1-bit-per-pixel clip mask from main memory" [2] p. 21. Apple used the one architecture everywhere; Plan B's channels follow the same conventions as Grand Central's (§4.6.7).

The architecture outlives this family: the CHRP I/O Device Reference (1996) mandates the identical channel model for CHRP's Apple legacy devices [1] Chapter 15, and points to the book *Macintosh Technology in the Common Hardware Reference Platform* for "a more extensive DBDMA architecture" [1] §15.1 p. 171, [10].

### 1.3 Division of labor: engine and device cell

The standard is explicit that a DBDMA controller has "two distinct sets of registers": the **channel registers**, which are the architecture described in Chapter 15, and the **device registers**, "which are device specific and are not described in this appendix" [1] §15.4.1 p. 173. The engine moves unaddressed data bytes between system memory and a device **stream** selected by a 3-bit key; the device cell owns everything else — the SCSI sequencer, the codec, the FIFOs, the phase logic. The device also owns the low byte of the channel's status: each channel has "up to eight general purpose state bits" (s7..s0) that software can write through ChannelControl and that a device can additionally set and clear "through hardwired connections — for example, when errors occur or to indicate the completion of a logical record" [1] §15.4.3 p. 175. Commands can test those bits at completion and branch, interrupt or wait on the result (§3.6). The meanings of the s-bits are channel-specific and are defined by each device — for the ESCC, the standard documents them (§4.6.4); for the SCSI cells, they are not publicly defined (§6.3).

### 1.4 The eleven channels and their addresses

Each channel occupies a 256-byte-aligned block; on the TNT machines the channel blocks are 256 bytes apart at Grand Central + $8000 + n × $100. Channel number and Grand Central interrupt number coincide — the identity is published by the firmware itself: the `mesh` node declares its DMA register at offset $8A00 and its `AAPL,interrupts` property contains 10, and every other channel's address comes from its own node's `reg` property [6]. The full assignment, from the firmware tree and the shipped interrupt classification [6]:

| Ch | Address | Device cell | Direction | Device's own interrupt | 68k IPL |
|---|---|---|---|---|---|
| 0 | $F3008000 | 53C94 external SCSI (in the Curio IC) | data in/out | 12 | 4 (DMA), 2 (chip) |
| 1 | $F3008100 | SWIM III floppy (in Grand Central) | data in/out | 19 | 4 (DMA), 2 (chip) |
| 2 | $F3008200 | MACE Ethernet (in the Curio IC) | transmit | 14 | 4 (DMA), 3 (chip) |
| 3 | $F3008300 | MACE Ethernet (in the Curio IC) | receive | 14 | 4 (DMA), 3 (chip) |
| 4 | $F3008400 | ESCC channel A (modem) | transmit | 15 | 4 (both) |
| 5 | $F3008500 | ESCC channel A (modem) | receive | 15 | 4 (both) |
| 6 | $F3008600 | ESCC channel B (printer) | transmit | 16 | 4 (both) |
| 7 | $F3008700 | ESCC channel B (printer) | receive | 16 | 4 (both) |
| 8 | $F3008800 | AWACS audio | output | 17 | 4 (DMA), 2 (chip) |
| 9 | $F3008900 | AWACS audio | input | 17 | 4 (DMA), 2 (chip) |
| 10 | $F3008A00 | MESH internal SCSI (bus interface via Grand Central) | data in/out | 13 | 4 (DMA), 2 (chip) |

The machine's NanoKernel classifies every Grand Central interrupt number 0 through 10 — all eleven channels — into emulated 68k interrupt priority level 4, alongside both ESCC channels (15, 16); the kernel's own annotation reads "all DMA IRQs will get the priority level 4" [6]. A device whose DMA is running therefore raises two independently maskable interrupts: its chip number and its channel number (§3.8).

The full-duplex pairing is the architecture's "logically distinct channels" rule made concrete: Ethernet gets one transmit and one receive channel, each ESCC channel gets one of each, audio gets output and input. Half-duplex devices — both SCSI cells, the floppy — get one channel each [1] §15.5.1 p. 179.

### 1.5 Clocking and bus arbitration

Grand Central "is connected to the PCI bus and uses the 33 MHz PCI bus clock" [2] p. 18. As a bus master it sits at the top of the PCI arbitration order: "Grand Central IC (I/O device controller; highest priority)" above the PCI slots and the Bandit master, which arbitrate round-robin [2] p. 18, [3] p. 11. The engine has no software-visible timing of its own: a channel paces at the rate its device cell consumes or produces bytes (a codec at its sample rate, a SCSI cell at the negotiated bus rate — the internal SCSI bus runs at up to 10 MB/s and the external bus at up to 5 MB/s [2] p. 19), and only the buffer-fill and command-fetch behavior described in §3 is observable from software.

---

## 2. Register file

### 2.1 The channel register block

Every channel presents the same register block, summarized here from the CHRP channel-register table [1] Table 321 p. 173 and the firmware-observed layout [6]. All registers are 32-bit and **little-endian** (§3.9). The offset appears literally in the command's address field when a register is accessed indirectly (`Command.key = KEY_REGS`); accessed directly by the host, the address is the system-defined channel base plus the offset [1] §15.4.1 p. 173.

| Offset | Register | Required/optional | Function |
|---|---|---|---|
| +$00 | ChannelControl | required | write port for the bits presented in ChannelStatus |
| +$04 | ChannelStatus | required | observe the channel; writes ignored |
| +$08 | reserved (CommandPtrHi in Apple's header [5]) | — | 64-bit addressing; unused on a 32-bit-physical machine |
| +$0C | CommandPtrLo | required | physical address of the next descriptor |
| +$10 | InterruptSelect | optional [1]; not attested on Grand Central (§6.1) | interrupt condition select |
| +$14 | BranchSelect | optional [1]; not attested on Grand Central | branch condition select |
| +$18 | WaitSelect | optional [1]; not attested on Grand Central | wait condition select |
| +$1C and above | optional extended registers (§2.7) | — | no shipped software touches them |

Apple's own interface header lays the block out to a full 256 bytes — the required registers, then `dataPtrHi/Lo`, `byteCount`, `data2PtrHi/Lo`, `transferModes` and `addressHi` at +$10 through +$2C, everything beyond +$30 unimplemented, padding the block to 256 bytes [5]. All of those extended registers are marked implementation-optional, and none of them appears in the CHRP channel set; whether Grand Central decodes anything above +$18 is unattested (§6.2). The shipped software contract uses only +$00, +$04 and +$0C: the ROM's own SWIM III driver carries exactly $F3008100, $F3008104 and $F300810C in its literal pool — the channel's ChannelControl, ChannelStatus and CommandPtrLo [6].

### 2.2 ChannelControl (+$00)

ChannelControl is "the writing port for bits that are presented in the ChannelStatus register" [1] Table 324 p. 174. A write is a **mask/value pair** in one 32-bit word: bits 31–16 are the mask, bits 15–0 the value, and "bits in the lower half of the ChannelControl register are written only if the corresponding bits in ChannelControl.mask are set" [1] Table 324 p. 174. Bits not selected by the mask are untouched. This is why every driver on the platform writes paired constants — set run is $80008000, clear run is $80000000 — and Apple's header defines the full set [5]:

```
kdbdmaSetRun     $80008000     kdbdmaClrRun     $80000000
kdbdmaSetPause   $40004000     kdbdmaClrPause   $40000000
kdbdmaSetS0..S3  $00010001 … $00080008     (one bit each)
kdbdmaClrS0..S3  $00010000 … $00080000
kdbdmaClrAll     $F00F0000
```

A read of ChannelControl returns zero [1] Table 322 p. 173 — it is effectively write-only, and the only way to observe a channel is ChannelStatus. The standard notes that certain ChannelStatus bits "are not writeable or should be written only to a set state or to a cleared state" [1] Table 324 p. 174; the per-bit rules are in §2.3.

### 2.3 ChannelStatus (+$04)

The read-only image of the channel. "A write to the ChannelStatus register shall be ignored" [1] §15.4.3 p. 174. The initial value is zero [1] Table 322 p. 173.

| Bit | Name | Owner | Meaning |
|---|---|---|---|
| 15 | run | software | channel enabled |
| 14 | pause | software | execution suspended |
| 13 | flush | software sets / hardware clears | force buffered input data to memory |
| 12 | wake | software sets / hardware clears | refetch the command at CommandPtr |
| 11 | dead | hardware | channel halted on a catastrophic error |
| 10 | active | hardware | a command is in progress |
| 9 | — | — | reserved |
| 8 | bt | hardware | the last command's branch was taken |
| 7–0 | s7..s0 | software and/or device | general-purpose status bits, device-defined |

Per-bit behavior, from the standard [1] §15.4.3 pp. 174–175:

**run (15).** Set by software to start execution — "this should be done only after the CommandPtr registers are initialized; otherwise, the operation of the channel is undefined." Clearing it aborts the channel: transfers terminate, status is returned, an interrupt is generated if the current command's `Command.i` field requested one, and "data that is stored temporarily in channel buffers may be lost."

**pause (14).** Set by software to suspend command processing. Hardware suspends transfers and command execution and then clears **active**; software must clear pause for processing to resume.

**flush (13).** Set by software to force a channel executing an INPUT_MORE or INPUT_LAST command to update memory with data received from the device but not yet written. When the update completes, hardware writes the xferStatus and resCount fields in the current memory-resident command and clears the flush bit — "a partial status update is characterized by 1 in the flush bit of the xferStatus field and a final status update is characterized by 0" (§3.5).

**wake (12).** Set by software "to cause a channel that has gone idle to wake up, refetch the command pointed to by CommandPtr, and continue processing commands. The channel becomes idle after executing a STOP command. The STOP command does not increment the CommandPtr register" [1] p. 175 — so wake after a STOP refetches the overwritten STOP slot, which is the backbone of the command-appending idiom (§4.5). Wake "shall be reset to 0 by hardware immediately after each command fetch."

**dead (11).** Set by hardware "when the channel halts execution due to a catastrophic event such as a bus or device error." The current command is terminated, hardware attempts the status write-back, further commands are not executed, and if a hardwired interrupt signal is implemented the controller generates an unconditional interrupt. Dead also forces active low. "Hardware shall reset ChannelStatus.dead to 0 when the ChannelStatus.run bit is cleared by software" — the only way out.

**active (10).** Set by hardware when software sets run; cleared when software clears run or sets pause, after a STOP command, or when hardware sets dead.

**bt (8).** Set at the completion of NOP, INPUT and OUTPUT commands to record whether the command's branch was taken — "the presence of this bit in the Command.xferStatus field allows software to follow the actual channel program flow with minimal overhead."

**s7..s0 (7–0).** The device-status byte. Writable through ChannelControl, readable here, and in many implementations also driven directly by the attached device (§1.3, §3.6).

The standard's own summary of ownership: "The run and pause bits are control bits that are set and cleared by software. The flush and wake bits are command bits that are set by software and are cleared by hardware when a given action has been performed. The dead, active, and Bt bits are hardware status bits. The bits s7..s0 can be used for general purpose status and control" [1] p. 175.

### 2.4 CommandPtrLo (+$0C)

The address of the next command entry to be fetched. "Since all channel commands are 16-byte aligned, the four least-significant bits of the CommandPtrLo register must always be written with zeros. If they are written with a nonzero value, the operation of the channel is indeterminate and the value returned when these bits are read is undefined" [1] §15.4.4 p. 176; requirement 15-2 makes the low bits of aligned pointers reserved so that unaligned values behave deterministically [1] p. 173. The register can be read at any time, but "writes to the CommandPtrLo register are ignored unless the ChannelStatus.run and ChannelStatus.active bits are both 0" [1] p. 176 — a channel that is running, paused-mid-command or merely parked (run set, active clear after a STOP) is not specified to accept a new pointer, though shipped software does exactly that in one place (§5, §6.6). The initial value is undefined [1] Table 322 p. 173. Drivers read CommandPtrLo to find where a halted channel stopped (§4.3).

### 2.5 Reserved +$08 (CommandPtrHi)

Reserved in the shipping architecture [1] Table 321 p. 173. Apple's interface header names it `commandPtrHi` and marks it implementation-optional [5] — the upper half of a 64-bit command-list address, meaningless on machines with 32-bit physical addressing. No software in any observed corpus reads or writes it; whether Grand Central decodes it is unattested.

### 2.6 InterruptSelect (+$10), BranchSelect (+$14), WaitSelect (+$18)

Three optional condition-select registers, one per conditional action. All three share one format: bits 31–24 reserved, bits 23–16 a **mask**, bits 15–8 reserved, bits 7–0 a **value** [1] Tables 329–332 pp. 176–177. Each generates a condition bit from the device-status byte by masked equality:

```
condition = ((s7..s0 & Select.mask) == (Select.value & Select.mask))
```

InterruptSelect feeds the `Command.i` test, BranchSelect the `Command.b` test, WaitSelect the `Command.w` test (§3.6). "In many implementations there will be no need to allow all the general purpose status bits to generate interrupts. In these cases, the generation of the interrupt condition may be as simple as tying it to a single status bit or tying it to 0" [1] p. 184.

The ESCC chapters of the CHRP standard document a real implementation narrowing these registers: for the ESCC transmit channels "only bits s0 and s5 may be used to generate the Interrupt, Branch and Wait conditions. These are the only bits implemented in the InterruptSelect, BranchSelect, and WaitSelect registers for these channels"; the receive channels implement s0 only [1] §9.10 pp. 127–128. Whether Grand Central's channels implement the registers at all, or hardwire conditions, is open — no shipped driver programs them, and a power-on value of zero makes every masked-equality condition read true, which is indistinguishable from `*_ALWAYS` behavior for every driver in the observed corpus (§6.1).

### 2.7 The optional extended region (+$1C and above)

Apple's header extends the block with optional registers that the CHRP channel set does not carry: `dataPtrHi/Lo` (+$10/+$14), `byteCount` (+$18), a reserved +$1C, `data2PtrHi/Lo` (+$20/+$24) — the second address for the two-address transfers the key field selects (§3.3) — `transferModes` (+$28) and `addressHi` (+$2C), then unimplemented space to +$7F and undefined padding to the 256-byte boundary [5]. The driver corpora name +$10..+$1C differently (`intr_sel`, `br_sel`, `wait_sel`, `xfer_mode`) [7], consistent with the shipping layout of the select registers at +$10/+$14/+$18. Nothing beyond +$18 is attested on Grand Central: no shipped software reads or writes any of it, and a driver that assumed a live `byteCount` or `data2Ptr` on these machines would be writing to undecoded space (§6.2).

### 2.8 Reset state

The only architected initial value is ChannelStatus = $00000000 [1] Table 322 p. 173; CommandPtrLo and the select registers start undefined, and ChannelControl has no stored state. The machines' firmware does not rely on any power-on latch: it explicitly resets every channel before use (§4.2), and the standard's own register-summary table marks the "value in register when OF passes control to OS" as undefined for every channel register [1] Table 322 p. 173.

---

## 3. Behaviour

### 3.1 The command descriptor

A channel program is an array of **command descriptors** in initiator-resident memory — "structured as linked arrays", allocated and filled by software before ownership passes to the engine [1] §15.1 p. 171. Each descriptor is **16 bytes, 16-byte aligned, little-endian** [1] §15.2 p. 171, requirement 15-2 p. 173, [5]:

| Offset | Word | Field |
|---|---|---|
| +$00 | Word 3 | operation: command word (bits 31–16), reqCount (bits 15–0) |
| +$04 | Word 2 | address |
| +$08 | Word 1 | cmdDep — command-dependent (branch target, immediate data) |
| +$0C | Word 0 | result: xferStatus (bits 31–16), resCount (bits 15–0) |

Software publishes a descriptor **operation-word-last**. Apple's own descriptor-building macros write the result, cmdDep and address words first and the operation word last, with a NOP between the stores — the operation word is the commit point, the only write that makes the descriptor live [5]. A channel that latched a descriptor on any other field would race with a correct driver; conversely, a driver that published the operation word before the payload words would let the engine fetch a half-built command.

### 3.2 The command word

Within the operation word's 16-bit command halfword [1] Tables 333, 343 pp. 182, 187:

```
bits 31–28  cmd   the command (see below)
bit  27     —     reserved (write 0)
bits 26–24  key   which device access port the transfer uses
bits 23–22  —     reserved (write 0)
bits 21–20  i     interrupt action on completion
bits 19–18  b     branch action on completion
bits 17–16  w     wait action on completion
bits 15–0   reqCount  bytes to transfer
```

**The command set** [1] Table 334 p. 183:

| cmd | Name | Description |
|---|---|---|
| 0 | OUTPUT_MORE | transfer more memory to stream |
| 1 | OUTPUT_LAST | transfer last memory to stream |
| 2 | INPUT_MORE | transfer more stream to memory |
| 3 | INPUT_LAST | transfer last stream to memory |
| 4 | STORE_QUAD | store immediate 4-byte value |
| 5 | LOAD_QUAD | load immediate 4-byte value |
| 6 | NOP | no data transfer |
| 7 | STOP | suspend command processing |
| 8–15 | — | reserved |

All eight are mandatory [1] §15.7 p. 182. There is **no JUMP command and no WAIT command** in the shipping architecture: a jump is a NOP (or any INPUT/OUTPUT) with `b` = BR_ALWAYS and the target in cmdDep; a wait is the `w` modifier on any command (§3.6). Apple's 1994 interface header defines a different, pre-release encoding — a command nibble at a different position, distinct JUMP/WAIT/STOP opcodes, and a `test` modifier field — that shipping silicon does not implement (§5).

**The key field** selects the access port [1] Table 335 p. 183:

| key | Name | Description | Usage |
|---|---|---|---|
| 0 | KEY_STREAM0 | default device stream | data |
| 1–3 | KEY_STREAM1..3 | device-dependent streams | control and status |
| 4 | — | reserved | — |
| 5 | KEY_REGS | channel-state register space | indirect register access |
| 6 | KEY_SYSTEM | system memory-mapped space | two-address transfers |
| 7 | KEY_DEVICE | device memory-mapped space | two-address transfers |

With INPUT and OUTPUT, the key selects alternate device streams (data versus status); with LOAD_QUAD and STORE_QUAD it selects which address space the immediate data moves through. A key of KEY_STREAM0..3 combined with LOAD_QUAD or STORE_QUAD is illegal [1] p. 184. The two-address forms — OUTPUT with KEY_SYSTEM/KEY_DEVICE/KEY_REGS moving SysMem(address) to SysMem/Device/ChannelRegs(Data2Ptr), and the INPUT mirror — exist in the architecture [1] Table 336 p. 184, and the Data2Ptr register they require is one of the optional registers not attested on Grand Central (§2.7).

**The i, b, w modifiers** [1] Tables 338, 340, 342 pp. 185–186 all share one encoding, tested against the corresponding condition bit of §2.6:

| Value | i (interrupt) | b (branch) | w (wait) |
|---|---|---|---|
| 0 | never | never | never |
| 1 | if condition set | if condition set | if condition set |
| 2 | if condition clear | if condition clear | if condition clear |
| 3 | always | always | always |

For a channel with no hardwired interrupt signal the `i` field is ignored [1] p. 185 — on these machines every channel has one (§3.8).

### 3.3 The eight commands

**OUTPUT_MORE / OUTPUT_LAST / INPUT_MORE / INPUT_LAST.** The data movers, between system memory and the device stream. reqCount is the byte count, address the system-memory buffer, cmdDep the 32-bit conditional branch target [1] Table 343 p. 187. Data chaining is the MORE/LAST distinction: "The MORE commands indicate that the current buffer is not expected to complete a logical record (such as a network packet). The LAST commands indicate that the buffer is expected to complete a logical record" [1] §15.7.2 p. 187. A conventional transfer to an unaddressed device stream uses KEY_STREAM0; the other stream identifiers reach device-side control and status [1] p. 187.

**STORE_QUAD / LOAD_QUAD.** Move a 32-bit immediate between cmdDep and the address (with key selecting the space). The `b` field is reserved and must be written 0; only keys KEY_REGS, KEY_SYSTEM and KEY_DEVICE are legal. The only valid reqCount values are 1, 2 and 4, only aligned transfers are supported, and illegal counts are mapped to legal ones (0b00x → 1, 0b01x → 2, 0b1xx → 4); when the count is under 4 the least significant bytes of data32 are transferred [1] §§15.7.3–15.7.4 pp. 187–189. STORE_QUAD is also the architecture's device-to-device signaling primitive: one channel program stores into another channel's register space or status bits — the standard's flow-control example uses a STORE_QUAD in channel A's program to set channel B's s0 bit, on which B's commands wait [1] §15.10.3 pp. 193–194.

**NOP.** "Performs no data transfers. However, it can use standard mechanisms to specify that interrupt, branch, or wait actions be performed" [1] §15.7.5 p. 189. A NOP with `b` = BR_ALWAYS and the target in cmdDep is the canonical jump; a bare NOP is the canonical placeholder for overwriting a STOP during ring extension (§4.5).

**STOP.** "The only effect of the STOP command is that the channel goes idle and clears the ChannelStatus.active bit" [1] §15.7.6 p. 190. Every field but the command nibble is reserved. STOP does not advance CommandPtr — the channel parks *on* the STOP descriptor, which is what makes the append idiom work.

### 3.4 The channel state machine

A channel is idle until software initializes CommandPtrLo and sets run. The engine then fetches the descriptor at CommandPtr, executes it, and continues "until a STOP command is executed" [1] §15.5.2 p. 179. Per command, the fetch-execute cycle is [1] §§15.5.2, 15.4.3:

1. Fetch the 16-byte descriptor at CommandPtr (clearing wake if it was set).
2. Execute the data movement (or none, for NOP/STOP), at the pace the device cell allows. A device that cannot supply or take bytes now simply holds the channel mid-command — this is the `w`-less half of flow control, and a paused-mid-command channel keeps active set (run clear or pause set is what drops it).
3. If the command's `w` test succeeds, suspend **before** the completion sequence — "if a wait action is invoked, it occurs before the normal command completion sequence (branch determination, status update, interrupt generation, or next command fetch)" [1] p. 186.
4. Write back the result word (§3.5).
5. Evaluate the `b` test: CommandPtr is "either incremented by 16, replaced by the command-entry's branchAddress value, or left unchanged (for the STOP command)" [1] §15.5.2 p. 180; record the outcome in bt.
6. Evaluate the `i` test and raise the channel's interrupt line if it fires (§3.8).
7. Fetch the next command — or go idle, if the command was STOP.

Software-side control of the state machine: setting run starts it; clearing run aborts it (terminating transfers, writing back status, interrupting if `i` requested it, possibly losing buffered data); setting pause suspends it at a boundary and drops active; setting flush drains a half-filled INPUT command to memory and reports partial status; setting wake refetches after an idle STOP; dead is hardware's one-way halt (§2.3) [1] §15.4.3 pp. 174–175.

The command list itself is free-form: "to loop on itself (a circular queue), to link individual data-transfer operations (linked lists), or to link groups of data-transfer operations (a hybrid approach...). The detailed structure of command lists is dependent on device driver software conventions and is not specified by the DBDMA architecture" [1] §15.5.2 p. 179. Multiple command lists per channel are a driver convention; multiple *initiators* appending concurrently are explicitly unsupported — "multiple initiators are expected to serialize their command list updates through other shared-memory semaphores that are beyond the scope of this specification" [1] §15.5 p. 178.

### 3.5 Status write-back

On completion the engine writes the descriptor's **result word**: the low 16 bits are **resCount**, the residual byte count — "normally 0, but may be more when the device prematurely terminates the data transfer" [1] p. 187 — and the high 16 bits are **xferStatus**, an image of ChannelStatus at completion. Two rules bind the write-back [1] p. 187:

- "When Command.xferStatus is written to memory, the bit corresponding to ChannelStatus.active is always set to 1. This serves as an indication that the corresponding command has been executed, and both the xferStatus and resCount fields have been updated. To make use of this feature, host software should clear the active bit in the Command.xferStatus field to 0 when the command descriptor is initialized."
- "The Command.resCount and Command.xferStatus fields shall be updated in an indivisible operation."

Software therefore builds each descriptor with the active bit of the (cleared) result word at zero and polls or interrupts on its reappearance — the done-flag idiom every driver on the platform uses. The flush path gives partial status mid-command: a forced or device-initiated flush updates xferStatus and resCount for the bytes actually moved, with "1 in the flush bit of the xferStatus field" marking the update partial [1] §15.4.3 p. 174. Devices may provide intermediate status autonomously by setting the flush bit themselves — useful "in cases such as terminal input, where the host needs to respond quickly to input data but would like to avoid interrupts on every byte"; the capability is optional and the trigger conditions device-dependent [1] p. 187.

The residual count is how short transfers are detected: both SCSI cells end a data phase early when the target disconnects, and the driver learns the true length from resCount [7], [8], [9] (§4.6).

### 3.6 Conditional actions

At each command completion the engine evaluates three masked-equality conditions against the device-status byte s7..s0 and the three select registers (§2.6), then applies the command's `i`, `b`, `w` fields to the results [1] §§15.4.5–15.4.7 pp. 176–177, §§15.6.4, 15.7.1 pp. 182–186. The wait test runs first in sequence (§3.4); the branch outcome lands in bt; the interrupt outcome drives the channel's line. Because the s-bits can be driven by the device cell, by software through ChannelControl, or by *another channel's* STORE_QUAD, the same primitive gives device-signalled flow control, software polling-free eventing, and inter-channel synchronization [1] §§15.6.3–15.6.4, 15.10.3.

### 3.7 Error handling

The engine sees two classes of failure on its system-bus side [1] §15.6.2 p. 181:

- **Unrecoverable** — parity and addressing errors on system-memory accesses. The engine sets **dead** on the affected channel, terminates the current command, writes back status, and stops. Recovery is entirely software's: "Software must explicitly reset the ChannelStatus.run bit to 0 and later set it back to 1 to return to an operational state."
- **Recoverable** — "A busy-retry error is treated as a recoverable bus error, and the bus action is retried until it finishes successfully or the DBDMA controller's command processing is terminated."

If a hardwired interrupt signal is implemented, dead generates an unconditional interrupt [1] §15.4.3 p. 175 — on these machines, the channel's own Grand Central interrupt number (§3.8).

### 3.8 Interrupts

The architecture's interrupt budget is one per I/O operation, raised per-command by the `i` field against the InterruptSelect condition [1] §§15.2, 15.7.1 pp. 171, 184–185. The CHRP requirement for PCI devices is a single system interrupt each, "shared with the other interrupt requirements of the device as described in the device chapters" [1] requirement 15-3 p. 191; the CHRP MESH device, for instance, wire-ORs its chip interrupt and its DBDMA interrupt into one line [1] requirement 12-4 p. 148. The TNT machines do it differently and more finely: Grand Central collects each channel as its own interrupt **number**, identical to the channel number (§1.4), alongside a separate interrupt number for each device cell — the firmware tree publishes both per device, so MESH's node carries `AAPL,interrupts` {13, 10} [6]. Every source is independently maskable in Grand Central's interrupt block at +$20..+$2C of the chip (see [grand-central.md](grand-central.md)).

Observed on the shipped machines [6]:

- All eleven channel interrupts classify into emulated 68k interrupt level 4, the same level as the ESCC channels; the devices' own chip interrupts classify lower (levels 2 or 3). A DMA completion therefore preempts device-housekeeping work but not the serial data path.
- A channel completion behaves as a **level**, held until acknowledged — not a one-shot edge. Mac OS 8.1's Sound Manager enables mask bit 8 (audio-out DMA), starts a two-buffer ring with an interrupt on each buffer, and acknowledges by writing bit 8 to Grand Central's InterruptClear both before starting the channel and again from its completion handler — an acknowledge of a standing request. MkLinux instead reads the interrupt block's Events register and writes the same bits back to Clear, which also drops the level.
- Open Firmware does not use the audio channel's interrupt at all: it polls the channel to completion (§4.7).

The architecture also defines an **optional event channel** — a shared channel whose command list is INPUT_LAST commands of 4 bytes each, executed on demand to report low-frequency asynchronous events (modem signal changes, media removal) from any affiliated device, each returning a 16-bit eventSource and 16-bit eventType packet [1] §§15.5.1, 15.8 pp. 179, 190. Nothing on these machines is observed to use one, and the standard allows it to be omitted when "the interrupt mechanism and device status registers are sufficient" [1] §15.5.1 p. 179 (§6.8).

### 3.9 Endianness, coherency and memory placement

DBDMA on these machines is **little-endian without exception**: "Since the devices defined herein that use DBDMA are PCI devices, only little endian addressing for DBDMA is supported... For such quadlets, the data byte with the smallest address is the least significant byte" [1] §15.3 p. 172. The 7500/8500 platform straddles two conventions — processor-bus big-endian, PCI little-endian, with the Bandit bridge performing byte swapping between them [2] p. 18 — and Grand Central sits on the PCI side: every 32-bit register and every in-memory descriptor field is little-endian, so PowerPC software reaches the whole channel file with `lwbrx`/`stwbrx` [6]. The standard's requirement 15-1 also binds all reserved fields: zero on write, ignored on read, "whether the reserved field is accessed by the DBDMA hardware or by device driver software" [1] p. 173.

Descriptor and status traffic is coherent with the processor caches on this platform: the first Bandit bridge's coherency mode bit (PCI configuration offset $50, bit $40) is set by every observed system start, making DBDMA descriptor writes, data transfers and result write-backs visible to the CPU without an explicit cache flush [8], [9]. That is load-bearing for the done-flag idiom of §3.5 — software polls a descriptor field the engine just wrote and expects to see it.

Descriptor fetches and data transfers go through the machine's **full physical map**, not a RAM-only path: the firmware's own audio program keeps both its descriptors and its sample data in read-only memory (§4.7), which the engine reads like any other physical address.

---

## 4. Programming model

### 4.1 Enumeration: the device tree, not a PCI probe

Grand Central is not enumerated through PCI configuration space. The ROM discloses no PCI vendor/device ID pair for the chip — the firmware image carries $0001106B (Bandit) and $0003106B (Chaos) but no Grand Central header — and every system, from Open Firmware to Mac OS to Linux and NetBSD, attaches the device from its Open Firmware node, `gc` with `device_type "dbdma"` [6], [7], [8], [9]. Each device node under `gc` publishes its channel address in its `reg` property — offsets relative to the Grand Central base $F3000000 — and its interrupt numbers in `AAPL,interrupts` [6]:

| Node | I/O reg | Channel reg entries | AAPL,interrupts |
|---|---|---|---|
| `53c94` | $10000 | $8000 (size $200) | 12 |
| `mace` | $11000 | $8200, $8300 | 14 |
| `escc/ch-b` | $13000 | $8600, $8700 | 16 |
| `escc/ch-a` | $13020 | $8400, $8500 | 15 |
| `awacs` | $14000 | $8800, $8900 | 17 |
| `swim3` | $15000 | $8100 | 19 |
| `mesh` | $18000 | $8A00 | 13, 10 |

The firmware's own construction code emits the entries with named constants — `c94-iooffset` ($10000) and `c94-dmaoffset` ($8000) for the external SCSI cell [6]. A device with a channel therefore shows *two or three* reg entries: its device-register aperture plus one per channel. Note the 53C94's channel entry is published with size $200, twice the 256-byte channel block — whether the second half decodes anything is unattested (§6.7). The 7200's tree omits the `mesh` node entirely (the machine has no MESH cell) [6].

### 4.2 Bring-up

The boot sequence that touches the channels, in order [6], [8]:

1. The first Bandit bridge is configured, including the coherency bit (§3.9).
2. Grand Central's base address register is programmed so its window lands at $F3000000.
3. The interrupt block is initialized — mask cleared, a $FFFFFFFF write to InterruptClear flushing stale events, mask cleared again (MkLinux's init is the reference spelling of this) [8].
4. **Every channel is reset** with the clear-all mask/value write and polled until run drops:
   ```
   ChannelControl = (ACTIVE|DEAD|WAKE|FLUSH|PAUSE|RUN) << 16
   while (ChannelStatus & RUN) ;
   ```
   This is the canonical reset sequence of the driver corpora [7], exercised by the firmware on the audio channel before it plays the boot beep [6].
5. Devices are probed in device-tree order; each receives its `reg` and `AAPL,interrupts`.
6. The interrupt mask is set per enabled device.

Nothing else in bring-up programs a channel: no select register, no extended register, is written by any observed system start [6], [7], [8], [9].

### 4.3 Start, stop and reset

The three canonical sequences, as every driver in the corpora writes them [7]:

**Start a program:**
```
CommandPtrLo = physical address of the first descriptor;
ChannelControl = (RUN << 16) | RUN;
```
The pointer write is only guaranteed to stick while run and active are both clear (§2.4); the run-set is the last write.

**Stop a channel:**
```
ChannelControl = (RUN | FLUSH) << 16;      /* clear RUN and FLUSH */
while (ChannelStatus & (ACTIVE | FLUSH)) ;
```
The poll covers both bits: active until the in-flight command unwinds, flush until any buffered input data has been written back with partial status.

**Reset a channel:**
```
ChannelControl = (ACTIVE|DEAD|WAKE|FLUSH|PAUSE|RUN) << 16;
while (ChannelStatus & RUN) ;
```
Clearing run is also the defined exit from dead (§3.7).

Drivers also read CommandPtrLo on a stopped channel to find where the program halted [7] — and after a STOP it reads as the STOP descriptor's own address, because STOP does not advance the pointer (§3.3).

### 4.4 Building a program

The per-descriptor contract, assembled from the standard and Apple's macros [1], [5]:

1. Allocate the array 16-byte aligned; every pointer field is 16-byte aligned with its low bits written zero (requirement 15-2).
2. Write the result word with the active bit clear — this is the done flag the engine will set (§3.5).
3. Write address and cmdDep.
4. Write the operation word last — the commit point (§3.1).
5. Terminate the list with STOP.

reqCount is 16 bits, so no single data command moves more than 65 535 bytes; larger transfers chain MORE commands. The MESH driver chunks its data phases to at most $FFF0 bytes per command [7], [8], [9] — a conservative bound below the architectural maximum that also fits the cell's 16-bit transfer-count register pair.

### 4.5 Appending commands and building rings

The architecture's dynamic-extension mechanism, verbatim from the standard [1] §15.7.6 p. 190:

1. Append the new commands to the existing channel program in memory.
2. Overwrite the STOP command with a NOP command (or the first new channel command).
3. Set the wake bit in the ChannelControl register to 1. "This lets the channel process the new commands whether or not it had already completed the old commands and gone idle."

Because STOP parks the channel on the STOP descriptor itself (§3.3), the wake refetches exactly the overwritten slot. The standard's Ethernet transmit example builds a circular queue this way: packet commands, STOP; on the next request, STOP becomes a NOP, the new commands follow, a new STOP ends the group, and when the allocated blocks run out "a branch back to the first descriptor can be executed, and new command groups can overwrite those which have been executed and released by hardware. (Hardware releases descriptors by writing to the Command.xferStatus field.)" — the branch being either a standalone NOP-with-branch or embedded in the last data command [1] §15.10.1 pp. 191–192. Its Ethernet receive example is the ring driver pattern in full: a STORE_QUAD that initializes BranchSelect to test the end-of-frame bit, then alternating INPUT_MORE (branch if the frame continues) and INPUT_LAST status-fetch commands, a STOP that always marks the boundary between controller-owned and host-owned buffers, and the host advancing the STOP as it returns buffers [1] §15.10.2 pp. 192–193.

### 4.6 How each device drives its channels

**53C94 external SCSI (channel 0).** The external bus's cell, in the Curio IC, at Grand Central +$10000; the tree publishes a 25 MHz `clock-frequency` for it [6]. Data phases run through channel 0 with ordinary OUTPUT/INPUT lists; the cell's non-data phases (command, status, messages) go through its own FIFO, not the DMA engine [7], [9].

**MESH internal SCSI (channel 10).** The fastest device on the engine, at up to 10 MB/s [2] p. 19. The driver's data-phase sequence: build the descriptor list; write CommandPtrLo, then `ChannelControl = (RUN<<16)|RUN` on channel 10; write the cell's 16-bit transfer count — chunked to at most $FFF0; then issue the cell's data-phase sequence command with its DMA-mode modifier set [7], [8], [9]. Short transfers — the target disconnecting early — are detected from the descriptor's resCount write-back [7]. Initialization stops the channel *before* resetting the cell: the clear-mask write of §4.3 is the first operation of the chip's init sequence [7].

**MACE Ethernet (channels 2 and 3).** One channel per direction. Transmit is OUTPUT_MORE/OUTPUT_LAST per frame; receive is a ring of INPUT_MORE/INPUT_LAST descriptors that branches back to its head, with each frame's true length taken from the LAST descriptor's resCount [7], [9]. The ROM's own MACE driver literal pool carries the channel addresses $F3008200/$F3008300 alongside the DBDMA control-word constants $80008000, $80000000 and $70000000 (STOP) — the mask/value convention and the STOP terminator in the shipping driver's own constants [6].

**ESCC serial (channels 4–7).** Two channels per Zilog ESCC channel — A gets 4/5, B gets 6/7 — giving "high-speed, full-duplex operation" [1] §9 p. 115. The ESCC is the one device whose DBDMA status-bit assignment is publicly documented: on the transmit channels, s0 is the wait bit (externally controlled) and s5 is the LocalTalk protocol controller's end-of-packet detect bit; on the receive channels only s0 is implemented; and only those bits are implemented in that device's select registers [1] §§9.9–9.10 pp. 127–128. The LTPC uses its detect bit to terminate a LocalTalk transmit: after about 15 mark bits it tristates the line and "set[s] the corresponding detect bit in the DBDMA S-bits" [1] §9.8 pp. 125–126 — the concrete example of a device cell driving a command's branch or wait condition.

**SWIM III floppy (channel 1).** The floppy cell inside Grand Central [2] p. 18. Its driver builds a descriptor list, writes CommandPtrLo, starts with `(RUN<<16)|RUN`, and terminates the list with a STOP command — nothing floppy-specific [7]. The ROM's driver literal pool confirms the channel's three load-bearing registers ($F3008100/$04/$0C) [6]. The Developer Note records the system-level payoff of moving this device onto descriptor DMA: "The SWIM III controller supports DMA data transfers and does not require disabling of interrupts during floppy disk accesses" [2] p. 18 — on the pre-PCI machines a floppy access was an interrupt-locking event.

**AWACS audio (channels 8 and 9).** Output and input. Playback is a ring of buffers with an interrupt on each buffer's LAST command; the codec's own byte-swap register selects the sample data's endianness independently of the register endianness (§3.9). Observed under Mac OS 8.1: the Sound Manager runs a two-buffer output ring, acknowledging the channel-8 interrupt through Grand Central's InterruptClear on every completion (§3.8).

**Plan B video input (two channels in the Plan B IC).** The write channel moves captured pixels from the 7196 DESC's pixel FIFO into memory or the frame buffer; the read channel pulls the 1-bit-per-pixel clip mask from main memory, which is what lets play-through video be occluded by windows and menus [2] p. 21. Where these two channels are addressed is not published (§6.4).

### 4.7 The firmware boot beep

The machines' Open Firmware exercises the engine before any operating system exists. On every cold start, during memory sizing, it plays the startup beep through channel 8 [6]:

- The **descriptor program lives in the boot ROM**, at physical $FFE00060–$FFE0009F, and the sample data lives in ROM too, at $FFE200A0, $FFE280A0 and $FFE300A0 — four descriptors, three of them data:

| Address | Operation word | Decoded command | Address field |
|---|---|---|---|
| $FFE00060 | $00008000 | OUTPUT_MORE, reqCount $8000 | $FFE200A0 |
| $FFE00070 | $00008000 | OUTPUT_MORE, reqCount $8000 | $FFE280A0 |
| $FFE00080 | $10002B60 | OUTPUT_LAST, reqCount $2B60 | $FFE300A0 |
| $FFE00090 | $70000000 | STOP | — |

  76 640 sample bytes, then STOP. The opcodes are only meaningful under the shipping command encoding of §3.2 — under the pre-release encoding of Apple's 1994 header the same words are garbage (§5).

- The start sequence is a single write after loading CommandPtrLo: `ChannelControl = $F0008000` — mask RUN|PAUSE|FLUSH|WAKE, value RUN — set run while explicitly clearing the other three (§2.2) [6].
- Open Firmware **polls** the channel for completion; a channel that never completes hangs the boot inside firmware [6].
- After the STOP, the channel reads back `ChannelStatus = $00008000` (run set, active clear) with `CommandPtrLo = $FFE00090` — parked on the STOP descriptor, exactly as the standard's no-increment rule predicts (§3.3) [6].

This one program pins the little-endian descriptor fetch, the OUTPUT data path, descriptor-and-data fetch through the physical map including ROM, and the STOP-parking rule, all against the shipping ROM [6].

---

## 5. Quirks & errata

- **Apple's 1994 interface header does not describe shipping silicon.** The header in Apple's system-software sources (February 1994 snapshot) encodes the command nibble at a different position, defines distinct JUMP/WAIT/STOP opcodes and a `test` modifier field, and names the status bits one position shifted — "Halted" at bit 13, "Dead" at 12, "Active" at 11, "Paused" at 10, where the shipping architecture (and the CHRP standard, and every driver written for these machines) has flush, wake, dead and active [5], [1], [7]. The header is a pre-release architecture revision. It remains authoritative for the register-file layout, the mask/value ChannelControl convention, and the descriptor commit rule — and nothing else.
- **There is no JUMP and no WAIT command.** A jump is a NOP (or data command) with `b` = BR_ALWAYS; a wait is the `w` modifier. Driver code written against the header's JUMP/WAIT opcodes will not run.
- **`0x0100` in a status image is BT, not "done".** The header's `kTStatDone`-style constants belong to the pre-release revision; in the shipping architecture bit 8 of an xferStatus image means the branch was taken. The done flag is the **active** bit (bit 10), which the engine always sets on write-back (§3.5).
- **ChannelStatus writes are ignored; ChannelControl reads as zero.** Control goes only through the mask/value port; observation only through ChannelStatus. A write to +$04 is silently dropped [1] §15.4.3.
- **CommandPtrLo writes require run and active both clear.** The architecture says so [1] §15.4.4 — yet the shipped ROM's native sound driver writes channel 8's CommandPtrLo while the channel is parked on the firmware's STOP with run still set and active clear, and never restarts that program [6]. Whether shipping silicon accepts a parked-pointer write is unpinned (§6.6).
- **STOP parks on itself.** CommandPtr is left pointing at the STOP descriptor, run stays set, active drops. Every ring-extension idiom depends on this; a model or driver that advances past STOP breaks the overwrite-then-wake sequence (§4.5).
- **The done flag must be pre-cleared by software.** The engine unconditionally sets the active bit in xferStatus on write-back; the descriptor is not "done" unless software zeroed it first [1] p. 187.
- **resCount and xferStatus update indivisibly.** A driver that can see one without the other is racing the engine [1] p. 187.
- **reqCount is 16 bits.** No data command moves more than 65 535 bytes; the MESH drivers chunk to $FFF0 (§4.4).
- **The low status byte is the device's.** s7..s0 meanings are per-device; only the ESCC's are publicly documented (s0 wait, s5 LTPC detect). Assuming MESH's or the 53C94's s-bits mean anything specific is unsupported (§6.3).
- **Flush only means something on INPUT.** It exists to drain device-supplied bytes from channel buffers to memory; an OUTPUT channel has nothing buffered to flush [1] §15.4.3.
- **Descriptors and data can live in ROM.** The firmware's beep program keeps both in the boot ROM; the engine's fetch path covers the whole physical map (§4.7).
- **Everything is little-endian behind a big-endian CPU.** Registers, descriptors, results — all LE, all reached with byte-reversed loads and stores, with no swapper in the path [1] §15.3, [6] (§3.9).
- **Channel number equals interrupt number.** Eleven channels, interrupt numbers 0–10, all at 68k IPL 4; each device additionally carries its own chip interrupt at a lower level (§1.4, §3.8).
- **Channel completions are levels, acknowledged by a Clear write.** Software that treats a completion as a one-shot edge loses every completion after the first — the observed Mac OS acknowledge idiom writes the channel's bit to Grand Central's InterruptClear before starting and again per completion (§3.8).
- **The 53C94's channel reg entry is size $200.** Twice the channel block; whether the extra 256 bytes decode anything is unattested (§6.7).
- **The 7200 has no channel 10.** The machine has no MESH cell, its tree has no `mesh` node, and the external 53C94 is the only SCSI controller [6].
- **Reserved means reserved, both ways.** Requirement 15-1: every reserved field is written zero and ignored on read, by hardware and by software alike [1] p. 173.
- **Grand Central is not a PCI device.** No vendor/device ID pair is published for it; enumeration is by firmware tree, and a PCI-bus-walk driver will never find the DMA engine (§4.1).

---

## 6. Open questions

1. **Whether Grand Central implements the three condition-select registers.** They are optional in the architecture [1] §15.4, and no shipped driver on these machines programs InterruptSelect, BranchSelect or WaitSelect — a power-on zero register makes every masked-equality condition read true, which is behaviorally identical to `*_ALWAYS` for every driver in the observed corpus, so the two possibilities are indistinguishable from software seen so far. The ESCC chapters document a device that narrows them to s0/s5 [1] §9.10; whether Grand Central's cells narrow or hardwire them is unattested.
2. **The optional extended registers.** Whether Grand Central decodes CommandPtrHi (+$08), anything at +$1C, the Data2Ptr pair (required for the architecture's two-address KEY_SYSTEM/KEY_DEVICE transfers), byteCount, transferModes or addressHi — Apple's header maps them all into the 256-byte block and marks them implementation-optional [5]; no shipped software touches any of them.
3. **The s-bit meanings of the SCSI, floppy, Ethernet and audio cells.** MESH and the 53C94 clearly drive device-status bits that a descriptor can branch or wait on — the drivers' flow control depends on the wait mechanism — but the bit assignments are documented in no public source and no observed driver; they would have to come from Apple's SCSI HAL in the ROM.
4. **Plan B's channel addresses and register map.** The 7500/8500 Developer Note establishes that the IC contains a DBDMA write channel and a read channel [2] p. 21, but not where its channel register blocks live or how its engine differs, if at all, from Grand Central's.
5. **The asynchronous event channel.** The architecture defines an optional shared event channel for reporting low-frequency events from any device [1] §§15.5.1, 15.8. Nothing on these machines is observed to use one; whether Grand Central implements one at all is unknown.
6. **The parked-pointer write.** The shipped ROM's sound driver writes CommandPtrLo to a channel parked on a STOP with run set (§5), against the register's "both bits zero" write condition [1] §15.4.4. Whether silicon accepts the write, ignores it, or behaves indeterminately is unpinned; the driver never starts that program, so no observed boot distinguishes the cases.
7. **The 53C94's $200-size channel reg entry.** The firmware publishes channel 0 with size $200 rather than $100 [6]. Whether the upper 256 bytes of that entry decode anything (a second aperture, or padding in the tree) is unattested.
8. **Command fetch granularity and buffering.** The standard does not state how many descriptors the engine prefetches or buffers; observable software behavior (STOP parking, wake refetch semantics) is consistent with a strict fetch-execute-writeback machine, but the microarchitecture is unknown.
9. **Whether any TNT driver ever uses the conditional `i`/`b`/`w` fields beyond ALWAYS.** The CHRP examples show conditional branch and interrupt patterns (end-of-frame tests, flow control) [1] §15.10, and the ESCC's documented s-bits exist to be tested [1] §9.9, but the shipped Mac OS, MkLinux, Linux and NetBSD drivers observed for these machines use only the unconditional settings. Which ROM-native drivers (if any) exercise the conditional machinery is unexamined.

---

## References

1. Apple Computer, Inc., International Business Machines Corporation, and Motorola, Inc., *PowerPC Microprocessor Common Hardware Reference Platform: I/O Device Reference*, Version 1.0, May 1996 — Chapter 15 "Descriptor-Based DMA" pp. 171–194: §15.1 Overview p. 171, §15.2 DBDMA Characteristics p. 171, §15.3 Conventions p. 172, §15.4 Controller Registers pp. 173–177 (Table 321 channel registers p. 173, Table 322 register summary p. 173, Table 324 ChannelControl p. 174, Table 326 ChannelStatus pp. 174–175, §15.4.4 CommandPtrLo p. 176, Tables 329–332 select registers pp. 176–177), §15.5 Summary of DBDMA Operations pp. 178–180 (§15.5.1 Multiplexed Channels p. 179, §15.5.2 Command-List Structure p. 179), §15.6 Design Model pp. 180–182 (§15.6.2 System Bus Errors p. 181, §15.6.3 Device Status p. 182, §15.6.4 Conditional Actions p. 182), §15.7 Commands pp. 182–190 (Table 334 cmd values p. 183, Table 335 key values p. 183, Table 336 data-transfer operations p. 184, Tables 337–342 condition algorithms pp. 184–186, §15.7.2 INPUT and OUTPUT p. 187, §15.7.3 STORE_QUAD pp. 187–188, §15.7.4 LOAD_QUAD pp. 188–189, §15.7.5 NOP p. 189, §15.7.6 STOP and command appending p. 190), §15.8 Asynchronous Event Packet Formats p. 190, §15.9 Hardwired Interrupts p. 191, §15.10 Examples pp. 191–194; Chapter 9 "ESCC": §9 pp. 115 ff., §9.8 LTPC pp. 125–126, §9.9 ESCC DBDMA Channel Status Register Usage pp. 127–128 (Tables 250–251), §9.10 Conditional Interrupt, Branch and Wait Generation p. 128; Chapter 12 "SCSI": §12.2 p. 147, requirement 12-4 p. 148, Table 283 p. 148, §12.6 DBDMA Registers p. 158.
2. Apple Computer, Inc., *Developer Note: Power Macintosh 7500 and Power Macintosh 8500 Computers*, Developer Press, 1995 — Chapter 2 "Architecture": §"Bandit PCI Bridge IC" and the PCI arbitration list p. 18, §"Big-Endian and Little-Endian Bus Addressing" p. 18, §"Grand Central I/O Subsystem IC" p. 18 (the DMA controller; SWIM III DMA and the no-interrupt-disable note), §"Curio I/O Controller IC" p. 19, §"MESH High-Speed SCSI Interface" p. 19 (10 MB/s internal, 5 MB/s external), §"Video Input" p. 21 (Plan B's two DBDMA channels and the clip-mask read channel); Glossary ("DBDMA", "Grand Central", "descriptor-based direct memory access").
3. Apple Computer, Inc., *Developer Note: Power Macintosh 9500 Computers*, Developer Press, 1995 — §"Grand Central I/O System IC" p. 11 ("It provides DBDMA support for all I/O transfers, including transfers through its internal I/O controllers as well as transfers through the Curio IC for other I/O devices"; PCI arbitration priority); abbreviations p. 13; Glossary p. 76; Index p. 79.
4. Apple Computer, Inc., *Designing PCI Cards and Drivers for Power Macintosh Computers*, Developer Press — Chapter "Descriptor-Based DMA" (the published Apple treatment of the DBDMA programming interface; Part One "The PCI Bus" is cited by [2] p. 18 for the byte-order bridge behavior; the 7500/8500 Developer Note p. 10 records the book as in preparation with preliminary drafts available from Apple Developer Support).
5. Apple Computer, Inc., `DBDMA.h`, DBDMA interface definitions from the SuperMario system-software sources, snapshot dated 9 February 1994 — the channel register layout (including the implementation-optional `commandPtrHi`, `dataPtrHi/Lo`, `byteCount`, `data2PtrHi/Lo`, `transferModes`, `addressHi` fields), the mask/value ChannelControl constants (kdbdmaSetRun $80008000, kdbdmaClrAll $F00F0000 and kin), the descriptor-building macros (result/data32/address first, operation word last, with a NOP between — the commit rule), and the pre-release command and status encoding (cmd mask $0F00, JUMP/WAIT/STOP opcodes 6/7/8, kdbdmaHalted/Dead/Active/Paused at bits 13/12/11/10, kTStat* constants) that shipping silicon does not implement. <!-- lint-allow: SuperMario -->
6. Power Macintosh 7200/7500/8500/9500 boot ROM (4 MB image: Open Firmware, NanoKernel, 68k Toolbox ROM with native-driver resources) — the Open Firmware device tree (`gc` node, `device_type "dbdma"`, `model "AAPL,343S1125"`, per-device `reg`/`AAPL,interrupts` properties, the `c94-iooffset`/`c94-dmaoffset` constants, the 7200's missing `mesh` node, absence of a Grand Central PCI vendor/device pair); Open Firmware's channel-8 boot-beep program at physical $FFE00060–$FFE0009F with sample data at $FFE200A0–$FFE32C5F, the start write $F0008000, the polled completion, and the parked end state (ChannelStatus $00008000, CommandPtrLo $FFE00090); the SWIM III driver literal pool ($F3008100/$F3008104/$F300810C); the MACE driver literal pool ($F3008200/$F3008300, $80008000/$80000000/$70000000); the NanoKernel's external-interrupt classification (all DBDMA channel numbers 0–10 at emulated 68k IPL 4); the native sound driver's parked-pointer write to channel 8; observed Mac OS 8.1 Sound Manager behavior on the audio-output ring and its InterruptClear acknowledges.
7. P. Mackerras et al., Linux kernel PowerPC sources, 1996 onward — `arch/powerpc/include/asm/dbdma.h` (register offsets, status bits RUN/PAUSE/FLUSH/WAKE/DEAD/ACTIVE/BT, command encoding, the canonical stop/reset/start sequences) and the Power Macintosh drivers written against it: `drivers/scsi/mesh.c` and `mesh.h` (channel-10 data-phase order, the $FFF0 chunk bound, resCount short-transfer detection, the stop-channel-first init sequence), `drivers/scsi/mac53c94.c` (channel 0), `drivers/net/ethernet/apple/mace.c` (channels 2/3, the receive ring), `drivers/block/swim3.c` (channel 1, the STOP-terminated list), `drivers/tty/serial/pmac_zilog.c` (channels 4–7), `sound/ppc/pmac.c` and `awacs.h` (channels 8/9, the output ring), `arch/powerpc/platforms/powermac/pic.c`.
8. Apple Computer, Inc. and the Open Software Foundation, MkLinux (OSF Research Institute) POWERMAC platform sources — `powermac_pci.h` (Grand Central base $F3000000, size $20000, per-device DMA offsets including `PCI_DMA_BASE_PHYS +0x8000`), `interrupt_pci.c` (the 32-bit interrupt table with the DBDMA channel entries), `mesh.c` (DR3; the channel-10 data path and the chip reset sequence).
9. The NetBSD macppc port sources — `sys/arch/macppc/dev/dbdma.h` and `dbdma.c` (channel and command definitions), `meshreg.h` and `mesh.c`, `esp.c`, `zs.c`, `am79c950reg.h` and the MACE driver, `awacs.c`; all describing the same silicon.
10. Apple Computer, Inc., *Macintosh Technology in the Common Hardware Reference Platform*, Morgan Kaufmann Publishers, Inc., San Francisco, 1996, ISBN 1-55860-393-X — the fuller DBDMA architecture that [1] §15.1 p. 171 points to for features beyond the subset the CHRP devices require.
