# Gallery

A tour of Granny Smith running real Macintosh software, and the tooling around it.

> [Try Granny Smith yourself →](https://pappadf.github.io/gs-pages/latest/)

---

## Display Card 24AC (Macintosh IIcx) with Norton System Info

![System Info on a IIcx with the Display Card 24AC, ready to run the Display benchmark](docs/assets/24ac_sysinfo.png)

System Info benchmark from Norton Utilities on a Macintosh IIcx fitted with the Apple Macintosh Display Card 24AC, the Radius-built NuBus colour card, emulated with its hardware QuickDraw accelerator.

---

## Display Card 8•24 GC (Macintosh IIcx) with Norton System Info

![System Ratings window showing the emulated 8•24 GC](docs/assets/24gc_sysinfo.png)

The System Ratings verdict for the Macintosh Display Card 8•24 GC with QuickDraw acceleration modelled: the emulated card draws 2373 rectangles/sec, well clear of the Quadra 700 reference system at 649.

---

## Filesystem Browser

![Filesystem panel browsing host OPFS and guest HFS/UFS volumes](docs/assets/fs_browser.png)

The Filesystem panel unifies host and guest storage into a single tree, so disk images open up like directories and you can descend straight from a host folder into the volumes inside an image. The host side covers both OPFS (the browser's Origin Private File System) and the in-memory MemFS; expanding `hd160-with-aux-301.img` walks the disk image in place, through the HFS volume (`MacPartition`, with its System Folder, A/UX Startup, and Trash) and the A/UX UFS partitions (`bin`, `dev`, `etc`, `init.d`, …). Any file can be copied straight out of either tree.

---

## Integrated Debugger (Macintosh IIfx) with A/UX

![Integrated debugger with disassembly, register file, and breakpoints alongside a live A/UX desktop](docs/assets/iifx_aux3.png)

The full integrated debugger attached to a Macintosh IIfx running A/UX. The left half is the live machine, with Control Panels, a CommandShell, and the A/UX System Console; the right pane is a live disassembly view that tracks the program counter instruction by instruction, a register file that highlights the values changed by the last step, and inline breakpoints.

---

## Macintosh SE/30 booting A/UX 3.0.1

![Animated SE/30 cold boot into A/UX 3.0.1](docs/assets/aux_boot.gif)

A full SE/30 cold boot into A/UX 3.0.1, captured end to end: from the Happy Mac through the UNIX kernel bring-up to the Finder desktop sitting on top of a System V UNIX userland.

---

## Macintosh IIfx with MacTest 1.0

![MacTest 1.0 identifying a Macintosh IIfx in 256 colours](tests/integration/iifx-mactest/main.png)

Apple's MacTest 1.0 identifying a Macintosh IIfx (System 6.0.5, a 68030 CPU with a 68882 FPU, 512 KB ROM, and 16 MB RAM), with the hardware-identification panel rendered in 256 colours (8 bpp on the JMFB display card).

---

## Macintosh SE/30 with the A/UX 3.0.1 Installer

![A/UX Easy Install dialog](tests/integration/se30-aux-3/installer.png)

The A/UX 3.0.1 Installer dialog, booted from an A/UX boot floppy with the installer CD-ROM mounted as the root filesystem. From here the installer lays down a MacPartition, the A/UX startup files, and the standard system software onto a SCSI hard disk.

---

## Macintosh SE/30 with the A/UX 3.0.1 Desktop

![A/UX 3.0.1 Finder desktop](tests/integration/se30-aux3-boot/desktop.png)

The A/UX 3.0.1 Finder desktop after a clean boot from an installed HD image. A/UX presents itself as a familiar Mac desktop on top of a System V Release 2 UNIX kernel, and `MacPartition` is the HFS volume.

---

## Macintosh SE/30 with the A/UX 3.0.1 CommandShell

![A/UX CommandShell with directory listing](tests/integration/se30-aux3-boot/shell.png)

A/UX's CommandShell launches in a regular Mac window, mixing the classic toolbox with a real UNIX userland.

---

## Macintosh SE/30 with MacTest

![MacTest SE/30 main screen after reboot](tests/integration/se30-mactest/main-after-reboot.png)

Apple's internal MacTest SE/30 diagnostic suite reporting "Tous les tests sélectionnés ont réussi", meaning every selected hardware test passed. This exercises the VIA, SCSI, IWM, sound, and logic-board paths against Granny Smith's emulated hardware.

---

## Macintosh Plus with MacTest Rev 7.0

![Service MacTest Rev 7.0 on Mac Plus](tests/integration/plus-mactest/mactest-success.png)

The classic MacTest Rev 7.0 running on an emulated 4 MB Macintosh Plus. The full suite (logic board, RAM, video, IWM, sound) completes with a green PASS, validating the 68000 core and Plus-era peripheral emulation.

---

## Macintosh SE/30 with Apple HD SC Setup

![Apple HD SC Setup formatting a blank disk](tests/integration/se30-format-hd/ready.png)

Apple HD SC Setup v7.0.1 ready to initialize a freshly created blank 80 MB SCSI image.
