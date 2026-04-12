# **68k Macintosh ROM Signatures and Identification**

## **1\. Overview**

Early machines (Mac Plus era) used model-specific ROMs identifiable solely by their header checksum. Starting with the Mac II family, Apple introduced "Universal ROMs" shared across multiple models, with runtime hardware probing to distinguish the host machine.

This document covers the ROM header layout, checksum algorithm, per-model identification, and the VIA-based hardware detection used by Universal ROMs.

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