# **68k Macintosh ROM Signatures and Identification**

## **1\. Overview**

Early machines (Mac Plus era) used model-specific ROMs identifiable solely by their header checksum. Starting with the Mac II family, Apple introduced "Universal ROMs" shared across multiple models, with runtime hardware probing to distinguish the host machine.

This document covers the ROM header layout, checksum algorithm, per-model identification, and the VIA-based hardware detection used by Universal ROMs. [§10](#10-rom-provisioning) covers how the emulator itself finds and identifies the ROM files it is given, and the card ROMs (vROMs and PCI expansion ROMs) that go with them.

## ---

**2\. ROM Header Layout**

The ROM has no ASCII magic number. The "signature" is the **ROM Checksum** at offset $00. The standard layout for the first 16 bytes:

| Offset (Hex) | Size | Data Type | Description |
| :---- | :---- | :---- | :---- |
| **$00** | 4 Bytes | LongInt | **ROM Checksum** (The "Signature") |
| **$04** | 4 Bytes | Pointer | **Reset PC** (Entry point for boot code) |
| **$08** | 2 Bytes | Word | **ROM Version** (Major) |
| **$0A** | 2 Bytes | Word | **ROM Revision** (Minor) |
| **$0C** | 4 Bytes | LongInt | *Reserved / Internal Use* |

The checksum at offset $00 is unique per ROM build and serves as the primary identifier.

## ---

**3\. Checksum Algorithm**

The checksum is a simple 32-bit "sum of words"—not a CRC or cryptographic hash.

### **3.1 Algorithm**

The ROM is treated as a sequence of 16-bit big-endian words. Each word is added to a 32-bit accumulator (with natural overflow/wrap). The first 4 bytes (the stored checksum itself) are skipped during summation.

### **3.2 Assembly Implementation (P\_ChecksumRom)**

From the Start Manager boot code:

Code snippet

; Register Map:  
; D0 \= Scratch register for reading ROM data  
; D1 \= Accumulator for the calculated sum  
; A0 \= Address Pointer to the ROM  
; D3 \= Loop Counter (Number of words to read)  
; D4 \= Stored Checksum (read from Offset $00)

Start\_Checksum:  
    MoveQ.L  \#0, D0           ; Clear D0  
    MoveQ.L  \#0, D1           ; Clear D1 (The Sum)  
    Lea.L    ($400000), A0    ; Load Base Address of ROM (e.g., $400000)  
      
    ; Step 1: Read the Stored Checksum  
    Move.L   (A0)+, D4        ; Fetch first 4 bytes into D4.   
                              ; (A0 increments by 4, pointing to code start)

    ; Step 2: Initialize Loop Counter  
    ; The value $1FFFE roughly corresponds to a 128KB ROM size in words  
    Move.L   \#$1FFFE, D3      

Checksum\_Loop:  
    Move.W   (A0)+, D0        ; Fetch next 16-bit word into D0  
    Add.L    D0, D1           ; Add word to 32-bit Accumulator D1  
    SubQ.L   \#1, D3           ; Decrement Loop Counter  
    BNE      Checksum\_Loop    ; If D3 is not zero, Branch to Checksum\_Loop

    ; Step 3: The Verification  
    ; At this point, D1 holds the calculated sum of the entire ROM.  
    ; D4 holds the value stamped at Offset $00.  
      
    Eor.L    D4, D1           ; Exclusive OR the Calculated Sum with Stored Sum.  
    BEQ      Checksum\_Passed  ; If result is 0, they match. Jump to Success.

Checksum\_Failed:  
    ; If we fall through here, the ROM is corrupt.  
    ; This triggers the "Sad Mac" routine.  
    Move.L   \#-1, D6          ; Set Error Flag  
    Jmp      Error\_Handler    ; Jump to death chime routine

**Key points:**

1. **Word granularity:** `Move.W (A0)+, D0` — checksum is over 16-bit words.  
2. **Skip stored checksum:** `Move.L (A0)+, D4` reads the checksum and advances A0 past it before the loop.  
3. **Comparison via XOR:** `EOR.L D4, D1` — result is zero if calculated sum matches stored checksum.

### **3.3 Sad Mac on Failure**

A failed checksum triggers the Sad Mac icon. For a Mac Plus, the error code is typically 010004 or similar (class 01 = ROM test failure).

## ---

**4\. Macintosh Plus (Model-Specific ROM Era)**

The Mac Plus uses a 128 KB ROM (two 64 KB chips, "High" and "Low"). The checksum uniquely identifies the model and revision — no other machine shares these ROM images.

**Table 1: Macintosh Plus ROM Revisions**

| Revision | Stored Checksum (Offset $00) | ROM Version (Offset $08) | Common Name | Technical Notes |
| :---- | :---- | :---- | :---- | :---- |
| **Rev 1** | **4D1E EEE1** | $0075 | "Lonely Hearts" | **Bug:** Cannot boot from external SCSI drives if they are powered off at boot. |
| **Rev 2** | **4D1E EAE1** | $0075 | "Loud Metal" | **Fix:** Corrected the SCSI boot bug. This is the most common ROM found in beige Mac Pluses. |
| **Rev 3** | **4D1F 8172** | $0075 | "Platinum" | **Fix:** Additional SCSI improvements. Found in the later Platinum-colored cases. |

All three revisions share ROM Version $0075 at offset $08, so the checksum is the only way to distinguish them.

## ---

**5\. Macintosh IIcx (Universal ROM Era)**

Starting with the Mac II family, Apple shipped a single ROM binary across multiple models.

### **5.1 Ambiguous Checksum**

The IIcx uses a 256 KB ROM with checksum 9722 1136 — the same binary as the Mac IIx and SE/30.  
**Table 2: The Shared "Universal" 256K ROM**

| Stored Checksum (Offset $00) | ROM Size | Compatible Models |
| :---- | :---- | :---- |
| **9722 1136** | 256 KB | **Macintosh IIx Macintosh IIcx Macintosh SE/30** |

**Implication:** If a researcher finds a ROM file with the signature 9722 1136, they **cannot** decide based on the signature alone whether it came from a IIcx, a IIx, or an SE/30. They are binary-identical files.

### **5.2 Hardware Identification via VIA Registers**

Since the ROM is shared, machine identity is determined at runtime by probing VIA port bits. The Universal ROM reads **VIA1 Port A** and **VIA2 Port B** early in StartInit:
**Table 3: Universal ROM Hardware Identification Matrix**

| Detected Model | VIA1 Port A (Bit 6\) | VIA2 Port B (Bit 3\) | Resulting Gestalt ID |
| :---- | :---- | :---- | :---- |
| **Macintosh IIx** | Low (0) | Low (0) | 7 |
| **Macintosh SE/30** | High (1) | Low (0) | 9 |
| **Macintosh IIcx** | **High (1)** | **High (1)** | **8** |

The ROM masks VIA1 Port A bit 6, then VIA2 Port B bit 3, and sets the Gestalt machine ID accordingly.

### **5.3 32-Bit Dirty**

This ROM (9722 1136) is "32-bit dirty" — it uses the upper 8 bits of pointers for flags, preventing native 32-bit mode on the 68030. The IIcx requires the MODE32 extension to run System 7 in 32-bit mode. Compare with the "32-bit clean" IIci ROM (368C ADFE).

## ---

**6\. Comprehensive ROM Signature Reference**

**Table 4: Master 68k Macintosh ROM Signature Table**

| Model Family | ROM Size | Stored Checksum (Offset $00) | ROM Version (Offset $08) | Notes |
| :---- | :---- | :---- | :---- | :---- |
| **Mac 128k / 512k** | 64 KB | 28BA 61CE | $0069 | Original MFS ROM (v1). |
| **Mac 128k / 512k** | 64 KB | 28BA 4E50 | $0069 | Updated Sony Driver (v2). |
| **Mac Plus / 512Ke** | 128 KB | 4D1E EEE1 | $0075 | Rev 1 (Buggy SCSI). |
| **Mac Plus / 512Ke** | 128 KB | 4D1E EAE1 | $0075 | Rev 2 (Standard). |
| **Mac Plus / 512Ke** | 128 KB | 4D1F 8172 | $0075 | Rev 3 (Platinum). |
| **Macintosh SE** | 256 KB | B2E3 62A8 | $0276 | Unique to SE. |
| **Macintosh II** | 256 KB | 9779 D2C4 | $0276 | Original Mac II only. |
| **Mac IIx/IIcx/SE30** | 256 KB | 9722 1136 | $0178 | **Universal Dirty ROM.** |
| **Macintosh Portable** | 256 KB | 96CA 3846 | $067C | Portable / Backlit Portable. |
| **PowerBook 100** | 256 KB | 9664 5F9C | $0178 | Derived from Portable. |
| **Macintosh IIci** | 512 KB | 368C ADFE | $067C | **32-Bit Clean.** |
| **Macintosh IIsi** | 512 KB | 36B7 FB6C | $067C | Similar to IIci but distinct. |
| **Macintosh IIfx** | 512 KB | 4147 DD77 | $067C | "Wicked Fast" ROM. |
| **Macintosh LC** | 512 KB | 350E ACF0 | $067C | LC Series. |
| **Mac Classic** | 512 KB | A49F 9914 | $067C | Includes ROM Disk (XO). |
| **Quadra 700/900** | 1 MB | 420D BFF3 | $077D | First 68040 ROM. |
| **Quadra 660AV/840AV** | 2 MB | 5BF1 0FD1 | $077D | "SuperMario" ROM. |

## ---

**7\. ROM Internal Structure**

### **7.1 Resource Manager**

The ROM is structured as a Resource Manager file containing DRVR (drivers), FONT, CODE (Toolbox segments), CURS, and other resources. Tools like ResForge can open ROM images to inspect `vers` resources for human-readable version strings.

### **7.2 ROM Disk**

Later models (Classic, Portable) embed a bootable disk image in the ROM. The presence of a DRVR resource named `.EDisk` or `.ROMDisk` indicates this capability.

## ---

**8\. Validating a ROM Dump**

### **8.1 Checksum Verification (Python)**

Python

import sys

def validate\_rom(filepath):  
    try:  
        with open(filepath, 'rb') as f:  
            data \= f.read()  
    except FileNotFoundError:  
        print("File not found.")  
        return

    \# 1\. Extract Stored Checksum (First 4 Bytes)  
    \# Mac is Big Endian  
    stored\_checksum \= int.from\_bytes(data\[0:4\], byteorder='big')  
    print(f"Stored Signature: {hex(stored\_checksum).upper()}")

    \# 2\. Calculate Checksum (Sum of 16-bit Words)  
    calculated\_sum \= 0  
      
    \# We iterate over the file in 2-byte chunks (words)  
    \# Note: The actual Apple algorithm usually skips the first 4 bytes   
    \# during the loop, or subtracts them out.  
    \# We will simulate the 'Skip' method by starting at offset 4\.  
      
    for i in range(4, len(data), 2):  
        chunk \= data\[i:i+2\]  
        if len(chunk) \== 2:  
            word \= int.from\_bytes(chunk, byteorder='big')  
            calculated\_sum \= (calculated\_sum \+ word) & 0xFFFFFFFF

    \# 3\. Final Verification (XOR)  
    \# The ROM routine XORs the Sum with the Stored Checksum.   
    \# If the result is 0, they match.   
    \# Therefore, Calculated Sum MUST EQUAL Stored Checksum.  
      
    if calculated\_sum \== stored\_checksum:  
        print("Integrity: VALID")  
    else:  
        print(f"Integrity: INVALID (Calculated {hex(calculated\_sum)})")

if \_\_name\_\_ \== "\_\_main\_\_":  
    validate\_rom(sys.argv)

### **8.2 MD5 for Definitive Identification**

The internal 32-bit checksum is useful but not collision-proof. Use MD5 or SHA hashes for definitive file identification.

## ---

**9\. Summary**

1. The **32-bit checksum at offset $00** is the ROM's primary identifier.  
2. **Mac Plus era:** Checksum uniquely identifies the model and revision.  
3. **Universal ROM era (post-1988):** Checksum identifies the ROM *build*, but the same binary runs on multiple models (e.g., IIx/IIcx/SE30 all use 9722 1136).  
4. **Machine identity** in the Universal era is determined at runtime via VIA register probing, yielding the Gestalt Machine ID.

## 10. ROM provisioning

This section describes how the emulator obtains the ROMs it runs: the CPU
ROM, NuBus declaration ROMs (vROMs), and PCI expansion ROMs (PROMs). One
rule covers all three. **A path is a handle, not a fact.** Core opens a
path it was given and identifies the bytes; it never builds a path of its
own and never reads meaning into a filename. The platform owns the
filesystem and decides which files core sees. Line numbers are as of this
writing; the function names are the stable reference.

### 10.1 Identity is content

| Kind | Identity | Gates before the identity is trusted | Catalog |
|---|---|---|---|
| CPU ROM | The ROM's own **stored checksum field(s)**, read verbatim (§10.1.1) | None beyond reading the file. The self-check (`intact`) is reported, and gates naming: a damaged dump keeps its id but is never stored by it (`rom_identify_data`, `src/core/memory/rom.c`) | `rom_table` (`src/core/memory/rom_table.c`): id → family name, compatible models, size, per-row exceptions |
| vROM (NuBus declaration ROM) | The Format Block **CRC**, read from the last 12 bytes of the chip image ([nubus_vrom.md §2](../peripherals/nubus_vrom.md)) | Size 32 KB or 64 KB, and the `$5A932BC7` TestPattern (`vrom_identify_core`, `src/core/memory/vrom.c:113`) | `VROM_CATALOG` (`vrom.c:82`): CRC → card-kind id, plus a `preferred` bit |
| PROM (PCI expansion ROM) | **CRC-32 of the whole chip image** | Power-of-two size between 2 KB and 256 KB, `$55AA`, a `PCIR` structure, Open Firmware code type, and an FCode start token ([pci_prom.md](../peripherals/pci_prom.md#the-gates)) | `PROM_CATALOG` (`src/core/memory/prom.c`): CRC → card-kind id, plus `preferred` |

A CPU ROM does not choose the machine. The Universal ROM, for example,
lists `se30`, `iicx` and `iix`, so the caller names the model and the ROM
only has to be compatible with it (`rom.c`, file header).

#### 10.1.1 The CPU ROM id and self-check

Every family the emulator knows carries its own checksum. The id is those
stored field(s) as lowercase hex; `intact` is the same checksum recomputed
over the bytes (`identity_eval`, `rom.c`). The kind is a content rule, so
an unknown ROM gets an id and a verdict too:

| Kind | Rule | `id` | Self-check |
|---|---|---|---|
| `lisa` | 16 KB, first longword `$00000480` (the reset SSP) | the check word at `$3FFE`: `3f7b` | the boot ROM's own first diagnostic: add each word from `$0000` and rotate left one bit, up to `$3FFE`, then add the check word; an intact ROM leaves zero (Lisa Boot ROM source, rev 2H, `ROMTST`) |
| `ppc` | exactly 4 MiB | the header sum at `$00`, a dash, the ConfigInfo 64-bit sum: `9feb69b3-dedc602cfc3b9221` | the header sum over `[4, $300000)` (the 68k half) **and** the 64-bit sum of big-endian doublewords over the whole image except the 40-byte ConfigInfo record |
| `mac68k` | anything else with an even size of at least 8 bytes | the header sum at `$00`: `4d1f8172` | the header sum over `[4, size)` (§3) |

The Old World PowerPC ROMs keep, at `$300000 + BE32[$300080]`, a 40-byte
record Apple's source calls `NKConfigurationInfo`: eight 32-bit byte-lane
sums (`ROMCheckSumByte0..7`) followed by the 64-bit sum (`ROMCheckSum64`),
both over the entire image. Apple defined the record in internal source and
never published it. The header sum covers only the 68k half, so it cannot
tell apart two ROMs whose first 3 MiB agree — three Apple Network Server
ROMs share `962F6C13`, and the ANS 2.26B6 ROM shares `9630C68B` with the
Power Macintosh 9500 v2 ROM — nor see damage in the last megabyte. The
64-bit part of the id does both. When it fails, the reason names the byte
lanes whose recomputed sum differs, which narrows a bad dump to a chip.

Why a *stored* label and not a computed one: a dump either passes its
self-check, and then the recomputed sums equal the stored fields by
definition, or it fails, and then nothing is ever named by its id (the
browser refuses it). Under that gate the two schemes agree on everything
that is stored, and the stored label keeps the one thing a damaged dump
still knows — which ROM it was meant to be — so the refusal can say so.

Two known ROMs need a per-row exception, applied only when the id matches
their row: the Classic, whose header sum stops at 256 KB where its built-in
ROM disk starts (`checksum_span`), and the Bandai Pippin ROMs, whose
ConfigInfo sums cover some other range (`ROM_F_NO_SUM64`: only the 68k half
is verified).

#### 10.1.2 The ROM table

`rom_table.c` lists every CPU ROM the emulator knows about — about 135,
from images verified here, a published list of ROM checksum fields, and
published checksum values alone — each with the emulated models it boots.
An empty model list marks a real ROM for a machine that is not emulated:
`rom.identify` reports it `recognised` but not `supported`, `machine.boot`
and headless refuse it by name, and the browser refuses the upload by
name. Adding a machine model means adding its id to the compatible lists
of the ROMs it runs. The supported ROMs:

| Id | Compatible models |
|---|---|
| `4d1eeee1`, `4d1eeae1`, `4d1f8172` | `plus` |
| `97221136` | `se30`, `iicx`, `iix` |
| `4147dd77` | `iifx` |
| `368cadfe` | `iici` |
| `36b7fb6c` | `iisi` |
| `420dbff3` | `q700`, `q900` |
| `3dc27823` | `q950` |
| `5bf10fd1` | `q840av`, `q660av` |
| `9feb69b3-dedc602cfc3b9221` | `pm6100`, `pm7100`, `pm8100` |
| `9b7a3aad-ed510ac937721e25` (newer "Boot PDM 601 1.1" ROM) | `pm7100` |
| `96cd923d-c241cd82bf90797a` (v1), `9630c68b-4db4a42fea3b53b3` (v2) | `pm7500`, `pm8500`, `pm9500` |
| `962f6c13-c60da96de537f08a` (OF 1.1.20.1), `962f6c13-d540b3dd5bcf9caa` (OF 1.1.22), `962f6c13-50348b3d0126096b` (OF 2.26NT), `9630c68b-a71fb907dd180b8a` (OF 2.26B6), `49b2be8f-5f2aeeb25507b2cb` (2.0 prototype, Mac OS) | `ans500`, `ans700` |
| `3f7b` | `lisa` |
| `d905` | `macxl` |

The production Network Server ROM carries the same version fields as the
9500 v2 ROM, so a ROM is never identified by its version: only the full
id tells these apart.

A vROM that is not an Apple dump can still be recognised structurally: an
image produced by one of the emulator's own generic cards is identified by
its `granny-smith` VendorId and BoardId (`vrom.c:158-191`). The generic
kinds generate their declaration ROM when the card is built, and they never
consult the registry ([nubus_generic_vrom.md](../peripherals/nubus_generic_vrom.md)).

The identify surfaces answer from content alone. Each returns `V_ERROR` for
an unreadable path and `recognised: false` for a file that does not
identify:

| Method | Answer |
|---|---|
| `machine.rom.identify(path)` | `{recognised, supported, compatible, name, size, kind, id, intact, reason}` (`rom_method_identify`, `rom.c`). An unrecognised file still reports `kind`, `id`, `intact` and `reason`; `reason` is empty when intact and otherwise names what does not verify ("PowerPC section does not verify (byte lanes 2)") |
| `machine.vrom.identify(path)` | `{recognised, card_id?, compatible?, size, crc}`, with `crc` as `0x` plus 8 lowercase hex digits (`vrom.c:320`) |
| `machine.prom.identify(path)` | `{recognised, card_id?, compatible?, vendor_id?, device_id?, size, crc, reason?}` (`prom.c`, `prom_method_identify`) |

### 10.2 What `rom=` resolves

`machine.boot rom=` (and `rom.load`) take a **filesystem path**. Nothing
searches for it or looks it up in a registry. A relative path resolves
against the process's working directory. At boot the file must be
readable, must identify through `rom_table`, must boot at least one
emulated model, and must list the requested model as compatible. The
two-chip Lisa form (`rom2=`) skips the per-file identification
(`machine_boot_apply`, `src/machines/machine.c`). Once the new machine is
constructed, `rom_load_into_machine` copies the bytes into the ROM region.
A file of the wrong size is truncated or padded, with a warning. A damaged
ROM (not `intact`) still loads, with a warning naming the part that does
not verify: research on damaged or hand-edited images is a legitimate
headless use. The path and the id go into the built-from record as
`machine.config.rom` and `machine.config.rom_id`.

`rom.load(path)` swaps the ROM of the running machine. If the ROM is not
listed as compatible with the model it only warns, then loads anyway,
writes the new path and id into the record so `machine.restart`
rebuilds with it, and resets the CPU from the new vectors
(`install_rom_into_machine`, `rom.c`).

`machine.rom.id`, `machine.rom.intact` and `machine.rom.name` describe the
loaded ROM by running the same identification over the ROM region, so
`rom.id` always equals what `rom.identify` said about the file and what
`machine.config.rom_id` records.

The Lisa two-chip loader (`rom_load_lisa_pair`) picks the high/low chip
orientation whose interleave passes the boot ROM's own self-check; the check
covers both chips, so a swapped pair never passes it.

### 10.3 Card ROMs: the offer registry

Declaration ROMs and expansion ROMs are not named in the boot document by
default. The platform **offers** candidate files, and a card factory asks
for the ROM of its card kind. The registry (`src/core/memory/offer_registry.c`)
has two instances, `vrom.c` and `prom.c`, with the same behaviour:

- **Registration by content.** `offer_registry_add` runs the kind's
  identifier. A file that is not recognised is dropped with a log, since
  strays are expected when a whole directory is offered. The registry
  keeps one entry per content id, and offering the same bytes again
  refreshes the stored path (`offer_registry.c:19-70`).
- **Pick order.** First the explicit pick, then catalog rows marked
  `preferred`, then the remaining catalog rows in order. No filename ever
  enters the comparison (`offer_registry_find`, `offer_registry.c:83`). A
  card loader tries the candidates in that order and takes the first that
  lays out cleanly (`declrom_load_vrom_card`,
  `src/core/peripherals/nubus/declrom.c:956`; `prom_load_card`). Its
  choice is recorded in `machine.config.vroms`.
- **Explicit pick.** `machine.boot vrom=`/`prom=` identifies the file and
  offers it as the explicit pick (`vrom_set_path` / `prom_set_path`;
  `machine.c:964-984`). A registry holds one explicit pick at a time, and
  the pick belongs to that boot document only: every boot first clears the
  previous one (`machine_config_set_explicit_picks`), so a later boot that
  omits `vrom=` resolves by catalog order again. A boot rejected before
  teardown puts the running machine's pick back.
- **Strict resolution.** A card the user picked explicitly that finds no
  offer fails the boot before teardown. A default card degrades to an
  empty slot, and the SE/30's onboard video synthesises a fallback ROM
  (`machine.c:737-798`;
  `src/machines/glue/builtin_se30_video.c:163`). See
  [object-model.md, Boot arguments](../shell/object-model.md#boot-arguments).
- **Lifetime.** The registries are process-global. Offers survive
  `machine.boot`, `machine.restart` and `checkpoint.load`, and are dropped
  only by `vrom_delete`/`prom_delete`. The card ROMs themselves are not
  checkpointed; on restore they are resolved again from the registry, with
  the record's `vrom` and `prom` made the explicit picks again
  (`system_restore`).
- **Hooks.** `machine.vrom.offer(path)` and `machine.prom.offer(path)`
  register one file and return `true` only if it was recognised.

### 10.4 Where the files come from

| | Headless (`src/platform/headless/headless_main.c`) | Browser (`app/web2`, `src/platform/wasm/em_main.c`) |
|---|---|---|
| CPU ROM | The `rom=` argument on the command line, which must identify. `model=` picks among the compatible models and otherwise defaults to the first (`headless_main.c:1297-1326`). A script names further ROMs by path in its own `machine.boot rom=`. The integration runner exports the startup path as `$ROM` (`scripts/run-integration-test.sh:154-164`). | Stored in OPFS as `/opfs/images/rom/<id>`, named by the content id `rom.identify` reports, and only when the ROM is `intact` and `supported`: a damaged dump or a ROM of an unemulated machine is refused with a message saying which (`app/web2/src/lib/media.ts`, the `rom` descriptor). The configuration dialog lists that directory and identifies each file. A dropped ROM boots its first compatible model if no machine is running (`maybeBootFromRom`, `app/web2/src/bus/upload.ts:440`). A URL `?rom=` is fetched, stored, and booted, using the URL's `model=` when it is compatible (`app/web2/src/bus/urlMedia.ts:119-134`). |
| vROM | Before the startup boot, every `*.vrom` in the directory of the command-line ROM is offered (`offer_sibling_card_roms`, `headless_main.c`), and `machine_boot_apply` repeats the walk for the directory of every ROM a script boots, through `platform_offer_sibling_card_roms` (platform.h; the browser's implementation is a no-op) — a `machine.boot rom=<elsewhere>` used to find none of the card ROMs beside it (#187). | Every file in `/opfs/images/vrom/` is offered at startup, whatever its name (`em_main.c:724`). An upload is stored as `<crc>`, 8 lowercase hex digits (`media.ts:180`), and offered at once through `machine.vrom.offer` (`upload.ts:397`). |
| PROM | The same offer pass for `*.prom` (`headless_main.c:946`). | The same, from `/opfs/images/prom/` (`em_main.c:729`; `upload.ts:401`). |

In the browser, a dropped file is classified by trying the
identifiers in the order `rom`, `vrom`, `prom`, `fd`, `cdrom`, `hd` (`upload.ts`, `probeAndPersist`).
A descriptor that recognises the file but refuses it (`reject`) ends the
probe with that message, so a refused ROM is never stored as a disk image.
vROM and PROM cannot claim each other's files, because they identify from
opposite ends of the image.

Headless offers card ROMs once, from the directory of the **command-line**
ROM. `machine_boot_apply` never offers anything, so a script that calls
`machine.boot rom=` with a ROM in another directory does not make that
directory's `*.vrom`/`*.prom` visible. It has to offer them
(`machine.vrom.offer`/`machine.prom.offer`) or name one with
`vrom=`/`prom=`.

### 10.5 Test-data filenames

The ROM fixtures in `tests/data/roms` (a copy of `gs-test-data`; see
[TEST_DATA.md](../../guide/TEST_DATA.md)) have readable names such as
`iix-iicx-se30-97221136.rom`, but the names are labels for the people who
write `TEST_ROM :=`: nothing parses them and nothing checks them against a
grammar. What a file is comes from the identify surfaces alone. The
`tests/integration/rom-catalog` row checks that every file there is
recognised, and every CPU ROM intact and supported; `scripts/rom-manifest.sh`
and `scripts/test-matrix.py` ask `gs-headless` rather than reading
filenames. Only the browser store gives a ROM a canonical name, its id.
