# Gossamer family — the beige Power Macintosh G3

`src/machines/gossamer/` implements the beige Power Macintosh G3 platform
("Gossamer"): a PowerPC 750 behind Motorola's MPC106 ("Grackle") host
bridge, and Apple's Heathrow I/O controller as a PCI function.  As on TNT,
`config_t.cpu` is NULL and the ROM's own Open Firmware probes the machine
and builds the device tree on our CPU core; the classic 68k world exists
only as ROM code the NanoKernel's emulator runs.

Models: `pmg3dt` (desktop, 266 MHz 750, 512 KB L2) and `pmg3mt` (mini tower
and Server G3, 300 MHz, 1 MB L2), both on the Rev C logic board and the Rev C
ROM (`$78F57389`, Open Firmware 2.4); the Rev A ROM (`$79D68D63`, 2.0f1)
boots on the same board model.  Hardware facts live in the reference pages
— [g3.md](../../../reference/machines/g3/g3.md) for the family,
[dbdma.md](../../../reference/machines/tnt/dbdma.md) and
[grand-central.md](../../../reference/machines/tnt/grand-central.md) for
the inherited cells — and are cited from there, not restated.

## 1. Board model

| File | What it models |
|---|---|
| `gossamer.c` | The substrate: construction order, bus reset, teardown, the checkpoint stream, the VIA/Cuda, SCC, MESH and SWIM3 glue, the PCI slot table (three slots plus the on-board ATI as slot "F1"), the media hooks. |
| `grackle.c` | The MPC106: its configuration registers behind the little-endian `CONFIG_ADDR`/`CONFIG_DATA` ports, the memory bank registers the RAM decode is rebuilt from, the PCI windows, the board-register page at `$FF000000`, and `machine.grackle`. |
| `heathrow.c` | The I/O controller: its PCI header (`$106B:0010`, BAR0 512 KB), the two-bank interrupt controller, the feature-control and control-block registers, the NVRAM window, the cell decode, `machine.heathrow` and `machine.nvram`. |
| `gossamer_i2c.c` | The Cuda's I2C bus: the DIMMs' SPD EEPROMs (built from the RAM size) and the personality card's ID EEPROM. |
| `gossamer_ata.c` | Heathrow's two ATA cells on the core channel model ([ata.md](../../core/peripherals/ata.md)), the ATAPI back end, `machine.ata`. |
| `gossamer_bmac.c` | The Ethernet cell on the core BMAC model ([bmac.md](../../core/peripherals/bmac.md)): the 16-bit bus edge, the FCR enable and reset, `machine.bmac`. |
| `pmg3dt.c`, `pmg3mt.c` | The profiles: CPU clock, PVR and HID1 straps, the board-register value, RAM options, the ATI identity, the personality card. |

The shared core models carry the rest: `core/cpu/ppc` with the 750 deltas
(L2CR, HID1, THRM, the performance monitor — see
[ppc.md](../../core/cpu/ppc.md)), `core/peripherals/dbdma.c` (thirteen
channels), `davbus.c` in its Screamer face, `scsi_mesh.c`, `scc.c`,
`swim3.c`, and the Rage Pro face of `pci/cards/mach64gx.c`.

### 1.1 The interrupt controller

Two banks of Events / Mask / Clear / Levels.  Before the NanoKernel's first
mode acknowledge (a Clear write with bit 31) a bank's output is
combinational, `(events | levels) & mask`; after it, the bank drives a
latch set by any change of an enabled source and dropped by the
acknowledge — the TNT law.  DBDMA completions are held levels until their
Clear bit is written.  The DBDMA channel number and the interrupt source
decouple on Heathrow: ATA 0/1's channels 11/12 complete on sources 2/3,
BMAC's on bank-2 sources 32/33 (`gos_dbdma_source`).

### 1.2 What the guest needed that was not obvious

- **The SPD read length.**  The boot program reads each DIMM's whole
  EEPROM in one I2C transaction; a short read sent it down the PERCH
  fallback path.
- **Reaching the Open Firmware prompt.**  As on the real machine, the rows
  hold Command-Option-O-F from power-on.  The ADB keyboard re-reports every
  held key after the ROM's bus reset, in press order, and Cuda keeps an
  unclaimed autopoll packet parked until the host takes it; with video
  present the prompt is on the screen, so the rows type `ttya io` on the
  keyboard and talk over SCC channel A from there.
- **MESH holds ACK on the status byte** until the next sequence command, so
  REQ stays low between STATUS and MESSAGE IN; Apple's driver waits for
  exactly that.
- **`mtdec` signals the decrementer** when it turns bit 0 from 0 to 1 —
  the NanoKernel re-posts an expired DEC that way.
- **The Rage Pro's set-up shortcut** `DP_SET_GUI_ENGINE2`, the
  `GUI_STAT` FIFO count and colour host data, all of which ATI's Mac
  accelerator relies on — and the upper half of BAR2, the register
  aperture: the accelerator streams its host data through the second copy
  of the register blocks there, and without it every alert, button and
  menu title drew blank.
- **DBDMA CommandPtr on a parked channel.**  The ATA driver rewrites it
  with RUN set to continue a long transfer.
- **ATA DMA must take time.**  With the device ready at the command write,
  a whole READ DMA — data, INTRQ and the channel's completion interrupt —
  used to finish inside the guest's command-register store.  Mac OS 9.2.1's
  ATA Manager then leaks a critical section (the nesting count of the
  enter/exit pair whose first entry sets bit 14 of `$0160`) once paging
  reads run back to back, and the next page fault is refused as one taken
  at interrupt time: a bus error at "Welcome to Mac OS".  The cells' DBDMA
  port now moves 4 KB per activation, kicked every 10 us while a DMA
  command is pending, the scheme MESH already uses.

## 2. Object-model surface

`machine.grackle` (the bridge's registers), `machine.heathrow` (the
interrupt controller, FCR, MBCR), `machine.nvram` (peek/poke/dump/
snapshot/restore/clear on the 8 KB store), `machine.board`,
`machine.scsi` (MESH), `machine.ata` and `machine.atapi`
([ata.md](../../core/peripherals/ata.md) §4), `machine.bmac`
([bmac.md](../../core/peripherals/bmac.md)), plus the shared `machine.pci`,
`machine.cpu`, `machine.adb`, `machine.screen`.

## 3. Checkpointing

The core stream, ADB, Cuda, DBDMA and the floppy, then the substrate tail:
Grackle's configuration and address latch, the Heathrow block, the I2C
state, the Screamer registers, the PCI bus, the image table, the MESH bus
and chip, SWIM3, the ATAPI bus and the two ATA channels, and last BMAC.  The NVRAM
is part of the Heathrow block; it survives `machine.reset` and
`machine.restart` because neither destroys the machine (the board
battery), and a new machine (`machine.boot`) gets a new
part. The new part holds what the board's own firmware formats
(`src/machines/runtime/of_nvram.c`, the TNT rule in
`docs/internals/machines/tnt/tnt.md`): OF 2.4's variables (Rev C; the Rev A
ROM's OF 2.0f1 accepts the same store, its own format differing only in
`diag-device`) and the ROM's parameter RAM defaults (startup device `$6666`,
"no default"). `machine.nvram.clear` is the battery pull back to that
store. `machine.nvram` also has `getenv` / `setenv` and `startup_disk`
(the Mac OS 9 form, partition byte 1); the depth field is not offered here,
because Mac OS 9 re-applies the depth from its Display Preferences file
shortly after boot.

## 4. Testing

- `gossamer-rom-ladder` — both ROMs from reset to the Start Manager: the
  Grackle and Heathrow programming, the board register, the SPD-sized RAM
  decode, the boot beep against a golden capture, the tick chain, the
  Rage Pro gray desktop.
- `gossamer-device-tree` — the Open Firmware prompt reached from the
  keyboard, and the device tree checked property by property.
- `gossamer-pci-slots` — the three slots and the on-board ATI: empty
  sockets read all-ones, a card in each slot reaches the device tree with
  its interrupt, and Open Firmware assigns its BARs.
- `suite-gossamer` — both models boot Mac OS 9.2.1 from a SCSI disk to the
  desktop (boot drive, no startup system error, the chime and the
  Screamer output frame count, a golden frame), and a 1.44 MB floppy
  mounts on the desktop.
- `gossamer-bmac` — Open Firmware's own BMAC package: the EEPROM's
  station address as `local-mac-address`, receive and transmit through
  DBDMA, loopback set from Forth, the BOOTP request of `boot enet`.
- `gossamer-checkpoint` — a mid-boot save, a restore in a fresh process
  and the same desktop frame.
- Media-gated on the retail Mac OS 9.2.1 CD under `GS_EXTRA_MEDIA_DIR`
  (they skip without it): `g3-cd-boot` (the CD to the Finder as a SCSI
  CD-ROM and as an ATAPI one), `g3-install-scsi` (Drive Setup and the
  installer onto a blank SCSI disk, then that disk booted to the Setup
  Assistant) and `g3-install-ata` (the same install onto an ATA disk, then
  that disk booted from ATA to the Setup Assistant).
- The unit suites `ppc_750`, `scsi_mesh`, `dbdma`, `ata`, `bmac`.

## 5. Known debts

- **BMAC has no wire**: frames go nowhere unless looped back, and only
  `machine.bmac.receive` delivers one.  Mac OS 9 networking over it is
  unexercised.
- **LocalTalk on the SCC through DBDMA** is exercised only as far as the
  boot needs.
- Shut Down from the Finder ends in a system error.

## 6. See also

[tnt.md](../tnt/tnt.md) — the family this one descends from;
[ata.md](../../core/peripherals/ata.md);
[g3.md](../../../reference/machines/g3/g3.md).
