# Macintosh IIsi

The Macintosh IIsi is a 20 MHz Motorola MC68030 Macintosh II-family machine. Its architecture is based on two custom chips also used conceptually in the Macintosh IIci design:

- **MDU - Memory Decode Unit**: address decoding, RAM control, ROM mapping behavior, RAM burst support, refresh, and some I/O acknowledge behavior.
- **RBV - RAM-Based Video**: on-board video using system DRAM as the frame buffer, plus virtual VIA2 functions.

Other important custom or semi-custom devices:

- **Combo SCC/SCSI chip**: combines SCC-compatible serial and NCR 53C80-compatible SCSI functions.
- **SWIM floppy controller**: controls the internal SuperDrive and external floppy port.
- **ASC - Apple Sound Chip**: Macintosh II-family compatible sound output and four-voice synthesis.
- **Sound input hardware**: mono 8-bit digitizing path with FIFO and interrupt-driven control logic.
- **Custom 68HC05 ADB microcontroller**: ADB controller, real-time clock, parameter RAM, soft power, reset, NMI, wakeup, and network boot flag support.
- **Optional 68882 FPU**: not on the base logic board. Present only when the user installs either the PDS adaptor or NuBus adaptor; both adaptor kits include a 20 MHz MC68882.

The system was introduced with System 6.0.6 support and was intended to be supported by System 7.0. The ROM is described as a universal Macintosh II-family ROM with IIsi-specific startup hardware probing and support code.

## 2. CPU and execution model

- CPU: **Motorola MC68030**, 20 MHz.
- Internal caches: **256-byte instruction cache** and **256-byte data cache**.
- On-chip MMU is used for memory management and to make discontiguous physical RAM appear contiguous to software.
- The MDU supports MC68030 **burst reads** from RAM into the cache.
- Base system does **not** include an FPU; software must probe for it instead of assuming one because a 68030 is present.

### Emulator implications

- Emulate a 68030-class MMU environment, not a 68020 + external PMMU/HMMU machine.
- The IIsi ROM expects to discover RAM size at startup, then program the MMU to construct logical memory from physical Bank A and Bank B.
- FPU should be configurable: absent in a base model, present when an expansion adaptor with 68882 is installed.

## 3. Physical address map

The MDU changes the low physical address map after ROM has first been accessed at its true ROM space.

### 3.1 Power-up physical map

At reset, ROM is selected for addresses `$0000_0000` through `$3FFF_FFFF`. This allows the reset vectors fetched by the 68030 from address zero to come from ROM.

| Physical range | Meaning at power-up |
|---:|---|
| `$0000_0000` - `$3FFF_FFFF` | ROM images, including duplicate/mirrored ROM image at the bottom of memory |
| `$4000_0000` - `$4FFF_FFFF` | True ROM space and reserved ROM space |
| `$5000_0000` - `$52FF_FFFF` | I/O devices |
| `$5300_0000` - `$5FFF_FFFF` | Expansion I/O space |
| `$6000_0000` - `$EFFF_FFFF` | NuBus super-slot space |
| `$F000_0000` - `$F0FF_FFFF` | Reserved; no device assigned, slots 4-6 |
| `$F100_0000` - `$FFFF_FFFF` | NuBus slot space |

Trigger for normal map: **the first access to `$4000_0000` - `$4FFF_FFFF` causes the MDU to impose the normal map**. The only documented change is that `$0000_0000` - `$3FFF_FFFF` stops selecting ROM and selects RAM/reserved RAM instead.

### 3.2 Normal physical map

| Physical range | Size / notes | Meaning in normal map |
|---:|---:|---|
| `$0000_0000` upward | variable | Video screen buffer begins at physical zero when on-board video is active |
| `$0000_0000` - `$03FF_FFFF` | 64 MB window | **RAM Bank A**: soldered logic-board RAM; contains video frame buffer at bottom when on-board video is used |
| `$0400_0000` - `$07FF_FFFF` | 64 MB window | **RAM Bank B**: SIMM expansion RAM |
| `$0800_0000` - `$3FFF_FFFF` | reserved | Reserved RAM space |
| `$4000_0000` - `$4FFF_FFFF` | 256 MB region | ROM region plus reserved ROM space; actual 512 KB ROM is at start and mirrored through configured ROM-size aliases |
| `$4000_0000` - `$4007_FFFF` | 512 KB | Actual base ROM image size documented for production machines |
| `$4008_0000` | 1 MB boundary | ROM-size alias boundary shown in physical map |
| `$4010_0000` | 2 MB boundary | ROM-size alias boundary shown in physical map |
| `$4020_0000` | 8 MB boundary | ROM-size alias boundary shown in physical map |
| `$4080_0000` | 32 MB boundary | ROM-size alias boundary shown in physical map |
| `$4200_0000` - `$4FFF_FFFF` | reserved | Reserved ROM space above ROM image/aliases |
| `$5000_0000` - `$52FF_FFFF` | 48 MB | I/O devices, with repeated/decoded subranges described below |
| `$5300_0000` - `$5FFF_FFFF` | 208 MB | Expansion I/O space |
| `$6000_0000` - `$EFFF_FFFF` | 2.25 GB | NuBus super-slot space |
| `$F000_0000` - `$F0FF_FFFF` | 16 MB | Reserved; no device assigned; slots 4-6 |
| `$F100_0000` - `$FFFF_FFFF` | 240 MB | NuBus slot space |

### 3.3 RAM bank layout and mirroring

Each RAM bank is decoded into a fixed 64 MB physical window:

| Bank | Physical window | Hardware contents |
|---|---:|---|
| Bank A | `$0000_0000` - `$03FF_FFFF` | Logic-board RAM, fixed 1 MB soldered in base design |
| Bank B | `$0400_0000` - `$07FF_FFFF` | Four SIMM sockets, optional expansion RAM |

If a bank contains less than 64 MB, the existing RAM wraps/mirrors throughout that 64 MB window. The ROM uses this wrapping behavior to determine installed memory size.

Important note: the source text gives an example for Bank A with 1 MB soldered RAM, but appears to contain a typographical error in the second-image address. The intended behavior is that 1 MB Bank A RAM at `$0000_0000` - `$000F_FFFF` repeats at 1 MB intervals through `$03FF_FFFF`.

## 4. Logical address spaces and 24-bit compatibility mapping

The memory map is designed to support both old 24-bit addressing software and 32-bit software. The MMU creates logical mappings over the physical address space.

### 4.1 32-bit logical address space overview

| 32-bit logical range | Meaning |
|---:|---|
| `$0000_0000` - `$3FFF_FFFF` | RAM logical space |
| `$4000_0000` - `$4FFF_FFFF` | ROM logical space |
| `$5000_0000` - `$5FFF_FFFF` | I/O space |
| `$6000_0000` - `$EFFF_FFFF` | NuBus super-slot logical space |
| `$F000_0000` - `$F0FF_FFFF` | Reserved |
| `$F100_0000` - `$FFFF_FFFF` | NuBus slot logical space |

### 4.2 24-bit to 32-bit mapping mode

The developer note explicitly lists this translation table:

| Usage | 24-bit address range | 32-bit address range |
|---|---:|---:|
| RAM | `$xx00_0000` - `$xx7F_FFFF` | `$0000_0000` - `$07FF_FFFF` |
| ROM | `$xx80_0000` - `$xx8F_FFFF` | `$4000_0000` - `$400F_FFFF` |
| NuBus / 030 slot, NuBus address `$9` | `$xx90_0000` - `$xx9F_FFFF` | `$F900_0000` - `$F90F_FFFF` |
| 030 slot, NuBus address `$A` | `$xxA0_0000` - `$xxAF_FFFF` | `$FA00_0000` - `$FA0F_FFFF` |
| 030 slot, NuBus address `$B` | `$xxB0_0000` - `$xxBF_FFFF` | `$FB00_0000` - `$FB0F_FFFF` |
| Not used | `$xxC0_0000` - `$xxCF_FFFF` | `$FC00_0000` - `$FC0F_FFFF` |
| Not used | `$xxD0_0000` - `$xxDF_FFFF` | `$FD00_0000` - `$FD0F_FFFF` |
| On-board video, NuBus address `$E` | `$xxE0_0000` - `$xxEF_FFFF` | `$FE00_0000` - `$FE0F_FFFF` |
| I/O space | `$xxF0_0000` - `$xxFF_FFFF` | `$5000_0000` - `$500F_FFFF` |

### Emulator implications

- The on-board video frame buffer is physically in Bank A, but the OS maps it into **NuBus super-slot logical address `$E`**, i.e. `$FE00_0000` range in 32-bit addressing and `$xxE0_0000` in 24-bit compatibility mapping.
- A PDS expansion card should be modeled as occupying pseudo-NuBus address spaces `$F900_0000` - `$FBFF_FFFF`, corresponding to geographic pseudo-slots `$9`, `$A`, and `$B`.
- A NuBus card via the IIsi NuBus adaptor is mapped as slot/geographic address `$9`.

## 5. I/O address map

The I/O range is `$5000_0000` - `$5FFF_FFFF`. Figure 5-1 gives the detailed I/O decode. Only the bold entries in the original figure are used on the IIsi, code-named "Ray Ban" in the figure note.

### 5.1 Top-level complete map

| Physical range | Meaning |
|---:|---|
| `$0000_0000` - `$3FFF_FFFF` | RAM |
| `$4000_0000` - `$4FFF_FFFF` | ROM |
| `$5000_0000` - `$5FFF_FFFF` | I/O devices |
| `$6000_0000` - `$FFFF_FFFF` | NuBus |

### 5.2 Detailed I/O decode

| Address / range marker | Meaning |
|---:|---|
| `$5000_0000` | VIA1 |
| `$5000_2000` | Reserved |
| `$5000_4000` | SCC |
| `$5000_6000` | SCSI pseudo-DMA with DRQ |
| `$5000_8000` - below `$5001_0000` | Reserved |
| `$5001_0000` | SCSI normal mode |
| `$5001_2000` | SCSI pseudo-DMA with no DRQ |
| `$5001_4000` | Sound |
| `$5001_6000` | SWIM floppy controller |
| `$5001_8000` - below `$5002_4000` | Reserved |
| `$5002_4000` | VDAC |
| `$5002_6000` | RBV |
| `$5002_8000` - below `$5004_0000` | Reserved |
| `$5004_0000` - below `$5100_0000` | Reserved; repeated image of `$5000_0000` - `$5003_FFFF` |
| `$5100_0000` - below `$5800_0000` | Reserved |
| `$5800_0000` - below `$5900_0000` | Factory test space |
| `$5900_0000` - below `$6000_0000` | Reserved |

MDU provides DSACK for the I/O subranges shown in Figure 5-1. For emulator purposes, the important effect is that accesses in these decoded spaces complete even when the target is an MDU-acknowledged or reserved decoded area, rather than necessarily behaving like absent memory.

## 6. RAM subsystem

### 6.1 Installed RAM configurations

The logic board supports 1 MB to 65 MB total RAM:

- Bank A: fixed 1 MB soldered RAM on the logic board, made from eight 256K x 4 fast-page-mode DRAMs.
- Bank B: four 30-pin SIMM sockets.

Documented configurations:

| Total RAM | Bank A | Bank B |
|---:|---:|---|
| 1 MB | 1 MB soldered | Empty |
| 2 MB | 1 MB soldered | Four 256 KB SIMMs |
| 3 MB | 1 MB soldered | Four 512 KB SIMMs |
| 5 MB | 1 MB soldered | Four 1 MB SIMMs |
| 9 MB | 1 MB soldered | Four 2 MB SIMMs |
| 17 MB | 1 MB soldered | Four 4 MB SIMMs |
| 65 MB | 1 MB soldered | Four 16 MB SIMMs |

The text also states that Bank B can contain four 256 KB, four 1 MB, four 4 MB, or four 16 MB SIMMs, while the figure additionally lists four 512 KB and four 2 MB SIMM configurations. For an emulator, all the listed total sizes above are useful presets.

### 6.2 RAM performance behavior

- RAM uses `/STERM` synchronous termination.
- Burst read mode: **5-clock initial access followed by three 2-clock accesses**.
- Random RAM write: **4-clock minimum access**; may be 5 clocks when delayed by a preceding write cycle.
- Random RAM read: **5-clock minimum access**; may be 6 clocks when delayed by a preceding write.
- PDS bus-master RAM access differs from SE/30: bus master cards must observe `/STERM`, not only `/DSACK`.

### 6.3 Refresh behavior

- Refresh is performed by the MDU using `/CAS` before `/RAS` cycles.
- Refresh period: **15.6 microseconds**.
- Refresh cycles are **six CPU clocks long**.
- Refresh is initiated at the same time in both banks but continues independently per bank.
- Refresh does not affect the processor unless the processor is accessing RAM.

### Emulator implications

A functional emulator can usually ignore DRAM refresh as long as RAM contents persist. A cycle-accurate emulator should account for refresh-induced stalls only for RAM accesses, and independently for Bank A vs Bank B if modeling contention.

## 7. ROM subsystem

- Base ROM size: **512 KB**.
- Early production units use a 512 KB ROM SIMM; later units use a soldered 4-Mbit ROM device.
- ROM SIMM socket exists for future ROM revision/expansion.
- ROM access time: **five CPU clock cycles**.
- The MDU does **not** support burst reads in ROM space.
- On power-up, ROM is temporarily selected at low memory until the first true ROM-space access.

### Emulator implications

- Map the ROM image both at reset-vector address zero during the initial power-up map and at true ROM address `$4000_0000`.
- Switch to the normal map after the first access in `$4000_0000` - `$4FFF_FFFF`.
- Model ROM aliases/mirrors as needed; the map shows ROM-size alias boundaries at 512 KB, 1 MB, 2 MB, 8 MB, and 32 MB within the ROM region.

## 8. On-board video

The IIsi includes on-board RAM-based video. It can also use NuBus or PDS video cards.

### 8.1 Supported monitor modes

| Monitor ID bits `ID3 ID2 ID1` | Monitor selected | Active resolution | Max bit depth | Dot clock | Line rate | Frame rate |
|---|---|---:|---:|---:|---:|---:|
| `0 0 0` | Unsupported | - | - | - | - | video halted |
| `0 0 1` | 15-inch B&W Portrait | 640 x 870 | 4 bpp / 16 grays | 57.2832 MHz | 68.850 kHz | 75 Hz |
| `0 1 0` | 12-inch RGB | 512 x 384 | 8 bpp / 256 colors | 15.6672 MHz | 24.48 kHz | 60.15 Hz |
| `0 1 1` | Unsupported | - | - | - | - | video halted |
| `1 0 0` | Unsupported | - | - | - | - | video halted |
| `1 0 1` | Reserved for Apple | - | - | - | - | reserved |
| `1 1 0` | Macintosh II 12-inch B&W or 13-inch RGB | 640 x 480 | 8 bpp / 256 colors or grays | 30.2400 MHz | 35.0 kHz | 66.67 Hz |
| `1 1 1` | No external monitor | - | - | - | - | video halted |

The monitor asserts ID bits by grounding lines for zeroes and leaving lines unconnected for ones. Unknown or absent monitor configurations halt on-board video.

### 8.2 Video memory organization

- On-board video uses **Bank A** DRAM as its frame buffer.
- The frame buffer starts at **physical `$0000_0000`**.
- The RBV only uses the amount of memory required for the current screen size and bit depth.
- At startup, software chooses the maximum bit depth to reserve. If the user later selects a smaller bit depth, system software may reuse the extra space.
- The OS maps this physical frame-buffer region into logical NuBus super-slot `$E`, so it behaves like a NuBus video device.
- The RBV itself does not understand screen addresses or mapping. It requests data bursts; the MDU increments an internal pointer through the frame buffer and resets it to physical `$0000_0000` at the end of a screen.

### 8.3 Video DMA and CPU contention

- RBV and Bank A share a separate RAM data bus that can be disconnected from the CPU data bus through bus buffers.
- During active display, RBV requests data as needed.
- The MDU performs an **eight-longword DMA burst read** from Bank A into the RBV FIFO.
- If a CPU access to Bank A occurs during a video burst, the CPU access is delayed.
- The slowdown is worse for larger displays and higher bit depths.
- Only Bank A CPU accesses are affected. Bank B, ROM, and I/O accesses are not blocked by RBV video fetches.

### 8.4 Timing details by monitor

#### 13-inch RGB and 12-inch B&W

- Active video: 640 dots x 480 lines.
- Full line: 864 dots, 28.57 microseconds.
- Horizontal blanking: 224 dots.
- Horizontal back porch: 96 dots.
- Horizontal sync pulse: 64 dots.
- Horizontal front porch: 64 dots.
- Full frame: 525 lines, 15.00 milliseconds.
- Vertical blanking: 45 lines.
- Vertical back porch: 39 lines.
- Vertical sync pulse: 3 lines.
- Vertical front porch: 3 lines.
- Dot clock: 30.2400 MHz +/- 0.1%.

#### 15-inch B&W Portrait

- Active video: 640 dots x 870 lines.
- Full line: 832 dots, 14.52 microseconds.
- Horizontal blanking: 192 dots.
- Horizontal back porch: 80 dots.
- Horizontal sync pulse: 80 dots.
- Horizontal front porch: 32 dots.
- Full frame: 918 lines, 13.33 milliseconds.
- Vertical blanking: 48 lines.
- Vertical back porch: 42 lines.
- Vertical sync pulse: 3 lines.
- Vertical front porch: 3 lines.
- Dot clock: 57.2832 MHz +/- 0.1%.

#### 12-inch RGB

- Active video: 512 dots x 384 lines.
- Full line: 640 dots, 40.85 microseconds.
- Horizontal blanking: 128 dots.
- Horizontal back porch: 80 dots.
- Horizontal sync pulse: 32 dots.
- Horizontal front porch: 16 dots.
- Full frame: 407 lines, 16.626 milliseconds.
- Vertical blanking: 23 lines.
- Vertical back porch: 19 lines.
- Vertical sync pulse: 3 lines.
- Vertical front porch: 1 line.
- Dot clock: 15.6672 MHz +/- 0.1%.

## 9. VIA, RBV, and interrupts

The IIsi maintains compatibility with existing Macintosh software through:

- A physical **VIA1**.
- A **virtual VIA2** implemented by RBV circuitry.

VIA2 functions provided by the RBV include:

- Decoding expansion slot interrupts.
- Handling two SCSI interrupts.
- Handling the sound subsystem interrupt.
- Blocking NuBus accesses to RAM.
- Decoding NuBus transaction errors.

Several VIA1 bits were redefined so ROM can distinguish between different machines. The developer note does not provide a complete VIA register map; it assumes existing Macintosh II-family hardware documentation for standard VIA behavior.

## 10. Serial and SCSI - Combo chip

The IIsi uses a custom **Combo** chip that combines SCC and SCSI.

### 10.1 SCC

- Software compatible with the SCC 85C30 it replaces.
- Provides two serial ports.
- Each port can be independently programmed for asynchronous, synchronous, or AppleTalk protocols.
- Uses the same Macintosh 8-pin serial connector style as other Macintosh II-family computers.

### 10.2 SCSI

- Software compatible with the 53C80 SCSI controller used in other Macintosh II-family machines.
- Supports the ANSI X3T9.2 SCSI interface.
- Provides internal 50-pin SCSI connector for an internal hard drive and external DB-25 SCSI connector.
- The I/O map includes normal SCSI mode and two pseudo-DMA regions: with DRQ and with no DRQ.

### Emulator implications

- Treat the Combo chip as SCC 85C30-compatible plus NCR 53C80-compatible at the documented I/O addresses.
- Software that uses standard drivers should work; direct hardware access expects the IIsi address map.

## 11. Floppy subsystem

- Controller: **SWIM**.
- Controls one internal 1.4 MB 3.5-inch SuperDrive.
- Supports one external 800 KB floppy drive or one external 1.4 MB SuperDrive.
- The external floppy port does **not** support the 400 KB floppy disk drive, but it does support 400 KB disks used in an 800 KB drive.
- Signal interface is identical to other Macintosh II-family SWIM-based implementations.

### Emulator implications

Emulate a SWIM-compatible floppy controller. The IIsi-specific note mainly affects supported external drive types, not disk-image formats.

## 12. Sound output and sound input

### 12.1 Sound output

- Uses the **Apple Sound Chip (ASC)**.
- Macintosh II-family compatible sound output.
- Supports four-voice synthesis in hardware.
- Uses Sony sound chips for filtering and output drive, as on the Macintosh II, IIx, IIcx, and IIci.

### 12.2 Sound input

The IIsi adds built-in mono sound input:

- Input is digitized as **8-bit monaural** audio.
- Hardware path includes input jack, audio filter/preamplifier, FIFO buffer, and control logic.
- Sound input is interrupt-driven and buffered by a large FIFO, reducing CPU bandwidth needs compared with external solutions.
- Low-level ROM routines support sound input.
- Sound Input Manager API presents high-level and low-level software interfaces. The developer note refers to Inside Macintosh Volume VI for details.

### Emulator implications

- For basic emulation, ASC output compatibility is more important than analog details.
- If supporting IIsi-specific sound input, emulate a mono 8-bit capture device with FIFO and interrupt behavior at the sound I/O address range.
- The developer note does not provide full sound input register definitions.

## 13. ADB microcontroller, RTC, PRAM, power, and reset

The IIsi uses a custom 68HC05-family ADB microcontroller that integrates functions previously implemented with multiple chips.

### 13.1 Integrated functions

- ADB controller.
- Real-time clock.
- Parameter RAM.
- Soft power control.
- Power-on reset.
- Keyboard reset.
- Keyboard NMI.
- Programmable wakeup.
- File-server style power recovery and network boot flag support.

### 13.2 ADB

- ADB is a single-master, multiple-slave asynchronous serial bus.
- The IIsi supports the standard keyboard and mouse and allows additional input devices.
- A maximum of three chained ADB devices is supported.
- Applications that use the ADB Manager should work. Software that directly addresses old ADB hardware may fail.

### 13.3 RTC and PRAM

- The microcontroller contains a 32-bit counter similar in behavior to the RTC chip in other Macintosh II-family machines.
- Battery or trickle power keeps the counter and PRAM alive while off or unplugged.
- RTC and PRAM access differs from previous Macintosh models and is performed through modified ADB-style commands.
- Existing driver routines should remain compatible; direct hardware access to older RTC/PRAM chips will not.

### 13.4 Power control

- The ADB microcontroller polls keyboard power and rear-panel power inputs using +5 V trickle power while the main machine is off.
- Pressing a power switch raises the PFW signal and turns the power supply on within about two seconds.
- Shutdown is software controlled: the OS sends a command that lets the microcontroller pull PFW low, shutting down the supply after pending activity is complete.
- Pressing the rear-panel power switch while the machine is on causes a hard-off after about two milliseconds without notifying software.
- If the rear-panel switch is locked on, the system automatically powers back on after AC power loss, useful for file-server mode.
- Software can program automatic power-on.

### 13.5 Reset and NMI

- No external programmer reset or NMI switches are present.
- NMI sequence: **Command + power button**, held at least one second.
- Hard reset sequence: **Command + Control + power button**, held at least one second.
- NMI is disabled by default and must be enabled using a control panel device.
- Hard reset is equivalent to a power-on reset.
- On power-up, the microcontroller asserts Reset and Test. Test resets the MDU earlier/with a shorter time constant; Reset goes to the 68030 and other I/O devices after the processor stabilizes.

### 13.6 Network booting and wakeup

- A control panel can set a microcontroller flag to boot from a communications network such as Ethernet or LocalTalk.
- A programmable wakeup time can cause automatic power-up at a specified time.

### Emulator implications

- Model RTC/PRAM access through the IIsi ADB microcontroller mechanism if emulating ROM-level behavior.
- Support PRAM persistence and a 32-bit RTC-like counter.
- Implement keyboard reset/NMI sequences if emulating user input at that level.
- Power-control behavior can be simplified unless boot/shutdown accuracy is important.

## 14. Expansion architecture

The IIsi has one 120-pin expansion connector on the logic board. A user-installable adaptor selects one of two expansion modes:

- 68030 Direct Slot / PDS adaptor.
- NuBus adaptor.

Both adaptors include a 20 MHz 68882 FPU.

### 14.1 Processor-direct slot mode

- Intended to support existing Macintosh SE/30 PDS cards, provided they work at the IIsi's 20 MHz clock.
- New or revised cards should use 32-bit addressing.
- Electrical and functional PDS interface is identical to the Macintosh SE/30 68030 Direct Slot, with one important difference for RAM bus-master cycles: the IIsi uses MDU-generated `/STERM` for burst transfers, not only `/DSACK`.
- PDS cards can occupy 32-bit addresses `$F900_0000` - `$FBFF_FFFF`.
- These correspond to geographic NuBus locations `$9`, `$A`, and `$B`.
- Apple recommends pseudo-slot design with declaration ROM and interrupt capability so the Slot Manager can manage the PDS card similarly to a NuBus card.

### 14.2 NuBus adaptor mode

- Allows installation of one standard NuBus card.
- Functionally equivalent to NuBus in other Macintosh II-family machines.
- The IIsi has only one NuBus slot.
- The NuBus slot maps to geographic address `$9`.
- The address mapping difference is transparent to NuBus cards.
- The NuBus adaptor contains interface logic including NuChip 30 and transceivers in addition to the FPU.
