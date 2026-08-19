# BART — the PDM NuBus bridge (`bart.c`)

BART is Apple's NuBus '90 controller for the first-generation Power
Macintosh family: the bridge between the 601's 64-bit bus and the 32-bit,
10 MHz NuBus.  On the 7100 and 8100 it sits on the logic board; on the 6100
it exists only on the optional PDS "NuBus adapter" card, which this
emulator does not offer — so `pm6100` presents a machine with *no* bridge,
and the ROM's runtime probe finds that out (§4).

Sources: Apple, *Power Macintosh Computers* Developer Note (1994); Apple,
*Designing Cards and Drivers for the Macintosh Family*, 3rd ed. (1992),
ch. 7–8; the Power Macintosh 8100 schematic set (051-0333 rev A, sheets
22–23); and the shipping 1994-03 ROM, whose `NuBusReset`, `TestForBart`
and `_HWPriv` selector 12 are the behavioral oracle.

## Topology — the slots are `$B`/`$C`/`$D`

The 7100 and 8100 carry three NuBus connectors, numbered **`$B`, `$C`,
`$D`** — not the widely repeated "`$C`/`$D`/`$E`".  Slot `$E` is the *PDS
video* pseudo-slot (the HPV VRAM card or the AV card), which claims its
window on the CPU bus directly; the ROM disables BART's own path to slot
`$E` on every 7100/8100 boot.  Physical board order is B, D, C: the middle
connector is `$D`.

`pm7100.c` / `pm8100.c` declare the three sockets; `pm6100.c` declares
none.  Which cards fit a socket is computed from the card registry, not
listed per machine (`nubus_card_fits_socket`).

## What software sees

### Registers (`$F0000000`, byte-wide)

| Offset | Name | Behavior |
|---|---|---|
| `$00` | reset | write `$80` = pulse NuBus `/RESET`; the model fans it out to every seated card (`nubus_reset`).  A *read* is the presence probe — that it completes at all is the whole signal. |
| `$01` | slow | wait-state bit; a latch (the shipping ROM never writes it) |
| `$08` | ID | longword read.  First-revision silicon returns `$43184000`, which is the trigger for a prototype-only interrupt-line swap — so this model deliberately returns something else. |
| `$11` | slot `$E` disable | write `$80` = BART's slot-`$E` decode and its interrupt output off.  A latch here: slot `$E` belongs to the PDS on these boards. |
| `$80 − 8·(n−1)` | burst enable, slot *n* = 1…14 | bit 0 per slot, set by the Slot Manager for cards that declare slave block transfers.  Bursts and single beats are indistinguishable to software, so these are latches. |

Anything else in the register page faults: the chip's decode granularity
above `$87` is unknown, so the window is kept minimal.

### Windows

| Range | Meaning |
|---|---|
| `$Fs000000-$FsFFFFFF` | standard slot space, 16 MB per slot |
| `$s0000000-$sFFFFFFF` | super slot space, 256 MB per slot |

Both are claimed for every declared connector, plus the slot-`$E` standard
window for the PDS.  Each is registered as an *empty* window before
`nubus_init` runs; a seated card's own regions then overlay the pages it
answers, and everything left over faults.

## Faults are the contract

Four places must fault, and the ROM depends on each:

1. **An empty slot.**  The Slot Manager goes looking for a valid
   `ByteLanes` byte at the top of the slot, under a bus-error catcher
   inside the 68k emulator, and records an empty slot when the read
   faults — in practice one byte at `$FsFFFFFF` per slot, descending, and
   then it moves on.  Returning `$FF` instead would make every empty slot
   look like a *broken card*.
2. **The "VidReset" probe.**  Before the Slot Manager runs, every boot the
   Start Manager reads slot `$E`'s declaration ROM at `$FEFFFF00` looking
   for that ASCII signature (it resets an HPV/AV card early).  With no PDS
   card, those reads must fault this early in the boot.
3. **A 6100 with no adapter.**  `tst.b $F0000000` must fault so the ROM
   clears its `BARTExists` flag — and the machine must still boot, with
   zero NuBus slots.
4. **Anything else BART claims but nobody answers** — the same recoverable
   transfer error.

The delivery path: the device window latches the fault
(`memory_signal_bus_error`), the 601 seam takes it at the sprint boundary
as a machine check, and the nanokernel reflects it into the 68k emulator
as an ordinary 68k bus error — which is exactly what the Slot Manager's
catcher is there for.  Addresses *outside* every BART window are decoded
by nobody; AMIC's 40 µs error there is documented as unrecoverable
("forces restart"), so this model leaves them reading `$FF` rather than
faulting: nothing in the ROM depends on that path, and a recoverable fault
would be the wrong kind of failure.

## Interrupts belong to AMIC, not BART

Each connector's `/NMRQ` runs from the slot straight to an AMIC pin; BART
is not in the path.  The Slot Manager reads the lines from AMIC's
pseudo-VIA2 slot bank at `$50F26002`, **active low**: bit 2 = `$B`, bit 3 =
`$C`, bit 4 = `$D`, bit 5 = `$E`, bit 6 = the built-in video VBL.  The bus
controller's per-slot hook (`machine_substrate_t.nubus_slot_irq`) therefore
lands in `pdm_bart_slot_irq`, which forwards to `pdm_amic_set_slot_irq`.
See amic.md for the enable/aggregate side.

A note on which screen the guest picks: with a 24AC in slot `$C` the desktop
stays on the built-in video and the card comes up as screen two.  That is a
consequence of the built-in *monitor*, not of slot priority — `ariel.c`
straps a 14" Hi-Res monitor onto the HDI-45, and the ROM allocates the
604 KB framebuffer only when it senses one (video.md).  A PDM with nothing
plugged into the built-in port allocates no framebuffer at all, and a card
is then the machine's only screen; the emulator has no way to configure
that yet.

The whole path is exercised by a 24AC in slot `$C`: its VBL raises `/NMRQ`,
the flag reaches the 601 through AMIC's interrupt control register, the
nanokernel reflects the interrupt into the 68k emulator, the level-2 RBV
dispatch runs the card's driver, and the driver's acknowledgement drops the
line — once per frame, all boot long.

## Not modeled

Card DMA (a NuBus master reaching main memory through BART), BART 21's
burst reads, the `_HWPriv` per-slot cacheability selectors, the 6100 PDS
adapter, and any PDS video card in slot `$E`.  Every one of those faults or
no-ops exactly as it does without the feature; no guest path this family
runs depends on them.
