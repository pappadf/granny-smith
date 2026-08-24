# Gallery

Demos of Granny Smith running real Macintosh (and Lisa) software. Click any thumbnail to watch on YouTube.

> [Try Granny Smith yourself →](https://pappadf.github.io/gs-pages/latest/)

---

## Booting Copland (Mac OS 8) on an Emulated Power Macintosh 7100

[![Copland D11E4 booting to the Finder desktop on an emulated Power Macintosh 7100](https://i.ytimg.com/vi/BelSAvdV2A0/hq720.jpg)](https://www.youtube.com/watch?v=BelSAvdV2A0)

Apple's Copland — the next-generation "Mac OS 8" that was cancelled before it shipped — booting to the Finder desktop on an emulated Power Macintosh 7100. This is the D11E4 developer release, running unmodified: the emulated 7100 brings up Copland's microkernel, mounts the boot volume, and hands off to the Finder. *(August 2026)*

---

## PlainTalk Speech Recognition on an Emulated Quadra 840AV (DSP3210)

[![PlainTalk speech recognition running on an emulated Quadra 840AV](https://i.ytimg.com/vi/TZw71SAVR_Q/hq720.jpg)](https://www.youtube.com/watch?v=TZw71SAVR_Q)

Apple's 1993 PlainTalk speech recognition understanding spoken commands - "Computer, open the Trash" opens the Trash. The interesting part is where the work happens: the Quadra 840AV shipped with an on-board AT&T DSP3210, a 32-bit floating-point DSP running alongside the 68040, and PlainTalk was its flagship application. The audio in this demo is processed, sample by sample, by PlainTalk's original DSP code inside a full emulation of that chip. *(August 2026)*

---

## Emulating the Video Input Path of the Macintosh Quadra 840AV

[![Live webcam video digitized by the emulated 840AV](https://i.ytimg.com/vi/zUiRTOQy7RM/hq720.jpg)](https://www.youtube.com/watch?v=zUiRTOQy7RM)

The browser's webcam fed into the emulated 840AV's video digitizer as a live NTSC source - real-time video capture on a 1993 Mac, running in a browser tab. *(August 2026)*

---

## Marathon on a Virtual Macintosh IIfx — 8•24 GC Accelerated

[![Marathon running on an emulated IIfx with 8•24 GC](https://i.ytimg.com/vi/7fpy4xqSDLw/hq720.jpg)](https://www.youtube.com/watch?v=7fpy4xqSDLw)

Bungie's Marathon on an emulated Macintosh IIfx with the Display Card 8•24 GC. The card has no "3D" acceleration, but Marathon renders to an offscreen bitmap and blits each frame with CopyBits — so the emulated QuickDraw acceleration genuinely improves the frame rate. *(July 2026)*

---

## Macintosh Display Card 8•24 GC — with Working Acceleration

[![The 8•24 GC accelerator benchmarked in the emulator](https://i.ytimg.com/vi/Rhv8IiNycAw/hq720.jpg)](https://www.youtube.com/watch?v=Rhv8IiNycAw)

Apple's first "GPU": the 1990 Display Card 8•24 GC, a $1,999 NuBus card with an AMD Am29000 RISC processor that offloaded QuickDraw from the CPU. Here it runs in an emulated IIcx under System 7.1 with unmodified Apple drivers. Rather than emulating the Am29000 itself, the card's drawing engine is implemented natively in the emulator - the command protocol, RPC channel and drawing queue behave exactly like the real firmware's, and the Mac-side software can't tell the difference. Norton System Info's video benchmark runs twice to measure the effect: acceleration off, then on. *(July 2026)*

---

## Benchmarking the Macintosh Display Card 24AC: Acceleration On vs. Off

[![Norton System Info benchmarking the emulated 24AC](https://i.ytimg.com/vi/oXxxA1ID1pA/hq720.jpg)](https://www.youtube.com/watch?v=oXxxA1ID1pA)

The emulated Display Card 24AC running Norton System Info 3.1's video benchmark under System 7.1, with hardware acceleration on and off. Absolute numbers depend as much on the host as on the emulated target, but the relative comparison is what's interesting. *(July 2026)*

---

## Booting Lisa Office System 3.1 on an Emulated Apple Lisa 2

[![Lisa Office System 3.1 on an emulated Lisa 2](https://i.ytimg.com/vi/rYM1fCCsnXI/hq720.jpg)](https://www.youtube.com/watch?v=rYM1fCCsnXI)

The Lisa Office System 3.1 booting on an emulated Apple Lisa 2. *(June 2026)*

---

## Installing Xenix 3.0 from Floppies on an Emulated Lisa 2

[![Xenix 3.0 installation on an emulated Lisa 2](https://i.ytimg.com/vi/IyyMSOJn64Q/hq720.jpg)](https://www.youtube.com/watch?v=IyyMSOJn64Q)

Microsoft's Xenix 3.0 (a UNIX for the Lisa) installed from the original floppy set onto an emulated Lisa 2. *(June 2026)*

---

## Emulated Macintosh XL (Apple Lisa 2) Booting MacWorks XL 3.0

[![MacWorks XL 3.0 booting on an emulated Macintosh XL](https://i.ytimg.com/vi/8BvvyhE_fvU/hq720.jpg)](https://www.youtube.com/watch?v=8BvvyhE_fvU)

MacWorks XL 3.0 turning an emulated Lisa 2 into a Macintosh XL. *(June 2026)*

---

## Emulated Macintosh IIfx Booting A/UX 3.0.1

[![A/UX 3.0.1 booting on an emulated Macintosh IIfx](https://i.ytimg.com/vi/KlJAWr3AL9s/hq720.jpg)](https://www.youtube.com/watch?v=KlJAWr3AL9s)

A/UX 3.0.1 booting on an emulated IIfx — an interesting workload because it's one of the few that exercises the IIfx's bus-master DMA. Where its contemporaries drove the NCR 53C80 SCSI controller by hand, the IIfx pairs it with a dedicated SCSIDMA engine that moves entire transfers to and from memory on its own. *(June 2026)*

---

## Installing A/UX 3.0.1 from Floppy and CD on an Emulated Macintosh SE/30

[![A/UX 3.0.1 installation on an emulated SE/30](https://i.ytimg.com/vi/o0_bxDgY_SI/hq720.jpg)](https://www.youtube.com/watch?v=o0_bxDgY_SI)

The full A/UX 3.0.1 installation on an emulated SE/30: booting from the A/UX boot floppy with the installer CD-ROM as root, partitioning the SCSI disk, and laying down a complete UNIX system. *(May 2026)*

> **Note:** This demo was recorded with an older, now-deprecated UI frontend, so the interface around the emulated screen looks different from current Granny Smith releases.
