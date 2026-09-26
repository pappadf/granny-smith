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
| CPU ROM | The **stored checksum** at `$00` (§2) | None beyond reading the file. The computed sum is checked over `checksum_span` (3 MB of the 4 MB PowerPC images), but a mismatch only prints a warning (`rom_identify_data`, `src/core/memory/rom.c:174-203`) | `ROM_TABLE` (`rom.c:72`): checksum → family name, compatible models, size, span |
| Lisa / Macintosh XL boot ROM | The Mac-style **computed** checksum of the 16 KB interleaved image. The first longword is always the reset SSP `$00000480`, so the stored checksum cannot serve | Exactly 16 KB, the first longword `$00000480`, and a known version word at `$3FFC` (`rom_identify_lisa`, `rom.c:135`) | `LISA_ROM_TABLE` (`rom.c:116`) |
| vROM (NuBus declaration ROM) | The Format Block **CRC**, read from the last 12 bytes of the chip image ([nubus_vrom.md §2](../peripherals/nubus_vrom.md)) | Size 32 KB or 64 KB, and the `$5A932BC7` TestPattern (`vrom_identify_core`, `src/core/memory/vrom.c:113`) | `VROM_CATALOG` (`vrom.c:82`): CRC → card-kind id, plus a `preferred` bit |
| PROM (PCI expansion ROM) | **CRC-32 of the whole chip image** | Power-of-two size between 2 KB and 256 KB, `$55AA`, a `PCIR` structure, Open Firmware code type, and an FCode start token ([pci_prom.md](../peripherals/pci_prom.md#the-gates)) | `PROM_CATALOG` (`src/core/memory/prom.c`): CRC → card-kind id, plus `preferred` |

A CPU ROM does not choose the machine. The Universal ROM, for example,
lists `se30`, `iicx` and `iix`, so the caller names the model and the ROM
only has to be compatible with it (`rom.c:4-12`). The ROMs the emulator
recognises:

| Stored checksum | Compatible models |
|---|---|
| `4D1EEEE1`, `4D1EEAE1`, `4D1F8172` | `plus` |
| `97221136` | `se30`, `iicx`, `iix` |
| `4147DD77` | `iifx` |
| `368CADFE` | `iici` |
| `36B7FB6C` | `iisi` |
| `420DBFF3` | `q700`, `q900` |
| `3DC27823` | `q950` |
| `5BF10FD1` | `q840av`, `q660av` |
| `9FEB69B3` | `pm6100`, `pm7100`, `pm8100` |
| `96CD923D`, `9630C68B` | `pm7500`, `pm8500`, `pm9500` |
| `962F6C13`, `49B2BE8F` | `ans500`, `ans700` |
| `098917B2` (computed) | `lisa` |
| `094C82F0` (computed) | `macxl` |

The production Network Server ROM carries the same version fields as the
9500 v2 ROM. Only the checksum tells the two apart (`rom.c:62-67`).

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
| `machine.rom.identify(path)` | `{recognised, compatible, checksum, name, size}`, with `checksum` as 8 uppercase hex digits (`rom.c:532`) |
| `machine.vrom.identify(path)` | `{recognised, card_id?, compatible?, size, crc}`, with `crc` as `0x` plus 8 lowercase hex digits (`vrom.c:320`) |
| `machine.prom.identify(path)` | `{recognised, card_id?, compatible?, vendor_id?, device_id?, size, crc, reason?}` (`prom.c`, `prom_method_identify`) |

### 10.2 What `rom=` resolves

`machine.boot rom=` (and `rom.load`) take a **filesystem path**. Nothing
searches for it or looks it up in a registry. A relative path resolves
against the process's working directory. At boot the file must be
readable, must identify through `ROM_TABLE`/`LISA_ROM_TABLE`, and must list
the requested model as compatible. The two-chip Lisa form (`rom2=`) skips
the per-file identification (`machine_boot_apply`,
`src/machines/machine.c:873-899`). Once the new machine is constructed,
`rom_load_into_machine` copies the bytes into the ROM region. A file of the
wrong size is truncated or padded, with a warning (`rom.c:376-379`). The
path and the identity checksum go into the built-from record as
`machine.config.rom` and `machine.config.rom_crc` (`machine.c:1107-1108`).

`rom.load(path)` swaps the ROM of the running machine. If the ROM is not
listed as compatible with the model it only warns, then loads anyway,
writes the new path and checksum into the record so `machine.restart`
rebuilds with it, and resets the CPU from the new vectors
(`install_rom_into_machine`, `rom.c:346-410`).

`machine.rom.checksum` is **not** the identity. It is the sum computed
over the whole loaded ROM region (`calculate_checksum`,
`src/core/memory/memory.c:1349`). The two agree only when the stored
checksum covers the whole image, which is not the case for the 4 MB
PowerPC ROMs (span 3 MB). The identity of the loaded ROM is
`machine.config.rom_crc`.

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
| CPU ROM | The `rom=` argument on the command line, which must identify. `model=` picks among the compatible models and otherwise defaults to the first (`headless_main.c:1297-1326`). A script names further ROMs by path in its own `machine.boot rom=`. The integration runner exports the startup path as `$ROM` (`scripts/run-integration-test.sh:154-164`). | Stored in OPFS as `/opfs/images/rom/<checksum>`, named by the 8 uppercase hex digits `rom.identify` reports (`app/web2/src/lib/media.ts:153-156`). The configuration dialog lists that directory and identifies each file. A dropped ROM boots its first compatible model if no machine is running (`maybeBootFromRom`, `app/web2/src/bus/upload.ts:440`). A URL `?rom=` is fetched, stored, and booted, using the URL's `model=` when it is compatible (`app/web2/src/bus/urlMedia.ts:119-134`). |
| vROM | Before the startup boot, every `*.vrom` in the directory of the command-line ROM is offered (`offer_sibling_card_roms`, `headless_main.c:931`, called at `1337`). | Every file in `/opfs/images/vrom/` is offered at startup, whatever its name (`em_main.c:724`). An upload is stored as `<crc>`, 8 lowercase hex digits (`media.ts:180`), and offered at once through `machine.vrom.offer` (`upload.ts:397`). |
| PROM | The same offer pass for `*.prom` (`headless_main.c:946`). | The same, from `/opfs/images/prom/` (`em_main.c:729`; `upload.ts:401`). |

In the browser, a dropped file is classified by trying the
identifiers in the order `rom`, `vrom`, `prom`, `fd`, `cdrom`, `hd` (`upload.ts:304`).
vROM and PROM cannot claim each other's files, because they identify from
opposite ends of the image.

Headless offers card ROMs once, from the directory of the **command-line**
ROM. `machine_boot_apply` never offers anything, so a script that calls
`machine.boot rom=` with a ROM in another directory does not make that
directory's `*.vrom`/`*.prom` visible. It has to offer them
(`machine.vrom.offer`/`machine.prom.offer`) or name one with
`vrom=`/`prom=`.

### 10.5 Fixture naming: `scripts/rom_naming.py`

The ROM fixtures in `tests/data/roms` (a copy of `gs-test-data`; see
[TEST_DATA.md](../../guide/TEST_DATA.md)) are read by people, so they use
a readable canonical grammar keyed on the content identity:

```
<targets>[-<rev>]-<checksum8>.rom          iix-iicx-se30-97221136.rom
<card-id, _ -> ->[-<rev>]-<crc8>.vrom      mdc-8-24-revb-d1629664.vrom
<card-id, _ -> ->[-<rev>]-<crc8>.prom      mach64-gx-104-437584e0.prom
```

The `<targets>` and `<rev>` parts are facts about the hardware or its
history that the bytes cannot supply, so the grammar comes down to one
table, `CANONICAL_NAMES`, mapping a content id to a basename. The content
id is 8 lowercase hex digits: the stored checksum for CPU ROMs (the
computed one for the Lisa/XL), the Format Block CRC for vROMs, and the
whole-image CRC-32 for PROMs. `canonical_name()` also accepts the
`0x`-prefixed and uppercase forms that the identify surfaces emit. The
table's consumers are `scripts/rom-manifest.sh` and the
`tests/integration/rom-naming` conformance row. The emulator never reads
it, and the browser store does not use it either: it names files by the
bare content id.

