# The Lisa segment MMU

**Contents:**

1. [Overview](#1-overview) — what the part is, the three physical address spaces, which machines carry it, what is MMU state and what is not
2. [Register file](#2-register-file) — the 1024-word descriptor RAM, SOR and SLR, descriptor addressing in special I/O space, read-back and reset state
3. [Behaviour](#3-behaviour) — translation, the limit check, space decode, context selection, START mode, bus-cycle timing, violations
4. [Programming model](#4-programming-model) — the Boot ROM bring-up, the Lisa OS System Mapping Table and its descriptor programmer, contexts as domains, the OS segment allocation, demand loading through the bus-error handler, MacWorks XL
5. [Quirks & errata](#5-quirks--errata)
6. [Open questions](#6-open-questions)

---

## 1. Overview

### 1.1 What the part is

The Lisa's memory-management unit is a **custom segment translator** built from a 1024 × 12-bit
descriptor RAM and supporting adders, latches and decode logic on the processor board
([1] §2.3.2 p. 2-5a, §4.4 pp. 4-18 – 4-19, Figure 4-13). It is *not* a Motorola PMMU: there are no
page tables in memory, no ATC, and no 4 KB pages. The MMU sits between the 68000's logical address
bus and the rest of the machine, and does exactly three things on every CPU bus cycle
([1] §2.3 pp. 2-3A – 2-5a):

1. **Relocation** — it adds a per-segment origin to the address, turning the 24-bit logical address
   into a 21-bit physical address.
2. **Access checking** — it compares the address against a per-segment limit and a per-segment
   access code, terminating the cycle with a bus error if either fails.
3. **Space classification** — it outputs flags that say which of the machine's three physical
   address spaces the access belongs to (main memory, I/O, or special I/O), so the address never
   overlaps between them: "There is, therefore, no physical overlap of memory space and no masking
   of any memory area" [1] pp. 1-6 – 2-1.

The MMU is clocked within the processor board's CPU/video interleaved timing: the 68000 runs at
5 MHz with an 800 ns memory access, and "the MMU must process [the logical address] and present the
result to the memory on the basis of the type of access" within the video half of each bus cycle
([1] §1 pp. 1-7 – 1-8; §4.2.2 p. 4-11a). Its internal sequencing is driven by two signals of its
own making, MALE and B/L/ (§3.6).

The descriptor RAM is the only writable state in the part. The MMU has no interrupt output, no
timer, and no software-visible status; everything the software sees is either a descriptor value
or a translation result. The context and START latches (§2.6, §3.4) live in the processor board's
control register, not inside the MMU proper, but they are part of the MMU's programming surface and
are documented here ([1] §4.7.2 p. 4-36; [lisa.md](lisa.md) §6.1).

### 1.2 The three physical address spaces

The Lisa has three physical address spaces, each a 2 Mbyte block ([1] §4.4 p. 4-19):

| Space | SLR access code | Contents |
| --- | --- | --- |
| **Main memory** | `%01xx` | RAM, up to 2 Mbytes; also the video framebuffer |
| **I/O** | `%1001` | memory-mapped device registers, physical `$00C000`–`$00FFFF` plus the slot decodes |
| **Special I/O** | `%1111` | the boot ROM *and* the MMU's own descriptor RAM |

The full physical I/O space map and the RAM-board physical base (RAM boards sit high, at physical
`$080000`, not at 0) are family-level facts and are not restated here — see [lisa.md](lisa.md) §3.1
and §3.2. Special I/O space "is used during startup, and when the registers that configure the MMU
are being modified. During normal operation, only the operating system has access to special I/O
space" [1] p. 1-6. The MMU's space flags — MEM, IO, RO, STK — are decoded from the SLR's access
bits and generate the cycle-type signals the rest of the board consumes (§3.3).

### 1.3 Machines that carry it

Every Lisa processor board carries the same MMU: the original Lisa 1, the Lisa 2 variants, and the
Macintosh XL, which is a Lisa 2/10 with the square-pixel screen kit and the "3A" boot ROM
([lisa.md](lisa.md) §1.1, §17). The 1983 Hardware Manual documents the part across the Lisa 2 and
Macintosh XL population [1]. The descriptor RAM geometry, the access codes and the START-mode
switch are identical on all of them; what differs per software load is only the map that gets
programmed into the descriptor RAM (§4).

### 1.4 What belongs to this page

The MMU's neighbours on the processor board are documented at family level and cited here rather
than restated: the control-strobe latches that hold SEG1/SEG2 and START ([lisa.md](lisa.md) §6.1),
the Memory Error Address latch and Status Register that the OS reads when classifying an MMU fault
([lisa.md](lisa.md) §6.3, §7.4), and the bus time-out logic that turns an unacknowledged device
access into a bus error ([1] §2.7 p. 2-28; §4.7.5 p. 4-38). The interrupt architecture the
supervisor-mode switch participates in is [lisa.md](lisa.md) §7. This page owns the descriptor RAM,
the translation and check logic, START mode, and the software contract built on top of them.

## 2. Register file

### 2.1 The descriptor RAM

The MMU's registers are "physically a matrix of 1024 12-bit registers... divided into four sets of
128 pairs of registers, one for each of the four contexts" [1] §2.3.2 p. 2-5a; the storage itself
is "a 1K by 12 bit RAM memory matrix" [1] §4.4.1 p. 4-20. Each pair is one segment descriptor:

| Register | Width | Contents |
| --- | --- | --- |
| **SOR** — Segment Origin Register | 12 bits | physical page number of the segment's first page (§2.2) |
| **SLR** — Segment Limit Register | 12 bits | bits 11–8: access/space code; bits 7–0: length in pages (§2.3) |

Per context there are 128 pairs (one per logical segment), and there are four contexts, giving
4 × 128 × 2 = 1024 words. The ten lines that address the RAM during any descriptor access are
([1] §4.4.1 p. 4-22):

| Address input | Source |
| --- | --- |
| 7 segment lines | UA17–UA23, the unbuffered high-order CPU address lines |
| 2 context lines | MS1, MS2 — driven by the SEG1 and SEG2 latch bits, *not* by the CPU's function-code output (§3.4) |
| 1 register-select line | B/L/ — base (SOR) vs limit (SLR) select, derived from UA3 (§2.4) |

Note that the descriptor RAM is addressed as RAM by these lines; the descriptor *write* is an
ordinary special-I/O write cycle to an address that encodes segment, context and register (§2.4).

### 2.2 Segment Origin Register (SOR)

The SOR "contains the 12-bit page address in physical space where the corresponding physical
segment begins" [1] §2.3.1 p. 2-4. The physical page is 512 bytes, so 12 bits of page number cover
the full 2 Mbyte physical space. "A segment which was to begin at the physical address `$00020000`
would have its corresponding SOR set to `$0010`. The SOR contains the origin in terms of multiples
of 512-byte pages in the physical space" [1] pp. 2-6 – 2-7. The segment is contiguous in physical
space: every translated page displacement is added to this one origin, so a segment can only map a
contiguous physical run — there is no per-page indirection.

The SOR's high nibble participates in the relocation add; the adder's *displacement* input has its
high-order nibble forced to zero, so the relocated page number stays within the 12-bit physical
page space ([1] §4.4.3 p. 4-24). Bit 0 of the stored SOR is therefore the physical page's parity of
`$200` alignment: origins are 512-byte granular, nothing finer.

### 2.3 Segment Limit Register (SLR)

The SLR packs a 4-bit access code and an 8-bit length ([1] §2.3.1 p. 2-4; §4.4.1 p. 4-21):

| SLR bits | Field |
| --- | --- |
| 11–8 | access-control / space-type code |
| 7–0 | segment length in 512-byte pages, in a sense that depends on the stack flag (below) |

#### 2.3.1 Access-control and space-type codes (bits 11–8)

The complete decode, including the invalid combinations, is Figure 2-4 of the manual [1] p. 2-7:

| Bits 11 10 9 8 | Meaning | MMU flag outputs (§3.3) |
| --- | --- | --- |
| `0 1 0 0` | main memory, read-only, **stack** | RO, STK |
| `0 1 0 1` | main memory, read-only | RO |
| `0 1 1 0` | main memory, read/write, **stack** | STK |
| `0 1 1 1` | main memory, read/write | — |
| `1 0 0 1` | I/O space | IO |
| `1 1 0 0` | **page invalid** (segment not present) | none |
| `1 1 1 1` | special I/O space | SPIO |
| `0 0 0 0`, `0 0 0 1`, `0 0 1 0`, `1 0 0 0`, `1 0 1 0`, `1 0 1 1`, `1 1 0 1`, `1 1 1 0` | invalid codes — "unpredictable results will occur" [1] Fig. 2-4 p. 2-7 | undefined |

The flags the hardware actually decodes are four — MEM, IO, RO and STK [1] §4.4.3 pp. 4-24 – 4-25 —
and "all combinations are neither valid nor covered by the above" [1] p. 4-25: the ten codes not in
the valid list have no defined behaviour. The Lisa OS uses only the seven named codes
([3] source-MMPRIM.TEXT: read-only `$5`, stack `$6`, read/write `$7`, absent `$C`; the OS's I/O-space
re-map writes `$9` and `$C`, see §4.4). The boot ROM's own change log shows the I/O code was not
always stable in software: "change MMU I/O space code to '9'" is a dated change to the ROM, and the
4 K ROM variant of the source still assembles `$0800` — access code `%1000`, which the 1983 manual
classifies as invalid — as its I/O-segment limit value ([2] RM248.E.TEXT equates `IOLMT`).

#### 2.3.2 Length encoding (bits 7–0)

"The segment length is interpreted in TWO'S COMPLEMENT FORM. A length of `$00` implies a segment of
maximum length, 128 Kbytes. A length of `$FF` implies the minimal segment length of 512 bytes, one
page. An exception to the above occurs in the case of a stack segment... Thus `$00` implies one
page, 512 bytes, and `$FF` implies 128 Kbytes for a stack segment only" [1] p. 2-7. Concretely, for
a segment of *L* pages:

| SLR bits 7–0 | Normal segment (length *L*) | Stack segment (length *L*) |
| --- | --- | --- |
| stored value | `$100 − L` (mod 256) | `L − 1` |
| `$00` | 256 pages = 128 KB | 1 page = 512 B |
| `$01` | 255 pages | 2 pages |
| `$80` | 128 pages = 64 KB | 129 pages |
| `$FF` | 1 page = 512 B | 256 pages = 128 KB |

The stack form is not a documentation convention; it is how the limit adder is wired (§3.2), and
the Lisa OS computes exactly these values when programming a stack segment (§4.3). A stack segment
occupies the *top* *L* pages of its 128 KB logical segment and grows downward [1] p. 2-7;
[lisa.md](lisa.md) §4.2.

### 2.4 Descriptor addressing: the special-I/O register window

All MMU registers "are addressed in special I/O space" [1] §2.3.2 p. 2-5a. An access reaches the
descriptor RAM when the cycle is a special-I/O cycle (the segment maps `%1111`, or START mode forces
it, §3.5) **and** the untranslated address lines UA15 and UA16 decode to the MMU half of that
space: MMUIO is asserted when SPIO is asserted, UA15 is asserted and UA16 is deasserted
([1] §4.4.2 p. 4-23). The other half (UA15 low) is the boot ROM ([1] §2.4 pp. 2-10 – 2-11;
§4.7.1 pp. 4-35 – 4-36 — but see §5 on a polarity contradiction inside the manual).

The address word that selects a descriptor has the form
`SSSSSS$01xxxxxxxxxxxxBxxx` [1] §2.4 p. 2-10, Figure 2-3 p. 2-6:

| Address bits | Value | Meaning |
| --- | --- | --- |
| 23–17 | segment number `S` | which of the 128 descriptors is selected — in *all four* tables' index; the context comes from the latch (§3.4) |
| 16, 15 | `%01` | the MMUIO decode (UA16 = 0, UA15 = 1) |
| 14 | 0 | in START mode, required to make the access special I/O at all (§3.5); in normal mode, don't care for the decode |
| 13–4 | don't care | |
| 3 | `B` | `1` selects the SOR, `0` selects the SLR — "the SOR or SLR are selected by the state of the UA3 line" [1] §4.4.2 p. 4-23 |
| 2–0 | don't care | |

So the descriptor for segment *n* is reached at base `$nn × $20000 + $8000` (SLR) or `... + $8008`
(SOR) — the boot ROM's equates are exactly this arithmetic: `MMU0L = $00008000`,
`MMU0B = $00008008`, `MMU126L = $00FC8000`, `MMU127B = $00FE8008`, and the OS's descriptor
programmer steps its port pointer by `$20000` per segment ([2] RM248.E.TEXT MMU equates;
[3] source-LDASM.TEXT constants `slim = $8000`, `sorg = $8008`, `next_mmu = $20000`).
The full traversal in one context is `$00008000` (segment 0 SLR) to `$00FE8008` (segment 127 SOR)
in `$20000` steps ([2] RM248.E.TEXT `MMUSADRL`/`MMUEADRB`).

Every register is written "in a separate write cycle" — there is no block-load mechanism; the SEG1
and SEG2 latch bits pick the context table and UA17–UA23 pick the segment [1] §4.4.2 p. 4-23.

### 2.5 Read-back behavior

Descriptors are readable through the same window: data moves through bidirectional LS245
transceivers whose direction is the CPU's R/W line, and "register contents can be read via the same
path" [1] §4.4.2 pp. 4-23 – 4-24. Two properties of the read are pinned by software reliance:

1. **Only 12 bits are stored.** A word read of a descriptor returns the register in bits 11–0;
   bits 15–12 are undefined. Both the boot ROM and the OS mask every read-back with `$0FFF`, and
   the boot ROM's warm-start check documents the expectation explicitly — it compares segment 126's
   SLR against `$x901`, "x = random value" ([2] RM248.K.TEXT warm-start check and the `ANDI #$0FFF`
   masks throughout `MMURW`/`CHKRW`/`CONCHK`; [3] source-LDASM.TEXT `move.w (a2),d7` path).
2. **The read is not destructive** and does not disturb translation: the boot ROM's `CONCHK`
   context test reads three descriptors back in a loop as its entire mechanism (§3.4).

### 2.6 Reset state

"When the Lisa performs a Power-On Reset (POR), the MMU registers are in an unknown state" [1]
§2.3.3 p. 2-8. Nothing in the hardware clears the RAM on reset; the descriptor RAM is ordinary
static RAM and its contents survive a system reset that does not remove power — the boot ROM's
warm-start detection depends on reading previously-programmed descriptors immediately after reset
(§4.1) (*observed* in the ROM source's BEGIN path, [2] RM248.K.TEXT). The context latch bits and
the START latch likewise live in an LS259 latch that reset does not clear ([1] §4.7.2 p. 4-36;
§4.1 for what the ROM does about it).

START is "satisfied automatically at power-on time, or by the software accessing I/O address
`$00E010`" [1] §2.3.3 p. 2-8 — the hardware asserts START for you at power-on, so the very first
instruction fetch already works (§3.5).

## 3. Behaviour

### 3.1 Logical address decomposition and translation

A 24-bit logical address splits into three fields ([1] §2.3 pp. 2-3A – 2-4, Figure 2-1):

```
 23                 17 16                9 8               0
+---------------------+-------------------+-----------------+
|  segment number     | page displacement | byte in page    |
|  (7 bits: 0 - 127)  | (8 bits: 0 - 255) | (9 bits)        |
+---------------------+-------------------+-----------------+
```

- **Bits 23–17** select one of 128 segments: "UA17-UA23 are used to select one of the 128 possible
  logical segments" [1] §4.4 p. 4-18.
- **Bits 16–9** are the page displacement, "UA9-UA16 address a page within that segment" [1] §4.4
  p. 4-18 — up to 256 pages.
- **Bits 8–0** pass through untranslated: "Address lines UA1-UA8 are not operated on by the MMU"
  [1] §4.4 p. 4-18, and the nine low-order lines "are presented directly from the CPU... used as
  memory row addresses" [1] §4.4.4 p. 4-25.

A segment is therefore 512 bytes to 128 KB; logical space is 128 × 128 KB = 16 Mbytes, matching the
68000. Translation per cycle:

```
physical_page = SOR + page_displacement        ; 12-bit adder, high nibble of the
                                              ; displacement input forced to zero
physical_addr = (physical_page << 9) | (logical & $1FF)
```

The physical address word is 21 bits, "sufficient to address a space of 2 Mbytes" [1] p. 2-5a; it is
latched "on the falling edge of the MALE signal... stable on the A9-A20 lines if the BGACK/ signal
is deasserted, no DMA access in process, and CMUX is asserted" [1] §4.4.3 p. 4-24. Because the
relocation is one affine add over the whole segment, a contiguous run of translated pages is
contiguous in physical space — the OS exploits this and maps whole segments with two descriptor
writes (§4.3).

The video refresh path does *not* go through the MMU: the video address counter is loaded from the
Video Address Latch, which holds a physical address ([1] §2.5.5 pp. 2-25 – 2-26; [lisa.md](lisa.md)
§6.2, §8). Only CPU cycles translate.

### 3.2 The limit check and ACCK

In the second half of the bus cycle, after the physical address is latched, "the access limits are
checked. The B/L/ signal changes polarity and the contents of the SLR are read out. The eight
low-order bits, which indicate the number of pages contained in this segment, are also added to
the page address given by the UA9-UA16 lines from the CPU. The overflow line from the second nibble
is the access check (ACCK) signal. If ACCK is asserted, this indicates that the desired page lies
outside the limit set for the segment in question" [1] §4.4.3 p. 4-24. The check is therefore
additive, not comparative: the SLR's length byte is added to the displacement and the adder's carry
decides:

| Segment kind | SLR byte | Carry-in (STK) | Access in limit when |
| --- | --- | --- | --- |
| normal | `$100 − L` | 0 | no carry: `displacement < L` |
| stack | `L − 1` | 1 | carry (ACCK inverted): `displacement ≥ 256 − L` — the top *L* pages |

"The STK term being asserted causes a carry input to be presented to the low-order adder" and the
inversion of ACCK for stacks "is implemented in the S86 gate" [1] §4.4.3 pp. 4-24 – 4-25 — so the
stack sense is a hardware function, not a software convention. For a stack segment the valid window
is the top of the 128 KB segment, growing downward, exactly as the manual describes stack usage
[1] p. 2-7 and as the OS's SOR arithmetic confirms (§4.3).

"An overflow results in the suppression of the CAS/ signal, which prevents any memory operation
from taking place" [1] §4.4.3 p. 4-24. The fault is raised to the CPU as a bus error (§3.7).

### 3.3 Space decode and the flag outputs

The SLR's high nibble yields four flags, "the type of segment that is being accessed" [1] §4.4.3
pp. 4-24 – 4-25:

| Flag | Asserted for | Effect on the cycle |
| --- | --- | --- |
| **MEM** | memory-space codes (`%01xx`) | "enables the CAS signal for a memory access" |
| **IO** | I/O code (`%1001`) | "initiates an I/O cycle via the IOCY flop" |
| **RO** | read-only codes (`%0100`, `%0101`) | "inhibits a write cycle to memory... also inhibits an SPIO cycle" |
| **STK** | stack codes (`%0100`, `%0110`) | supplies the limit adder's carry-in (§3.2) |

These flags are also how the rest of the board learns what kind of cycle is running: "The IOCY and
SPIO signals indicate that an I/O or special I/O cycle is in progress. The IOCY and SPIO signals
are generated from information contained in the MMU" [1] p. 4-11. The special-I/O code `%1111` makes
the cycle an SPIO cycle, which is what routes the access to the boot ROM or back into the MMU's own
descriptor window (§2.4). The invalid code `%1100` and the ten undefined codes produce none of the
four flags, so the cycle is terminated without CAS — a fault (§3.7).

Because the space is a property of the segment, not of the address, the same numeric address can be
RAM in one context and a device register in another; the MMU's flags, not the address value, do the
routing. "Since logical addresses make no distinction among the three possible address spaces, each
can be operated on by the full 68000 command set" [1] p. 2-1.

### 3.4 Context selection: the SEG latch and the supervisor override

Four complete, independent descriptor tables exist. "The context in which the Lisa is currently
running is selected by two control bits, SEG1 and SEG2" [1] §2.3.4 p. 2-9:

| SEG2 | SEG1 | Context |
| --- | --- | --- |
| 0 | 0 | 0 — the operating system |
| 0 | 1 | 1 |
| 1 | 0 | 2 |
| 1 | 1 | 3 |

"Context 0 is intended for exclusive use of the operating system, while contexts 1, 2, and 3 are
intended for general purpose use. The Lisa automatically selects context 0 whenever an access is
made in supervisor mode" [1] §2.3.4 p. 2-9. The supervisor override is a hard wire: "The FC2 signal
originates in the 68000... Mapping is forced to the supervisor context, context 0, when FC2 is
asserted" [1] p. 4-11a. This single fact is the machine's whole protection model — a `TRAP`
instruction, an interrupt, or any other supervisor entry switches the address space to the OS's
table on the next cycle, without software touching the latch [1] §2.3.4 p. 2-9; [lisa.md](lisa.md)
§4.5 notes the corollary that the *return* to user mode (`RTE`, `ANDI`/`MOVE` to SR) re-selects the
latch context just as live.

The context bits are board latches, not MMU registers: they are set and reset by *strobing* — any
access, read or write, the data value ignored — addresses in the processor-board control block
([1] §2.3.4 p. 2-9; Figure 2-17 p. 2-26): `$00E008`/`$00E00A` reset/set SEG1 and
`$00E00C`/`$00E00E` reset/set SEG2, documented at family level in [lisa.md](lisa.md) §6.1. The
boot ROM equates them `SEG1OFF = $00FCE008`, `SEG1ON = $00FCE00A`, `SEG2OFF = $00FCE00C`,
`SEG2ON = $00FCE00E` and the Lisa OS calls the same four `$FCE008`–`$FCE00E` "the memory
locations to change mmu domain" ([2] RM248.E.TEXT; [3] source-starasm1.TEXT `SET_DOMAIN`).
"Normally, the context is changed while executing in supervisor mode. Execution in the new context
begins when user mode is entered" [1] §2.3.4 p. 2-9.

**Descriptor-RAM addressing ignores the supervisor override.** The MS1/MS2 lines that form part of
the descriptor RAM's address come from the SEG1/SEG2 latches, not from the FC2 override
([1] §4.4.1 p. 4-22). Supervisor-mode code therefore reads and writes whichever table the latch
selects. The boot ROM's context test is the direct proof: it runs entirely in the supervisor state
(after reset the 68000 is in supervisor mode), strobes SEG1/SEG2 to each context in turn, and then
`CONCHK` reads descriptors back expecting them *not* to match context 0's programmed values —
"a comparison to ensure destruction of context 0 mapping avoided" — which only works if the latch,
not FC2, selects the table ([2] RM248.K.TEXT `MMUTST2`, `CONCHK`). The Lisa OS relies on the same
property when it programs user domains 1–3 from supervisor code (§4.3).

### 3.5 START mode: the built-in bypass

At power-on the descriptor RAM is garbage, yet the 68000 must fetch its reset vector from ROM. The
hardware solves this with the START (setup) mode [1] §2.3.3 p. 2-8:

1. START "is satisfied automatically at power-on time" — the latch comes up set [1] p. 2-8.
2. While START is set, "bit 14 of the address acts as a switch between special I/O space and other
   address spaces" [1] p. 2-8]:
   - **bit 14 = 0** — the access is a special-I/O cycle regardless of descriptor contents: "A
     `1111` code output for the access bits of the SLR registers is provided by hardware in setup
     mode" [1] p. 2-8. The special-I/O decode (§2.4) then splits it: UA15 low → the boot ROM,
     UA15 high → the MMU descriptor RAM.
   - **bit 14 = 1** — "the MMU performing an address translation, just as in normal processing"
     [1] p. 2-8]. RAM-resident code can therefore keep running while START is set, provided its
     addresses carry bit 14.
3. This is exactly how the reset flow works: the reset vector fetch at logical `$000000`/`$000004`
   has bit 14 clear, so it is answered from the boot ROM, whose first words *are* the reset vectors
   — the boot ROM image is "ORG'ed AT 0 BUT RUNS AT `$00FE0000`" and begins with the initial SSP
   (`$00000480`), the initial PC (`$00FExxxx`, "assumes use of MMU reg 127"), and the exception
   vectors ([2] RM248.K.TEXT header comment and vector table). The initial PC points into the same
   ROM at its segment-127 logical address.
4. "The boot ROM initializes the MMU so that physical memory is contiguous and starts at address
   0. Accesses higher than the top of physical memory are not allowed except for segment 126
   (addresses of the form `$FCxxxx`) addresses I/O space, and segment 127 (addresses of the form
   `$FExxxx`) addresses special I/O" [1] p. 2-8. Then the ROM clears START and normal translation
   begins (§4.1).

START is set and cleared by the SETUP strobes `$00E010`/`$00E012` in the processor-board control
block ([1] Figure 2-17 p. 2-26; §4.7.2 p. 4-36; [lisa.md](lisa.md) §6.1). The boot ROM's equates
name them explicitly: `SETUPON = $00FCE010` — "ADDRESS TO TURN SETUP ON", and `SETUP = $00FCE012` —
"ADDRESS TO TURN SETUP BIT OFF" ([2] RM248.E.TEXT; usage in RM248.K.TEXT `START CLR.B SETUP ; TURN
OFF SETUP TO ENTER MAP LAND`). See §5 for the polarity confusion this strobe pair carries.

While START is set, the exception-vector fetches at logical `$000`–`$3FF` (bit 14 clear, UA15 low)
resolve to the boot ROM's own vector words, not to RAM: the ROM image deliberately carries its
bus-error, address-error, illegal-instruction... vectors right after the reset pair, each pointing
at a ROM-resident handler ([2] RM248.K.TEXT). An exception taken during setup mode therefore lands
in ROM code, not at whatever the descriptor RAM happens to say. The Lisa OS, by contrast, keeps
interrupts masked whenever it turns setup on to program descriptors, and re-enables them only with
setup off (§4.3).

### 3.6 Bus-cycle timing

The 68000 and the video logic interleave memory accesses ([1] §1 pp. 1-7 – 1-8), and the MMU's
two-phase work is folded into that interleave [1] §4.2.2 p. 4-11a, Figure 4-8:

| Phase | Timing | What happens |
| --- | --- | --- |
| Relocate (SOR read) | MALE asserted "at the end of a CPU cycle when the AS signal becomes deasserted... typically around the t3 period of the video cycle" [1] §4.4.1 p. 4-22 | B/L/ selects the base register; SOR + displacement is computed; the result is latched on the falling edge of MALE into the address latches, "stable on the A9-A20 lines" [1] §4.4.3 p. 4-24 |
| Check (SLR read) | "at the end of t7 time during the video cycle... With MALE deasserted, the B/L/ term also becomes deasserted and the SLR... is used to check the access limits and also give the type of cycle to be performed" [1] §4.4.1 pp. 4-22 – 4-23 | the limit adder runs (§3.2); the space flags latch (§3.3) |

"The address strobe (AS) signal being recognized during the video cycle... permits the MMU to
calculate and check the physical address in time for the result to be ready for the processor
cycle" [1] p. 4-11. The memory timing benefits from the pass-through: "Since the nine low-order
bits of the address are presented directly from the CPU, these are used as memory row addresses.
This avoids the need to wait for the output of the MMU to become stable before any memory
addressing can be done" [1] §4.4.4 p. 4-25. RAS "is generated even for cycles that turn out to be
I/O or erroneous. This does no harm provided that CAS is generated for such cycles" [1] §4.4.4
p. 4-25 — the MMU suppresses CAS, not RAS, on a fault (§3.2).

A descriptor write has its own micro-sequencing: it is a special-I/O cycle, "begins with the
assertion of the SPIO signal, which directly generates the MMUIO signal" [1] p. 4-11a; MMUIO
releases the clear inputs of two S109 JK flops, "allowing the E245 signal to be asserted at t4 time
and WMMU/ to be deasserted at t3 time if the 68000 is performing a read cycle", with data moving
through the two LS245 bidirectional drivers under CPU R/W control [1] §4.4.2 p. 4-23.

### 3.7 Violations and the bus error

Three conditions terminate a cycle as a fault ([1] §4.4.3 p. 4-24; §2.3.1 p. 2-5a):

1. **Out of limit** — ACCK says the page lies outside the segment (§3.2).
2. **Invalid segment** — the SLR code is `%1100`; none of the space flags assert, so nothing is
   enabled and no memory operation occurs.
3. **Write to a read-only segment** — the RO flag "inhibits a write cycle to memory" [1] §4.4.3
   p. 4-25].

In every case CAS is suppressed — "the access is terminated and an error condition is presented to
the CPU" [1] p. 2-5a — and the CPU takes the **bus error** exception, vector `$000008` ([1] §2.6
Figure 2-18 p. 2-27; [lisa.md](lisa.md) §7.2). How the terminated cycle becomes the BERR the CPU
sees — a dedicated error term versus the board's 30–300 µs bus time-out ([1] §2.7 p. 2-28) — is not
stated in the manual and is listed under §6.1; what is pinned is the software contract: the Lisa OS
treats an MMU fault exactly like a time-out bus error, reads the Status Register and the Memory
Error Address latch to classify it, and its demand-loading depends on the fault being delivered for
data accesses as well as instruction fetches (§4.6; [lisa.md](lisa.md) §4.8, §7.4).

## 4. Programming model

### 4.1 Power-on: the Boot ROM's MMU bring-up

The boot ROM (the released RM248 assembly source is the citable witness for this section) drives the
MMU as follows on a cold start ([2] RM248.K.TEXT):

1. **Run from ROM in START mode.** The reset vectors at ROM offset 0 supply initial SSP `$00000480`
   and an initial PC in the `$00FExxxx` ROM window; every fetch has bit 14 clear, so START-mode
   special-I/O decode (§3.5) answers it from the ROM.
2. **Warm-start detection.** Before touching anything, the ROM reads descriptors back: it reads
   segment 126's SLR expecting `$x901` — the ROM's own `IOLMT2`, "limit value for no reset
   feature", i.e. I/O space with a one-page-shorter limit than the normal map — and if that
   matches, reads segment 126's SOR expecting `$x000`. A match means the machine was reset rather
   than power-cycled into a map the ROM recognizes, and the ROM reprograms segment 0 (read/write
   low memory, so it can save registers), 126 (I/O) and 127 (special I/O), clears SETUP, and
   enters the ROM monitor directly. A second, weaker check reads segment 127 for the special-I/O
   pattern; a match sets the warm-start flag and skips parts of the self-test ([2] RM248.K.TEXT
   `BEGIN`, `BEGIN2`, build-conditional `NORESET`). This is the software contract that pins
   descriptor read-back (§2.5) and descriptor survival across reset (§2.6); who first leaves
   `$x901` in segment 126 is not established by the source (§6.13).
3. **Test the descriptor RAM.** The ROM writes and reads back every descriptor of the current
   (latch-selected) table between `$00008000` and `$00FE8008`, three patterns per pass, then
   repeats with walking patterns to catch address-line faults (`MMUTST` → `MMUINIT`, `MMURW`,
   `MMUACHK`). It then tests the other three contexts: it strobes SEG1 (context 1), both (context 3),
   SEG2 alone (context 2) and runs the same read/write pass per table, verifying via `CONCHK` that
   the table actually changed (`MMUTST2`). On failure the ROM toggles every address and data line
   to the MMU in a diagnostic loop (`MMUERR`, `TSTLOOP`).
4. **Install the default map in context 0.** `SETMMU` writes, for segments 0–15: SOR = *n* × `$100`
   and SLR = `$0700` (read/write memory, 256 pages — a full 128 KB); for segment 126: SOR = 0,
   SLR = `$0900` (I/O space); for segment 127: SOR = 0, SLR = `$0F00` (special I/O). Physical memory
   is thus contiguous from the bottom of the low segments, I/O is reachable at logical `$FCxxxx` and
   the ROM at `$FExxxx` ([1] §2.3.3 p. 2-8; [2] RM248.K.TEXT `SETMMU`). The segments the ROM does
   not program are left at the invalid page pattern `$0C00` by the preceding address-check pass
   (`MMUACHK` "leaves limit registers with invalid page value"), so the map is: segments 0–15 RAM,
   16–125 invalid, 126 I/O, 127 special I/O.
5. **Invalidate the user contexts.** For contexts 1, 3 and 2 (in that order), the ROM writes the
   invalid pattern `$0C00` into every SLR and 0 into every SOR, then resets the latch to context 0
   ([2] RM248.K.TEXT `INITMMU` loop, `INVPAG = $0C00`).
6. **Leave START mode.** `START CLR.B SETUP ; TURN OFF SETUP TO ENTER MAP LAND` — from here the
   MMU translates every cycle ([2] RM248.K.TEXT).

Two disciplines in the ROM's code follow directly from START-mode semantics (§3.5). First, the
entire pre-`SETUP`-off phase is **stack-free by construction**: its subroutine calls use
register-return macros (`BSR4`/`RTS4`, `BSR6`/`RTS6`, `BSR2`/`RTS2` — `LEA return,A4/A6/A2; BRA
target` and `JMP (An)`), never `BSR`/`RTS`, because a push to the low-logical stack at `$0480` (bit
14 clear) would be a special-I/O write, not RAM ([2] RM248.E.TEXT macro definitions; RM248.K.TEXT).
Second, when the ROM monitor needs to run again, its NMI handler toggles SETUP: it turns SETUP off
to reach memory, services the parity error, and turns it back on "to return to SETUP state" before
`RTE` — the monitor lives in ROM in setup mode ([2] RM248.K.TEXT `NMIEXCP`).

### 4.2 The default map and the `$FCxxxx`/`$FExxxx` convention

With the Boot ROM's map, I/O registers and the ROM are reached through segments 126 and 127:
logical `$00FCxxxx` reaches physical I/O `$00xxxx`, logical `$00FExxxx` reaches the boot ROM. All
the Hardware Manual's register tables list physical addresses (`$00E800`, `$00D901`, ...); driver
and OS source uses the logical forms; they are the same registers. This convention, and the full
physical I/O map behind it, are documented at family level in [lisa.md](lisa.md) §3.2 and §3.3 and
are not restated here.

### 4.3 The Lisa OS: the System Mapping Table and `DO_AN_MMU`

The Lisa Office System does not write descriptors ad hoc. It keeps a **System Mapping Table (SMT)**
in memory — one 4-byte entry per segment per domain, 512 bytes per domain, 2 KBytes for all four —
with the layout its descriptor programmer consumes ([3] source-LDASM.TEXT, `INITMMUTIL`):

| SMT entry offset | Size | Contents |
| --- | --- | --- |
| +0 | word | origin, in 512-byte pages, relative to the start of physical memory for memory segments |
| +2 | byte | access code (the SLR's bits 11–8 value) |
| +3 | byte | length in pages, in the OS's own sense (see below) |

The programmer is a small assembly routine, `DO_AN_MMU`, entered via trap #6 (`PROG_MMU` is its
Pascal wrapper), which copies itself into a dedicated MMU segment (`mmucodemmu`, segment 84, mapped
read/write 16 KB + 5 pages) and is entered at an address with bit 14 set so that it keeps executing
through the MMU while SETUP is on ([3] source-LDASM.TEXT). For each segment to program, it:

1. Strobes the context latch to the target domain (the raw-latch addressing of §3.4).
2. Turns SETUP off, restores the caller's interrupt level ("interrupts/breakpoints can happen
   here"), masks interrupts again, and turns SETUP on — "Enable interrupts on each loop, but leave
   them off while SETUP's on!".
3. Reads origin, access and length from the SMT and converts them to hardware values:
   - **stack segment** (access `$6`): if length = 0 it means 128 KB, so length := `$100`; then
     `origin := origin + length − $100` and `length := length − 1` — exactly the `L − 1` SLR form
     and the top-of-segment SOR of §2.3.2/§3.2;
   - **non-stack**: `length := neg.b length` — the two's-complement form;
   - **memory segments** (access ≤ `$700`): `origin := origin + location of physical byte 0` — the
     SMT stores origins relative to the installed RAM base, which on a Lisa is *high* at physical
     `$080000` ([lisa.md](lisa.md) §3.1), so the same SMT works wherever the boards are.
4. Writes the SLR (`move.w d3,(a2)` — the port at `segment << 17 | $8000`) and the SOR
   (`move d2,8(a2)` — `+8` selects SOR), then steps the port by `$20000` for the next segment.
5. Turns SETUP off, strobes the latch back to the caller's domain, and `RTE`s.

The one-segment case is wrapped by the shared utility `CHANGE_IOSPACE`, which the OS uses to map
and unmap I/O space from a running process: it writes `$900` (I/O) or `$C00` (invalid) into the
SMT's `iospace` entry for the current domain and re-programs that one descriptor through trap #6
([3] source-starasm1.TEXT).

### 4.4 Contexts as domains: the scheduler's use

The OS calls contexts **domains**: domain 0 is the system, domains 1–3 are user address spaces
([3] source-MMPRIM.TEXT, source-SCHED.TEXT). The scheduler keeps a Domain Context Table with one
owner per domain, and on every process launch `SET_ADDRESS_SPACE` either re-selects a domain the
process still owns (`SET_DOMAIN` strobes the latch — `ctbit1on = $FCE00A`, `ctbit1off = $FCE008`,
`ctbit2on = $FCE00E`, `ctbit2off = $FCE00C`, "established by touching" the strobe, value ignored)
or picks a free domain (LRU among 1–3, `SELECTDOMAIN`) and re-maps the process's whole address
space into it through the SMT ([3] source-SCHED.TEXT; source-starasm1.TEXT `SET_DOMAIN`).
`SET_DOMAIN`'s own comment pins the supervisor rule of §3.4 from the software side: "Can only be
called from the supervisor stack" — the latch is strobed from supervisor code, and because
descriptor addressing ignores FC2, the strobes program the *user* tables while translation of the
running supervisor code stays in context 0.

### 4.5 The OS segment allocation

The Lisa OS fixes the meaning of most segment numbers, in context 0 and replicated into the user
domains ([3] source-MMPRIM.TEXT):

| Segment | Name | Purpose |
| --- | --- | --- |
| 84 | `mmucodemmu` | "mmu used to re-map other mmu segments" — the SMT and `DO_AN_MMU` live here |
| 85–100 | `realmemmmu` × 16 | "defining real memory"; logical base `$AA0000` (`logrealmem`) |
| 101 | `superstkmmu` | supervisor stack |
| 102 | `sysglobmmu` | sysglobal data |
| 103 | `syslocmmu` | syslocal |
| 104–106 | `minsysldsnmmu`–`maxsysldsnmmu` | system load segments (−2…0); 106 is the last used for user code |
| 107–113 | `ldsn1mmu`–`ldsn7mmu` | load segments 1–7 (demand-loaded code/data, §4.6) |
| 123 | `stackmmu` | process stack, including the inter-segment jump table |
| 124 | `shrsegmmu` | shared Intrinsic Units' global data |
| 125 | `screenmmu` | the screen |
| 126 | `iospacemmu` | system I/O space (defined in source-DRIVERDEFS.TEXT) |
| 127 | `prommmu` | "reserved to access the prom" |

The OS's access-code constants are the manual's: read-only `$5`, stack `$6`, read/write `$7`,
absent `$C` ([3] source-MMPRIM.TEXT).

### 4.6 Demand loading through the bus-error handler

The OS's main use of the MMU fault is **demand loading**: a not-yet-resident code or data segment
is left with its SLR at the invalid code `$C`, and the first reference — a control transfer into it
or a data access — takes the bus error. The OS's `BUS_ERR` handler then loads the segment and
returns ([3] SOURCE-EXCEPASM.TEXT; [lisa.md](lisa.md) §4.8). The contract is exact:

**The frame.** The 68000 group-0 frame the handler parses (its own symbol names and offsets below
the stacked SR/PC):

| Handler symbol | Frame offset | Contents |
| --- | --- | --- |
| — (special status word) | +$00 | R/W, I/N, function-code bits |
| `BADADDR` | +$02 (long) | the faulting logical address |
| `B1`, `B2` | +$06 (two bytes) | the instruction register — on a 68000, the opcode of the instruction whose *prefetch* faulted |
| `SRX` | +$08 | status register |
| `PCX` | +$0A (long) | program counter |

**Classification.** The handler first rejects any fault taken with an interrupt level active in
`SRX` (non-recoverable). It then reads `B1`/`B2` and matches the opcode of the control transfer
that faulted, backing the saved PC up by the instruction's prefetch advance so that retry re-executes
the transfer into the now-present segment ([3] SOURCE-EXCEPASM.TEXT `BUS_ERR`):

| Opcode | Subcode (`B2`) | Saved-PC adjustment | Extra effect |
| --- | --- | --- | --- |
| `RTS` (`$4E75`) | `$75` | `−2` | `USP −= 4` — undo the pop, so the re-executed `RTS` pops again after the load |
| `RTE` (`$4E73`) | `$73` | set to `BADADDR` | resume at the faulting address itself |
| `JMP.L` (`$4EF9`) | `$F9` | `−2` | — |
| `JSR.L` (`$4EB9`) | `$B9` | `−6` | — |
| `JMP (An)` (`$4Ex0`, mask `$F8 = $D0`) | `$D0` | `−2` | — |
| `JSR d(An)` (`$4Ex8`) | `$A8` | `−4` | — |
| `JSR (An)` (`$4Ex0`) | `$90` | `−2` | — |
| `TST` (opcode `$4A`) | — | `−2` | stack-overflow path: the fault address is checked against the stack segment and `CHECK_ST` decides whether the stack can be expanded |

`CODE_CHK` extracts the segment number (`SWAP; LSR.B #1; ANDI #$7F`) and 17-bit offset from
`BADADDR` and calls the memory manager's `CHECK_CS`: a genuinely-absent segment of the faulting
process enters the scheduler (which loads it and re-runs the access); anything else escalates to a
hard bus error. The full recovery sequence — including the RTS/user-stack rollback and the
requirement that *data* faults reach the handler just as fetch faults do — is treated at family
level in [lisa.md](lisa.md) §4.8; the handler above is the primary source for it.

### 4.7 MacWorks XL

MacWorks XL (the Macintosh XL's Macintosh-emulation environment) does not extend the Boot ROM's
map — it takes the descriptor tables over completely. Recorded during MacWorks XL boot bring-up
(*observed*): the MacWorks boot file loads, reprograms the segment descriptors to present a
Macintosh-shaped, essentially flat memory map, maps a Macintosh ROM image at the logical
`$00400000` region (its own code checks `CMPI.L #'MACW', $00400040` to detect the environment), and
rewrites the hardware-dependent kernel and low-memory globals to Lisa addresses. The Boot ROM's
contribution ends at loading the boot block — "the Lisa is the last Apple computer that did not
include an operating system in the ROM" — so everything above the disk boot is MacWorks' own MMU
programming. The Lisa Office System 3.1 also boots end-to-end on the segment MMU with the
SMT/demand-load machinery of §4.3–§4.6 (*observed*, LOS 3.1 ProFile install).

## 5. Quirks & errata

- **512-byte pages, 128 segments.** The Lisa page is 512 bytes and the segment is up to 128 KB —
  not the 4 KB pages of a Motorola PMMU. Every piece of software arithmetic above is in 512-byte
  units (§2.2, §3.1).
- **The length byte is two's complement — with a stack exception.** `$00` is a full 128 KB segment
  and `$FF` a single page; for stack segments the sense is inverted by the STK carry-in and the
  inverted ACCK (§2.3.2, §3.2). Software that stores a plain length into bits 7–0 gets a segment
  of the complement size.
- **Descriptor reads return garbage in bits 15–12.** The registers are 12 bits; every read-back is
  masked `$0FFF`, and the ROM's warm-start check documents the top nibble as a random value
  (§2.5).
- **The supervisor override applies to translation only.** Descriptor-RAM addressing follows the
  raw SEG1/SEG2 latch, so supervisor code can — and both the ROM and the OS do — read and write
  the user context tables while running on context 0 (§3.4).
- **START mode makes bit 14 a translation switch.** Every logical address with bit 14 clear is
  special I/O while START is set — including the stack. The boot ROM's MMU phase is stack-free by
  construction (register-return call macros), and the OS copies its descriptor programmer to a
  bit-14-set address before turning SETUP on (§3.5, §4.1, §4.3).
- **In START mode, the exception vectors come from the ROM.** Vector fetches at logical
  `$000`–`$3FF` have bit 14 clear, so they read the boot ROM's own vector words; the ROM image
  starts with its vector table for exactly this reason (§3.5).
- **The reset vectors live at ROM offset 0 and the ROM runs at `$00FExxxx`.** The initial PC is a
  `$00FExxxx` address, and the same ROM words answer the power-on fetch at logical `$000000`
  because START mode decodes them identically (§3.5).
- **The SETUP strobe pair is documented with opposite polarity by different sources.** Figure 2-17
  of the manual lists `$00E010` as "SETUP Register Reset" and `$00E012` as "Set", while §2.3.3 of
  the manual and the boot ROM's own equates agree that *accessing `$00E010` sets START mode* and
  `$00E012` ends it. The latch bit and the start-mode signal are of opposite sense; the software
  contract is the ROM's: `$E010` = setup on, `$E012` = setup off (§2.6, §3.5; §6.2).
- **The manual contradicts itself on the ROM/MMUIO decode.** §4.3.4 says ROM/ is asserted "while
  UA15 is high and UA16 is low", and §4.4.2 gives the same UA15-high/UA16-low condition for MMUIO
  — the two decodes would collide. Software and Figure 2-3 pin the working split: UA15 low → ROM,
  UA15 high → MMUIO, UA16 low in both cases (§2.4; §6.1).
- **The ROM/`MMUIO` decode leaves half of special I/O unassigned.** The LS139 decodes UA15/UA16 to
  two outputs; what a special-I/O cycle with UA16 set does is not documented (§6.4).
- **Invalid access codes were once used on purpose.** The 4 K ROM variant programs its I/O
  segment with `%1000`, which the 1983 manual classifies as invalid with "unpredictable results";
  a dated change to the ROM moved the I/O code to `%1001` (§2.3.1).
- **The descriptor RAM survives reset.** Nothing clears it on a system reset; the ROM's warm-start
  path is built on reading back previous contents (§2.6, §4.1).
- **RAS fires even for faulting and I/O cycles.** The MMU suppresses CAS on a violation, never
  RAS; harmless "provided that CAS is generated for such cycles" (§3.6).
- **The context latch also survives, and reset leaves it undefined.** The ROM always ends its
  context tests by strobing SEG1OFF/SEG2OFF back to context 0 before continuing (§4.1); software
  that assumes a known latch value after reset is wrong.
- **`CHECK_CS` runs on the *logical* address from the fault frame.** The demand-load recovery keys
  off the group-0 frame's fault address and opcode bytes; an implementation that reports a
  different address, or the post-prefetch PC, mis-classifies the fault as fatal instead of
  demand-loading (§4.6; [lisa.md](lisa.md) §4.8).
- **Data faults are load-bearing, not just fetch faults.** Write-protection detection and
  demand-loaded data segments both depend on the RO-write and invalid-segment faults being
  delivered on data cycles at all (§3.7, §4.6).

## 6. Open questions

1. **How does a terminated MMU cycle reach the CPU as BERR?** The manual documents CAS suppression
   on ACCK/invalid/RO-write ([1] §4.4.3 pp. 4-24 – 4-25) and separately a 30–300 µs bus time-out
   that produces a bus error ([1] §2.7 p. 2-28), but never joins the two: does an MMU violation
   assert the CPU's bus-error line immediately, or does the cycle ride to the time-out? The OS works
   either way; the distinction matters for a cycle-accurate model.
2. **The START latch's electrical polarity.** Figure 2-17's reset/set labels, §4.7.2's garbled
   "START/: used to disable the start mode... It enables access to the MMU RAM" [1] p. 4-36, and
   the ROM's setup-on/setup-off equates name the same latch in three vocabularies (§5). Which LS259
   output sense maps to which strobe is not stated anywhere in prose.
3. **Does a system reset (not POR) re-assert START?** The manual says START is satisfied
   automatically "at power-on time" only [1] p. 2-8, yet the ROM's first act after any reset is
   to read descriptors through the special-I/O window, which requires START to be set. The reset
   path must assert START like POR does (*inferred*), but no document says so.
4. **What happens in a special-I/O cycle with UA16 set?** The ROM/MMUIO split covers UA16 low
   only (§2.4): the LS139 decodes UA15 and UA16 to two outputs, and both ROM/ and MMUIO/ require
   UA16 low [1] §4.7.1 pp. 4-35 – 4-36. A special-I/O cycle with UA16 high — for instance a
   logical `$xx0000`–`$xx7FFF` address whose segment maps `%1111` — matches neither output; what
   the cycle does is not documented.
5. **The exact ROM address bit range.** §2.4 says the ROM address is "bits 1 through 13" [1]
   p. 2-10 (13 lines = 8 K words in each of the two byte-lane devices, [1] §4.3.4 p. 4-18), but
   Figure 2-3's printed format row shows twelve R bits. The OCR of the figure may have lost a bit;
   whether UA0 is a word-select line (byte lanes) or a ROM address line is not stated.
6. **Do the invalid access codes fault or float?** The manual says "unpredictable results will
   occur" for the ten non-assigned codes [1] Fig. 2-4 p. 2-7. None of MEM/IO/RO/STK asserts, so
   CAS is suppressed like the `%1100` code (*inferred — unverified*); whether the cycle faults, and
   with which status, is untested.
7. **Read-modify-write behavior on descriptors.** The ROM's warm-start check uses `ANDI #$0FFF,
   MMU126B` — a read-modify-write *to a descriptor address*. The write-back presumably stores the
   same 12 bits (*inferred — unverified*; the ROM's own logic treats the result as a test), but
   whether an RMW cycle to the descriptor window has write side effects (e.g. on the SOR/SLR split)
   is undocumented.
8. **Byte and longword accesses to the descriptor window.** All documented descriptor accesses are
   word-sized ([2] equates; [3] `move.w`). Whether a byte or longword write reaches one RAM word,
   two adjacent bus cycles, or the SOR and SLR together, is not established.
9. **The power-on value of the context latch.** Nothing documents SEG1/SEG2 at POR; the ROM's
   `CONCHK` test tolerates it only because it compares against values it just wrote. Which table
   `BEGIN`'s very first descriptor reads hit on a cold start is therefore indeterminate.
10. **Which DMA paths, if any, translate through the MMU.** The physical address latch is gated by
    BGACK/ [1] §4.4.3 p. 4-24, so bus masters other than the CPU drive physical addresses; no
    document states whether any expansion-slot device masters the bus in practice, or how.
11. **What the high nibble of a descriptor read-back actually is.** Software masks bits 15–12 as
    undefined ([2], [3]); whether they float, read the data bus's last state, or read the RAM
    column's open lines is not documented (§2.5).
12. **The timing of a faulting cycle's CPU hand-off.** The manual gives MALE/B/L/ timing for the
    good path only ([1] §4.2.2, Figure 4-8); where in the t0–t7 sequence the suppressed-CAS cycle
    signals the CPU is not given.
13. **Who first leaves `$x901` in segment 126.** The boot ROM checks for `IOLMT2` (`$0901`) as its
    no-reset marker and re-writes it on the warm-start path, but the RM248 source contains no
    write of that value on the normal cold-boot path — the marker's original author (a later ROM
    revision, or OS software) is not established (§4.1).
14. **Does interrupt latency differ in START mode?** With setup on, exception vectors are fetched
    from ROM (§3.5); the OS masks interrupts during descriptor programming regardless ([3]
    source-LDASM.TEXT), but no source states what an interrupt taken with setup on does to the
    interrupted context's descriptor programming loop beyond that caution.
15. **MMU behavior with a parity/ECC memory error during the SOR read.** The memory-error path
    latches a physical address ([lisa.md](lisa.md) §6.3) but no document covers the interaction of
    a hard memory error with the MMU's own descriptor fetches.

## References

1. Apple Computer, Inc., *Apple Lisa Computer: Hardware Manual 1983 (with Errata)*, April 1983.
   MMU programming model: ch. 2 §2.3 "Memory Management Scheme" pp. 2-3A – 2-9 (address decode
   Figure 2-1 p. 2-3b; SOR/SLR §2.3.1 pp. 2-4 – 2-5a; limit check Figure 2-2 pp. 2-5a – 2-5b;
   register matrix §2.3.2 pp. 2-5a – 2-6; special-I/O addressing Figure 2-3 p. 2-6; length and
   stack sense p. 2-7; access-control bits Figure 2-4 p. 2-7; initialization §2.3.3 p. 2-8;
   contexts §2.3.4 p. 2-9); §2.4 "Addressing in Special I/O Space" pp. 2-10 – 2-11; processor
   board control §2.5.5 and Figure 2-17 pp. 2-25 – 2-26; exception vector table Figure 2-18
   p. 2-27; error processing §2.7 p. 2-28. Hardware description: ch. 1 pp. 1-5 – 1-8 (address
   spaces, clocks); ch. 4 §4.2.2 "Memory Management Timing" and Figure 4-8 pp. 4-11a, 4-15;
   §4.3.4 bootstrap ROM p. 4-18; §4.4 "The Memory Management Unit" pp. 4-18 – 4-19 with Figure
   4-13; §4.4.1 MMU RAM storage pp. 4-20 – 4-22 with Figure 4-14; §4.4.2 SOR and SLR
   initialization pp. 4-23 – 4-24; §4.4.3 address translation pp. 4-24 – 4-25; §4.4.4 memory
   timing generation p. 4-25; §4.7.1 I/O decode pp. 4-35 – 4-36; §4.7.2 processor board control
   register p. 4-36.
2. Apple Computer, Inc., Lisa Boot ROM assembly source, revision RM248 (officially released
   source listing). Equates: RM248.E.TEXT (SETUP/SETUPON strobes, MMU descriptor addresses
   MMU0L/MMU0B/MMU126L/MMU126B/MMU127L/MMU127B, segment-map limit values MEMLMT/IOLMT/IOLMT2/
   SPLMT/INVPAG, SEG1ON/SEG1OFF/SEG2ON/SEG2OFF, change log incl. "change MMU I/O space code to
   '9'"; BSR4/BSR6/BSR2 register-call macros). MMU bring-up: RM248.K.TEXT (reset vectors and
   ROM-relative ORG, NMIEXCP, BEGIN/BEGIN2 warm-start checks, MMUTST/MMUINIT/MMURW/MMUACHK
   read-write and address tests, CONCHK context check, SETMMU default map, INITMMU invalidation
   of contexts 1–3, START "TURN OFF SETUP TO ENTER MAP LAND").
3. Apple Computer, Inc., *Apple Lisa Operating System* source release (released via the Computer
   History Museum, 2023). OS/source-LDASM.TEXT: INITMMUTIL, PROG_MMU, DO_AN_MMU — the System
   Mapping Table layout, the setup/interrupt dance, stack and two's-complement length conversion,
   descriptor port arithmetic (`slim`/`sorg`/`next_mmu`). OS/source-starasm1.TEXT: SET_DOMAIN
   context strobes, CHANGE_IOSPACE map/unmap of the I/O segment, PROG_MMU trap-6 entry.
   OS/SOURCE-EXCEPASM.TEXT: BUS_ERR demand-load handler, frame symbols (PCX/SRX/B1/B2/BADADDR),
   per-opcode PC adjustments, RTS user-stack rollback, CODE_CHK/CHECK_CS, TST stack-overflow path.
   OS/source-MMPRIM.TEXT: segment-number allocation (mmucodemmu through prommmu) and access-code
   constants; OS/source-DRIVERDEFS.TEXT: iospacemmu. OS/source-SCHED.TEXT: Set_Address_Space,
   SelectDomain, domain LRU.
