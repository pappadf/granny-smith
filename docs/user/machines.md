# Supported machines

Every model Granny Smith emulates, the ROM it needs, and what you can
configure on it. The **id** column is the name used in links
(`model=iici`, see [URL parameters](url-parameters.md)) and in the
terminal.

## 1. ROMs

A machine starts only with its original ROM. Load ROM files with
**Load ROM...** on the Home screen, by dropping them on the window, or from
a link. Each ROM is recognised by its checksum (the 8-digit number below),
never by its file name. The Images panel lists a ROM by an id that starts
with this checksum in lower case and continues with a second, longer one
(`9feb69b3-dedc602cfc3b9221` for `9FEB69B3`).

| ROM checksum | ROM | Boots |
|---|---|---|
| `4D1EEEE1`, `4D1EEAE1`, `4D1F8172` | Macintosh Plus, revisions 1, 2, 3 | Plus, 512K enhanced |
| `97221136` | Universal IIx/IIcx/SE/30 ROM | SE/30, IIx, IIcx |
| `368CADFE` | Macintosh IIci | IIci |
| `36B7FB6C` | Macintosh IIsi | IIsi |
| `35C28F5F` | Macintosh LC II | LC II |
| `3193670E` | Macintosh Classic II | Classic II |
| `ECD99DC0` | Macintosh Color Classic | Color Classic |
| `ECBBC41C`, `EC904829` | Macintosh LC III (later, early) | LC III, LC III+ |
| `EDE66CBD` | LC 520/550, Color Classic II, Macintosh TV | LC 520, LC 550, Color Classic II |
| `4147DD77` | Macintosh IIfx | IIfx |
| `420DBFF3` | Quadra 700/900 | Quadra 700, Quadra 900 |
| `3DC27823` | Quadra 950 | Quadra 950 |
| `5BF10FD1` | Quadra 840AV / 660AV | Quadra 840AV, Centris/Quadra 660AV |
| `9FEB69B3` | Power Macintosh 6100/7100/8100 v1.0 | Power Mac 6100, 7100, 8100 |
| `9B7A3AAD` | Power Macintosh 7100 v1.1 | Power Mac 7100 |
| `96CD923D`, `9630C68B` | Power Macintosh 7500/8500/9500 v1, v2 | Power Mac 7200, 7500, 8500, 9500, 7600 |
| `960E4BE9` | Power Macintosh 7300/7600/8600/9600 (1997 v1) | Power Mac 7300, 7600, 8600, 9600 |
| `960FC647` | Power Macintosh 8600/9600 (1997 v2) | Power Mac 8600, 9600 |
| `9C7C98F7` | Workgroup Server 9150/80 | Workgroup Server 9150/80 |
| `9B037F6F` | Workgroup Server 9150/120 | Workgroup Server 9150/120 |
| `9630C68B`, `962F6C13`, `49B2BE8F` | Apple Network Server (2.26B6, Win NT, 1.1.20.1, 1.1.22, 2.0 prototype) | Network Server 500, 700 |
| `78F57389`, `79D68D63` | Power Macintosh G3 (Rev C, Rev A) | G3 Desktop, G3 Minitower |
| `3F7B` | Lisa 2 Boot ROM, revision H | Lisa 2 |
| `28BA61CE`, `28BA4E50` | Macintosh 128K/512K 64 KB ROM, revisions 1, 2 | 128K, 512K |
| `B2E362A8` | Macintosh SE | SE |
| `B306E171` | Macintosh SE FDHD | SE FDHD |
| `A49F9914` | Macintosh Classic | Classic |
| `D905` | Macintosh XL Boot ROM 3A | Macintosh XL |
| `F1A6F343` | Centris 610/650 and Quadra 800 (February 1993) | Centris 610, Centris 650, Quadra 610, Quadra 650, Quadra 800 |
| `F1ACAD13` | Quadra 610/650/800 and later Centris 610/650 | Centris 610, Centris 650, Quadra 610, Quadra 650, Quadra 800 |

Where one ROM file serves several models, the New Machine dialog lists each
model separately, and a ROM revision appears in the model's name
("Power Macintosh 7500 (v2)", "Apple Network Server 500 (Win NT)").

Display cards on NuBus Macs have their own small *declaration ROM*. If you
have not loaded the card's Apple ROM, Granny Smith uses a substitute ROM of
its own and the card still works (the dialog marks it "substitute"). The
PCI graphics card of the Power Mac 7500/8500/9500 needs its real option
ROM; without it the card is unavailable and the machine falls back to its
built-in video where it has one.

## 2. Compact and Lisa machines

| Model | id | Memory (default; choices) | Screen | Floppy | Hard disk |
|---|---|---|---|---|---|
| Macintosh Plus | `plus` | 4 MB; 1, 2, 2.5, 4 MB | built-in 9″, 512×342 black and white | 800K internal + 800K external | SCSI (external connector); CD-ROM possible |
| Macintosh 128K | `mac128k` | 128 KB | built-in 9″, 512×342 black and white | 400K internal (+ optional 400K external) | — |
| Macintosh 512K | `mac512k` | 512 KB | built-in 9″, 512×342 black and white | 400K internal (+ optional 400K external) | — |
| Macintosh 512K enhanced | `mac512ke` | 512 KB | built-in 9″, 512×342 black and white | 800K internal (+ optional 800K external) | — |
| Macintosh SE | `se` | 4 MB; 1, 2, 2.5, 4 MB | built-in 9″, 512×342 black and white | two internal 800K | SCSI; CD-ROM possible |
| Macintosh SE FDHD | `sefdhd` | 4 MB; 1, 2, 2.5, 4 MB | built-in 9″, 512×342 black and white | two internal 1.4 MB SuperDrives | SCSI; CD-ROM possible |
| Macintosh Classic | `classic` | 4 MB; 1, 2, 2.5, 4 MB | built-in 9″, 512×342 black and white | 1.4 MB SuperDrive (+ optional external) | SCSI; CD-ROM possible |
| Macintosh SE/30 | `se30` | 8 MB; 1–128 MB | built-in 9″ | 1.4 MB SuperDrive (+ optional external) | SCSI; CD-ROM |
| Lisa 2 | `lisa` | 1 MB; 0.5, 1, 1.5, 2 MB | built-in 12″, 720×364, tall pixels | one internal drive | ProFile on the parallel port |
| Macintosh XL | `macxl` | 1 MB; 0.5, 1, 1.5, 2 MB | built-in 12″, 608×431, square pixels | one internal drive | internal ProFile |
| Macintosh Classic II (Performa 200) | `classicii` | 10 MB; 2, 4, 6, 10 MB | built-in 9″, 512×342 black and white | 1.4 MB SuperDrive + optional external | SCSI; CD-ROM |
| Macintosh Color Classic (Performa 250) | `colorclassic` | 10 MB; 4, 6, 8, 10 MB | built-in 10″, 512×384 colour (up to 256 colours) | 1.4 MB SuperDrive | SCSI; CD-ROM |
| Macintosh Color Classic II (Performa 275) | `colorclassicii` | 8 MB; 4, 5, 6, 8, 12, 20, 36 MB | built-in 10″, 512×384 colour (up to 256 colours) | 1.4 MB SuperDrive | SCSI; CD-ROM |
| Macintosh LC 520 (Performa 520) | `lc520` | 8 MB; 4, 5, 6, 8, 12, 20, 36 MB | built-in 14″, 640×480 colour (up to 256 colours) | 1.4 MB SuperDrive | SCSI; CD-ROM |
| Macintosh LC 550 (Performa 550, 560) | `lc550` | 8 MB; 4, 5, 6, 8, 12, 20, 36 MB | built-in 14″, 640×480 colour (up to 256 colours) | 1.4 MB SuperDrive | SCSI; CD-ROM |

The 128K, 512K and 512K enhanced have no SCSI port: they start from a
floppy only. The 128K and 512K read single-sided 400K disks (System 1 to
about System 3.2 on the 128K, up to System 4.x on the 512K); the 512K
enhanced has the Plus's ROM and 800K drive. The 128K to Plus use the
original keyboard and mouse; the SE, SE FDHD and Classic use ADB. To start
a Classic from the System 6.0.3 disk built into its ROM, hold
Command-Option-X-O while it starts up.

The Lisa 2's screen has tall pixels; Granny Smith draws them 1.5 times taller
than wide so the picture has the right proportions. The Macintosh XL has the
screen modification MacWorks XL expects: 608×431 with square pixels. The
Lisa and XL have no AppleTalk setting: Serial B, the port the Lisa was built
to use for AppleBus, is always on the emulated AppleTalk network, for
software that drives it as LocalTalk. Their ImageWriter connects to Serial A.

A Lisa or XL starts in **Real** (every other machine starts in **Faster**).
Keep it in **Real** or **Max**, and switch to **Faster** only once it has
left the boot ROM's self-test. The self-test times the screen's
retrace with a loop sized for the original processor; a faster one finishes
the loop early and stops with CPU error 42, as an accelerated real Lisa
would.

## 3. Macintosh II family

| Model | id | Memory (default; choices) | Video | Floppy | Slots |
|---|---|---|---|---|---|
| Macintosh IIx | `iix` | 8 MB; 1–128 MB | Display Card 8•24 in slot 1 | two internal 1.4 MB | NuBus 1–6 |
| Macintosh IIcx | `iicx` | 8 MB; 1–128 MB | Display Card 8•24 in slot 1 | internal + optional external | NuBus 1–3 |
| Macintosh IIci | `iici` | 8 MB; 1–128 MB | built-in (13″ RGB or 15″ Portrait) | internal + optional external | NuBus 4–6 |
| Macintosh IIsi | `iisi` | 9 MB; 1, 2, 3, 5, 9, 17, 65 MB | built-in (13″, 15″ Portrait, 12″ RGB) | internal + optional external | — |
| Macintosh IIfx | `iifx` | 8 MB; 4–128 MB | Display Card 8•24 in slot 1 | two internal 1.4 MB | NuBus 1–6 |
| Macintosh LC III (Performa 450) | `lciii` | 8 MB; 4, 5, 6, 8, 12, 20, 36 MB | built-in (13″ RGB 640×480 or 12″ RGB 512×384, up to 256 colours) | one internal 1.4 MB | — (PDS empty) |
| Macintosh LC III+ (Performa 460–467) | `lciiip` | 8 MB; 4, 5, 6, 8, 12, 20, 36 MB | built-in (13″ RGB 640×480 or 12″ RGB 512×384, up to 256 colours) | one internal 1.4 MB | — (PDS empty) |
| Macintosh LC II (Performa 400–430) | `lcii` | 10 MB; 4, 6, 8, 10 MB | built-in (13″ RGB 640×480 up to 16 colours, or 12″ RGB 512×384 up to 256) | one internal 1.4 MB | — (PDS empty) |

All have SCSI with a hard disk at ID 0 and a CD-ROM drive at ID 3 by
default. Cards you can add: **Macintosh Display Card 8•24**, **Macintosh
Display Card 24AC** and **Macintosh Display Card 8•24 GC**.

**Addressing.** These machines start in 24-bit mode (as System 6 needs).
Mac OS 7.6 and later need 32-bit: choose **32-bit** under **Addressing**
(or `addressing=32` in a link), otherwise the system switches itself and
restarts. The SE/30, IIx and IIcx ROMs are not 32-bit clean, so 32-bit mode
on them also needs MODE32 installed on the startup disk.

## 4. Quadra and Centris

| Model | id | Memory (default; choices) | Video | Slots |
|---|---|---|---|---|
| Quadra 700 | `q700` | 20 MB; 4, 8, 20, 36, 68 MB | built-in; 12″–21″ monitors | NuBus D, E |
| Quadra 900 | `q900` | 16 MB; 4–256 MB | built-in; 12″–21″ monitors | NuBus A–E |
| Quadra 950 | `q950` | 16 MB; 4–256 MB | built-in; 12″–21″ monitors | NuBus A–E |
| Quadra 840AV | `q840av` | 16 MB; 8–128 MB | built-in 13″ RGB | — |
| Centris 660AV / Quadra 660AV | `q660av` | 16 MB; 4–68 MB | built-in 13″ RGB | — |
| Centris 610 | `c610` | 8 MB; 4–68 MB | built-in; 12″–21″ monitors | one NuBus card (adapter, slot E) |
| Centris 650 | `c650` | 16 MB; 8–136 MB | built-in; 12″–21″ monitors | NuBus C, D, E |
| Quadra 610 | `q610` | 8 MB; 4–68 MB | built-in; 12″–21″ monitors | one NuBus card (adapter, slot E) |
| Quadra 650 | `q650` | 16 MB; 8–136 MB | built-in; 12″–21″ monitors | NuBus C, D, E |
| Quadra 800 | `q800` | 16 MB; 8–136 MB | built-in; 12″–21″ monitors | NuBus C, D, E |

All have a 1.4 MB SuperDrive and SCSI (the 900 and 950 also an external
SCSI bus). The Quadra 700/900/950 start in 24-bit mode, the AV models in
32-bit mode. The AV models also have a video input and a sound input that
you can connect to your camera and microphone — see
[Sound, camera and 3D](sound-and-video.md).

The Centris 610/650 and Quadra 610/650/800 run either of their two ROMs.
The Centris 610 has a 68LC040, which has no floating-point unit. These
machines shipped with System 7.1 and **System Enabler 040**: a System 7.1
disk without that enabler stops with "This startup disk will not work on
this Macintosh model". System 7.5 and later start them without an enabler.

## 5. Power Macintosh

| Model | id | Memory (default; choices) | Video | Storage | Slots |
|---|---|---|---|---|---|
| Power Mac 6100 | `pm6100` | 16 MB; 8–72 MB | built-in (13″, 15″ Portrait, 12″) | SCSI hard disk, CD-ROM | — |
| Power Mac 7100 | `pm7100` | 24 MB; 8–136 MB | built-in | SCSI: two hard disks, CD-ROM | NuBus B–D |
| Power Mac 8100 | `pm8100` | 32 MB; 8–264 MB | built-in | SCSI, CD-ROM | NuBus B–D |
| Power Mac 7200 | `pm7200` | 16 MB; 8–256 MB | built-in, 1 MB VRAM (12″–21″) | one SCSI bus: hard disk, CD-ROM, external | PCI A1–C1 |
| Power Mac 7500 | `pm7500` | 32 MB; 8–512 MB | built-in (12″–21″) | internal SCSI bays; CD-ROM | PCI A1–C1 |
| Power Mac 8500 | `pm8500` | 32 MB; 16–512 MB | built-in | internal SCSI bays; CD-ROM | PCI A1–C1 |
| Power Mac 9500 | `pm9500` | 64 MB; 16–512 MB | PCI graphics card in A1 | internal SCSI bays; CD-ROM | PCI A1–F2 |
| Power Mac 7300 | `pm7300` | 32 MB; 16–512 MB | built-in | internal SCSI bays; CD-ROM | PCI A1–C1 |
| Power Mac 7600 | `pm7600` | 32 MB; 16–512 MB | built-in | internal SCSI bays; CD-ROM | PCI A1–C1 |
| Power Mac 8600 | `pm8600` | 32 MB; 16–512 MB | built-in | internal SCSI bays; CD-ROM | PCI A1–C1 |
| Power Mac 9600 | `pm9600` | 64 MB; 16–512 MB | PCI graphics card in A1 | internal SCSI bays; CD-ROM | PCI A1–F2 |
| Workgroup Server 9150/80 | `wgs9150` | 32 MB; 8–264 MB | built-in | SCSI, CD-ROM | NuBus A–D |
| Workgroup Server 9150/120 | `wgs9150_120` | 32 MB; 8–264 MB | built-in | SCSI, CD-ROM | NuBus A–D |
| Power Mac G3 Desktop | `pmg3dt` | 64 MB; 32–768 MB | built-in ATI Rage Pro (13″, 15″/17″/20″ multiple-scan, 21″) | SCSI hard disk + CD-ROM; two ATA buses (hard disks, ATAPI CD-ROM) | PCI A1–C1 |
| Power Mac G3 Minitower | `pmg3mt` | 64 MB; 32–768 MB | built-in ATI Rage Pro | as the Desktop | PCI A1–C1 |

Power Macs always run in 32-bit mode. The 7300/7600/8600/9600 are the
7500/8500/9500 with a faster processor (they run as a 604, not a 604e, and
the 9600 as a single-processor machine). The Workgroup Server 9150 is the
8100 with a fourth NuBus slot. The 7200 is the 7500's entry-level sibling:
a 90 MHz 601, built-in video with 1 MB of video memory (thousands of colours
at 640×480), and a single SCSI bus for the hard disk, the CD-ROM drive and
the external connector. The 7500, 8500 and 9500 (and their 1997 successors) have their
CD-ROM drive at ID 3 on a second SCSI bus, the one that also goes to the
external connector; the hard disk bays are on the fast internal bus. PCI cards you can add: the **Apple
Accelerated PCI Graphics Card** (ATI Mach64 GX) and the **3dfx Voodoo2** 3D
card.

## 6. Apple Network Server

| Model | id | Memory (default; choices) | Video | Storage | Slots |
|---|---|---|---|---|---|
| Network Server 500 | `ans500` | 64 MB; 16–512 MB | built-in VGA | two SCSI buses; CD-ROM in bay 0, hard disk in bay 2 | PCI 1–6 |
| Network Server 700 | `ans700` | 64 MB; 16–512 MB | built-in VGA | as the 500, plus two rear bays | PCI 1–6 |

Extra options: **keyswitch** (unlocked, service, locked) and, on the 700,
the number of **power supplies**. Which operating system the server runs is
decided by the ROM you choose (Mac OS, AIX or Windows NT).

## 7. System software known to run

Combinations the project tests regularly. Others may well work too.

| Machines | System software |
|---|---|
| Macintosh Plus | System 2.0 – 4.2, System 6.0.8, System 7.0, 7.1 (and 7.5 from a hard disk) |
| SE/30, IIx, IIcx | System 6.0.x – 7.5; A/UX 3.0.1 (SE/30, IIx) |
| IIci, IIsi | System 6.0.8 – 7.5, 7.6 (IIci); A/UX 3.0.1 (IIci) |
| IIfx | System 6.0.8 – 7.6; A/UX 3.0.1 with X11; Mac OS 8.1 |
| LC II, Classic II | System 7.0.1, 7.1; 7.5.3 (LC II) |
| Color Classic | System 7.5.3, 7.6 (7.1 needs the machine's System Enabler) |
| LC III, LC III+, LC 520, LC 550, Color Classic II | System 7.5.3, 7.6 (7.1 needs the machine's System Enabler) |
| Quadra 700 / 900 / 950 | System 7.5 – 7.6; System 7.1 on the Quadra 700 |
| Quadra 840AV / 660AV | System 7.1 with its enabler, 7.6 (840AV) |
| Power Mac 6100 / 7100 / 8100 | System 7.5 – 7.6, Mac OS 8.1 – 9.0.4; MkLinux DR3 |
| Power Mac 7200 | System 7.5.3, 7.6, Mac OS 8.1 |
| Power Mac 7500 / 8500 / 9500 | System 7.6, Mac OS 8.1; MkLinux DR3 (7500) |
| Power Mac 7300 / 7600 / 8600 / 9600 | Mac OS 8.1 (1997 ROMs); System 7.6 (7600 with the 1995 ROM) |
| Workgroup Server 9150 | System 7.5.3, 7.6 |
| Power Mac G3 | Mac OS 9.2.1 (install from CD and boot); Mac OS X 10.0.3 (from ATA) |
| Network Server 500 | AIX 4.1.5, Windows NT 4.0 for PowerPC, Network Server Diagnostic Utility |
| Lisa 2 | Lisa Office System 3.1, Lisa Workshop, SCO Xenix 3.0 |
| Macintosh XL | MacWorks XL 3.0 |

Notes:

- A PCI Power Mac (7500/8500/9500) refuses a System 7.5 startup disk; use
  7.6 or later. The 7200 starts System 7.5.2 or later (7.5.3 is tested).
- With the 1997 ROMs (`960E4BE9`, `960FC647`) the 7300/7600/8600/9600 also
  refuse System 7.6; use Mac OS 8 or later (7.6.1 is not tested). The
  Workgroup Server 9150 refuses System 7.5; use 7.5.3 or later.
- System 7.6 and later on the 68K Macs that start in 24-bit mode: set
  **Addressing** to 32-bit (see §3).
- MacWorks XL starts from its loader floppy, then asks for the MacWorks
  system disk.
- Mac OS X (10.0.3, 10.1) starts on the Power Mac G3 (Rev C ROM) from an
  ATA disk: in New Machine, put the disk on an ATA bus and choose its
  Mac OS X volume in **Start up from** (it is chosen for you when the disk
  has one). Like a real beige G3, Open Firmware needs Apple's Mac OS X
  patch in NVRAM to load Mac OS X; every new G3 starts with it, as one
  that has run Mac OS X does, so a disk installed on another machine (a
  converted QCOW2 image) needs nothing typed at the Open Firmware prompt.
  Mac OS X does not start from a SCSI disk yet: Open Firmware's SCSI
  driver cannot read one in the emulator.
