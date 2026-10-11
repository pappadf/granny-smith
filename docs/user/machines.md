# Supported machines

Every model Granny Smith emulates, the ROM it needs, and what you can
configure on it. The **id** column is the name used in links
(`model=iici`, see [URL parameters](url-parameters.md)) and in the
terminal.

## 1. ROMs

A machine starts only with its original ROM. Load ROM files with
**Load ROM...** on the Home screen, by dropping them on the window, or from
a link. Each ROM is recognised by its checksum (the 8-digit number below,
which is also how the Images panel lists it), never by its file name.

| ROM checksum | ROM | Boots |
|---|---|---|
| `4D1EEEE1`, `4D1EEAE1`, `4D1F8172` | Macintosh Plus, revisions 1, 2, 3 | Plus |
| `97221136` | Universal IIx/IIcx/SE/30 ROM | SE/30, IIx, IIcx |
| `368CADFE` | Macintosh IIci | IIci |
| `36B7FB6C` | Macintosh IIsi | IIsi |
| `4147DD77` | Macintosh IIfx | IIfx |
| `420DBFF3` | Quadra 700/900 | Quadra 700, Quadra 900 |
| `3DC27823` | Quadra 950 | Quadra 950 |
| `5BF10FD1` | Quadra 840AV / 660AV | Quadra 840AV, Centris/Quadra 660AV |
| `9FEB69B3` | Power Macintosh 6100/7100/8100 v1.0 | Power Mac 6100, 7100, 8100 |
| `96CD923D`, `9630C68B` | Power Macintosh 7500/8500/9500 v1, v2 | Power Mac 7500, 8500, 9500 |
| `9630C68B`, `962F6C13`, `49B2BE8F` | Apple Network Server (2.26B6, Win NT, 1.1.20.1, 1.1.22, 2.0 prototype) | Network Server 500, 700 |
| `78F57389`, `79D68D63` | Power Macintosh G3 (Rev C, Rev A) | G3 Desktop, G3 Minitower |
| `3F7B` | Lisa 2 Boot ROM, revision H | Lisa 2 |
| `D905` | Macintosh XL Boot ROM 3A | Macintosh XL |

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
| Macintosh SE/30 | `se30` | 8 MB; 1–128 MB | built-in 9″ | 1.4 MB SuperDrive (+ optional external) | SCSI; CD-ROM |
| Lisa 2 | `lisa` | 1 MB; 0.5, 1, 1.5, 2 MB | built-in 12″, 720×364 | one internal drive | ProFile on the parallel port |
| Macintosh XL | `macxl` | 1 MB; 0.5, 1, 1.5, 2 MB | built-in 12″, 720×364 | one internal drive | internal ProFile |

The Lisa's screen has tall pixels; Granny Smith draws them 1.5 times taller
than wide so the picture has the right proportions. The Lisa and XL have no
AppleTalk; their ImageWriter connects to Serial A.

## 3. Macintosh II family

| Model | id | Memory (default; choices) | Video | Floppy | Slots |
|---|---|---|---|---|---|
| Macintosh IIx | `iix` | 8 MB; 1–128 MB | Display Card 8•24 in slot 1 | two internal 1.4 MB | NuBus 1–6 |
| Macintosh IIcx | `iicx` | 8 MB; 1–128 MB | Display Card 8•24 in slot 1 | internal + optional external | NuBus 1–3 |
| Macintosh IIci | `iici` | 8 MB; 1–128 MB | built-in (13″ RGB or 15″ Portrait) | internal + optional external | NuBus 4–6 |
| Macintosh IIsi | `iisi` | 9 MB; 1, 2, 3, 5, 9, 17, 65 MB | built-in (13″, 15″ Portrait, 12″ RGB) | internal + optional external | — |
| Macintosh IIfx | `iifx` | 8 MB; 4–128 MB | Display Card 8•24 in slot 1 | two internal 1.4 MB | NuBus 1–6 |

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

All have a 1.4 MB SuperDrive and SCSI (the 900 and 950 also an external
SCSI bus). The Quadra 700/900/950 start in 24-bit mode, the AV models in
32-bit mode. The AV models also have a video input and a sound input that
you can connect to your camera and microphone — see
[Sound, camera and 3D](sound-and-video.md).

## 5. Power Macintosh

| Model | id | Memory (default; choices) | Video | Storage | Slots |
|---|---|---|---|---|---|
| Power Mac 6100 | `pm6100` | 16 MB; 8–72 MB | built-in (13″, 15″ Portrait, 12″) | SCSI hard disk, CD-ROM | — |
| Power Mac 7100 | `pm7100` | 24 MB; 8–136 MB | built-in | SCSI: two hard disks, CD-ROM | NuBus B–D |
| Power Mac 8100 | `pm8100` | 32 MB; 8–264 MB | built-in | SCSI, CD-ROM | NuBus B–D |
| Power Mac 7500 | `pm7500` | 32 MB; 8–512 MB | built-in (12″–21″) | internal SCSI bays | PCI A1–C1 |
| Power Mac 8500 | `pm8500` | 32 MB; 16–512 MB | built-in | internal SCSI bays | PCI A1–C1 |
| Power Mac 9500 | `pm9500` | 64 MB; 16–512 MB | PCI graphics card in A1 | internal SCSI bays | PCI A1–F2 |
| Power Mac G3 Desktop | `pmg3dt` | 64 MB; 32–768 MB | built-in ATI Rage Pro | SCSI hard disk + CD-ROM, two ATA buses | PCI A1–C1 |
| Power Mac G3 Minitower | `pmg3mt` | 64 MB; 32–768 MB | built-in ATI Rage Pro | as the Desktop | PCI A1–C1 |

Power Macs always run in 32-bit mode. The 7500, 8500 and 9500 have no CD-ROM
drive in their default configuration. PCI cards you can add: the **Apple
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
| Quadra 700 / 900 / 950 | System 7.5 – 7.6; System 7.1 on the Quadra 700 |
| Quadra 840AV / 660AV | System 7.1 with its enabler, 7.6 (840AV) |
| Power Mac 6100 / 7100 / 8100 | System 7.5 – 7.6, Mac OS 8.1 – 9.0.4; MkLinux DR3 |
| Power Mac 7500 / 8500 / 9500 | System 7.6, Mac OS 8.1; MkLinux DR3 (7500) |
| Power Mac G3 | Mac OS 9.2.1 (install from CD and boot) |
| Network Server 500 | AIX 4.1.5, Windows NT 4.0 for PowerPC, Network Server Diagnostic Utility |
| Lisa 2 | Lisa Office System 3.1, Lisa Workshop, SCO Xenix 3.0 |
| Macintosh XL | MacWorks XL 3.0 |

Notes:

- A PCI Power Mac (7500/8500/9500) refuses a System 7.5 startup disk; use
  7.6 or later.
- System 7.6 and later on the 68K Macs that start in 24-bit mode: set
  **Addressing** to 32-bit (see §3).
- MacWorks XL starts from its loader floppy, then asks for the MacWorks
  system disk.
